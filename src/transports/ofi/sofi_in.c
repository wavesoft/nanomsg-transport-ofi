/*
    Copyright (c) 2015 Ioannis Charalampidis  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "ofi.h"
#include "hlapi.h"
#include "sofi_in.h"

#include "../../utils/alloc.h"
#include "../../utils/cont.h"
#include "../../utils/err.h"
#include "../../utils/fast.h"
#include "../../utils/queue.h"

/* FSM States */
#define NN_SOFI_IN_STATE_IDLE           2001
#define NN_SOFI_IN_STATE_READY          2002
#define NN_SOFI_IN_STATE_RECEIVING      2003
#define NN_SOFI_IN_STATE_ACKNOWLEGING   2004
#define NN_SOFI_IN_STATE_CLOSED         2005
#define NN_SOFI_IN_STATE_ABORTING       2006
#define NN_SOFI_IN_STATE_ABORT_CLEANUP  2007
#define NN_SOFI_IN_STATE_ABORT_TIMEOUT  2008

/* FSM Sources */
#define NN_SOFI_IN_SRC_TIMER            2101
#define NN_SOFI_IN_SRC_TASK_RX          2102
#define NN_SOFI_IN_SRC_TASK_RX_ERROR    2103
#define NN_SOFI_IN_SRC_TASK_RX_ACK      2104

/* Timeout values */
#define NN_SOFI_IN_TIMEOUT_ABORT        1000

/* Incoming MR flags */
#define NN_SOFI_IN_MR_FLAG_POSTED       0x00000001
#define NN_SOFI_IN_MR_FLAG_LOCKED       0x00000002

/* State change with mutex lock */
#define NN_SOFI_IN_SET_STATE(self,val) self->state = val;
    // nn_mutex_lock( &(self)->mutex_ingress ); \
    // nn_mutex_unlock( &(self)->mutex_ingress );

/* Forward Declarations */
static void nn_sofi_in_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_in_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);

/* ============================== */
/*       HELPER FUNCTIONS         */
/* ============================== */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* BEGIN DANGEROUPS REGION                                                   */
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "../../utils/atomic.h"
#include "../../utils/wire.h"

/* Local copy of private API of `chunk.c` */
typedef void (*nn_ofi_chunk_free_fn_copy) (void *p);
struct nn_chunk_copy {
    struct nn_atomic refcount;
    size_t size;
    nn_ofi_chunk_free_fn_copy ffn;
};

/* Hack-update chunk size 
   WARNING: This code shares a major part with `nn_chunk_getptr`
   TODO: Perhaps expose the private API from `chunk.c` ? */
static void nn_sofi_in_set_chunk_size( void * p, size_t size )
{
    struct nn_chunk_copy* header;
    uint32_t tag;
    uint32_t off;

    tag = nn_getl((uint8_t*) p - sizeof (uint32_t));
    nn_assert ( (tag == 0xdeadcafe) || (tag == 0xdeadd00d) );

    /* On user-pointer chunks the offset is virtual */
    if (tag == 0xdeadd00d) {
        off = 0;
    } else {
        off = nn_getl( (uint8_t*) p - 2 * sizeof (uint32_t) );
    }

    /* Get chunk head */
    header = (struct nn_chunk_copy*)((uint8_t*) p - 2*sizeof (uint32_t) - off -
        sizeof (struct nn_chunk_copy));

    /* Update size */
    header->size = size;

}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* END DANGEROUPS REGION                                                     */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* Post buffers */
static int nn_sofi_in_post_buffers(struct nn_sofi_in *self)
{
    int ret;
    void * desc[1];
    struct iovec iov[1];
    struct nn_sofi_in_chunk * pick_chunk = NULL;

    /* Pick the first available free input chunk */
    for (struct nn_sofi_in_chunk * chunk = self->mr_chunks, 
            * chunk_end = self->mr_chunks + self->queue_size;
         chunk < chunk_end; chunk++ ) {

        /* If we found a free chunk (not posted and not locked), use it */
        _ofi_debug("OFI[i]: chunk=%p, flags=%i\n", chunk, chunk->flags);
        if (chunk->flags == 0) {
            pick_chunk = chunk;
            break; 
        }
    }

    /* If nothing found, we ran out of memory */
    if (!pick_chunk) {
        _ofi_debug("OFI[i]: Ran out of RxMR regions!\n");
        return -ENOMEM;
    }

    /* Prepare IOV */
    iov[0].iov_base = pick_chunk->chunk;
    iov[0].iov_len = self->msg_size;

    /* Prepare MR Desc */
    desc[0] = fi_mr_desc( pick_chunk->mr );

    /* Prepare fi_msg */
    struct fi_msg msg = {
        .msg_iov = iov,
        .desc = desc,
        .iov_count = 1,
        .addr = self->ep->remote_fi_addr,
        .context = &pick_chunk->context,
        .data = 0
    };

    /* Post receive buffers */
    _ofi_debug("OFI[i]: Posting buffers from RxMR chunk=%p (ctx=%p, buf=%p)\n", 
        pick_chunk, &pick_chunk->context, pick_chunk->chunk );
    ret = fi_recvmsg(self->ep->ep, &msg, 0);
    if (ret) {

        /* If we are in a bad state, we were remotely disconnected */
        if (ret == -FI_EOPBADSTATE) {
            _ofi_debug("OFI[i]: fi_recvmsg() returned %i, considering "
                "shutdown.\n", ret);
            return -EINTR;
        }

        /* Otherwise display error */
        FT_PRINTERR("nn_sofi_in_post_buffers->fi_recvmsg", ret);
        return -EBADF;
    }

    /* Mark chunk as posted */
    pick_chunk->flags |= NN_SOFI_IN_MR_FLAG_POSTED;

    /* Successful */
    return 0;
}

/* Move buffers for pick-up by SOFI */
static void nn_sofi_in_pop_buffers(struct nn_sofi_in *self)
{
    /* Pop egress item from queue */
    struct nn_queue_item * chunk = nn_queue_pop( &self->queue_ingress );
    _ofi_debug("OFI[i]: Moving item from queue to ingress (chunk=%p)\n", chunk);
    if (!chunk) {
        self->chunk_ingress = NULL;
        return;
    }

    /* Post message and remove locked flag */
    self->chunk_ingress = nn_cont(chunk, struct nn_sofi_in_chunk, item);
    nn_queue_item_term( &self->chunk_ingress->item );

}

/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/**
 * Initialize the state machine 
 */
void nn_sofi_in_init ( struct nn_sofi_in *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep,
    const uint8_t ng_direction, int queue_size, size_t msg_size,
    struct nn_pipebase * pipebase, int src, struct nn_fsm *owner )
{
    int ret, offset;
    _ofi_debug("OFI[i]: Initializing Input FSM\n");

    /* Reset properties */
    self->state = NN_SOFI_IN_STATE_IDLE;
    self->error = 0;
    self->ofi = ofi;
    self->ep = ep;
    self->msg_size = msg_size;
    self->queue_size = queue_size;

    /* Initialize events */
    nn_fsm_event_init (&self->event_started);
    nn_fsm_event_init (&self->event_received);

    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_in_handler, nn_sofi_in_shutdown,
        src, self, owner);

    /*  Choose a worker thread to handle this socket. */
    self->worker = nn_fsm_choose_worker (&self->fsm);

    /* Initialize worker tasks */
    nn_worker_task_init (&self->task_rx, NN_SOFI_IN_SRC_TASK_RX,
        &self->fsm);
    nn_worker_task_init (&self->task_rx_ack, NN_SOFI_IN_SRC_TASK_RX_ACK,
        &self->fsm);
    nn_worker_task_init (&self->task_rx_error, NN_SOFI_IN_SRC_TASK_RX_ERROR,
        &self->fsm);

    /* Initialize mutex */
    nn_mutex_init( &self->mutex_ingress );

    /* Initialize timer */
    nn_timer_init(&self->timer_abort, NN_SOFI_IN_SRC_TIMER, 
        &self->fsm);

    /* Initialize queue */
    nn_queue_init( &self->queue_ingress );

    /* Manage a new MR for small blocking calls */
    ofi_mr_init( ep, &self->mr_small );
    ofi_mr_manage( ep, &self->mr_small, 
        nn_alloc(NN_OFI_SMALLMR_SIZE, "mr_small"), 
        NN_OFI_SMALLMR_SIZE, NN_SOFI_IN_MR_SMALL, MR_RECV );

    /* Allocate chunk buffer */
    self->mr_chunks = nn_alloc( sizeof (struct nn_sofi_in_chunk) * queue_size,
        "mr chunks" );
    nn_assert( self->mr_chunks );

    /* Allocate the incoming buffers */
    _ofi_debug("OFI[i]: Allocating buffers len=%i, size=%lu\n", queue_size, 
        msg_size);

    offset = 0;
    for (struct nn_sofi_in_chunk * chunk = self->mr_chunks, 
            * chunk_end = self->mr_chunks + queue_size;
         chunk < chunk_end; chunk++ ) {

        /* Init properties */
        chunk->flags = 0;

        /* Allocate message chunk */
        ret = nn_chunk_alloc( msg_size, 0, &chunk->chunk );
        nn_assert( ret == 0 );

        /* Register this memory region */
        _ofi_debug("OFI[i]: Allocated RxMR chunk=%p, key=%i\n", chunk, 
            NN_SOFI_IN_MR_INPUT_BASE + offset);
        ret = fi_mr_reg( ep->domain, chunk->chunk, msg_size, 
            FI_RECV | FI_READ | FI_REMOTE_WRITE, 0, 
            NN_SOFI_IN_MR_INPUT_BASE + (offset++), 0, &chunk->mr, NULL);
        nn_assert( ret == 0 );

    }

}

/**
 * Check if FSM is idle 
 */
int nn_sofi_in_isidle (struct nn_sofi_in *self)
{
    return nn_fsm_isidle (&self->fsm);
}

/**
 * Cleanup the state machine 
 */
void nn_sofi_in_term (struct nn_sofi_in *self)
{
    _ofi_debug("OFI[i]: Terminating Input FSM\n");

    /* Free MR */
    nn_free( self->mr_small.ptr );
    ofi_mr_free( self->ep, &self->mr_small );

    /* The queue must be empty by here */
    nn_assert( nn_queue_pop( &self->queue_ingress) == NULL );

    /* Free pointers */
    for (struct nn_sofi_in_chunk * chunk = self->mr_chunks, 
            * chunk_end = self->mr_chunks + self->queue_size;
         chunk < chunk_end; chunk++ ) {

        /* No chunk must not be locked at shutdown time */
        nn_assert( (chunk->flags & NN_SOFI_IN_MR_FLAG_LOCKED) == 0 );

        /* Free everything */
        FT_CLOSE_FID( chunk->mr );
        nn_chunk_free( chunk->chunk );
        nn_queue_item_term( &chunk->item );
        _ofi_debug("OFI[i]: Released RxMR chunk=%p\n", chunk);

    }
    nn_free( self->mr_chunks );

    /* Terminate queue */
    nn_queue_term( &self->queue_ingress );

    /* Terminate mutex */
    nn_mutex_term( &self->mutex_ingress );

    /* Abort timer */
    nn_timer_term (&self->timer_abort);

    /* Cleanup events */
    nn_fsm_event_term (&self->event_started);
    nn_fsm_event_term (&self->event_received);

    /* Cleanup worker tasks */
    nn_worker_cancel (self->worker, &self->task_rx_error);
    nn_worker_cancel (self->worker, &self->task_rx_ack);
    nn_worker_cancel (self->worker, &self->task_rx);
    nn_worker_task_term (&self->task_rx_error);
    nn_worker_task_term (&self->task_rx_ack);
    nn_worker_task_term (&self->task_rx);

    /* Terminate fsm */
    nn_fsm_term (&self->fsm);

}

/**
 * Start the state machine 
 */
void nn_sofi_in_start (struct nn_sofi_in *self)
{
    _ofi_debug("OFI[i]: Starting Input FSM\n");
    nn_fsm_start( &self->fsm );
}

/**
 * Stop the state machine 
 */
void nn_sofi_in_stop (struct nn_sofi_in *self)
{
    _ofi_debug("OFI[i]: Stopping Input FSM\n");

    /* Handle stop according to state */
    switch (self->state) {

        /* This cases are safe to stop right away */
        case NN_SOFI_IN_STATE_IDLE:
        case NN_SOFI_IN_STATE_READY:
        case NN_SOFI_IN_STATE_ACKNOWLEGING:

            /* These are safe to become 'closed' */
            _ofi_debug("OFI[i]: Switching state=%i to closed\n", self->state);
            NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_CLOSED );

        case NN_SOFI_IN_STATE_CLOSED:

            /* We are safe to stop right away */
            _ofi_debug("OFI[i]: Stopping right away\n");
            nn_fsm_stop( &self->fsm );
            break;

        /* Processing switches to abording */
        case NN_SOFI_IN_STATE_RECEIVING:

            /* Start timeout and start abording */
            _ofi_debug("OFI[i]: Switching to ABORTING state\n");
            NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_ABORTING );
            nn_timer_start (&self->timer_abort, NN_SOFI_IN_TIMEOUT_ABORT);
            break;

        /* Critical states, don't do anything here, we'll stop in the end */
        case NN_SOFI_IN_STATE_ABORTING:
        case NN_SOFI_IN_STATE_ABORT_TIMEOUT:
        case NN_SOFI_IN_STATE_ABORT_CLEANUP:
            _ofi_debug("OFI[i]: Ignoring STOP command\n");
            break;

    };

}

/* ============================== */
/*        EXTERNAL EVENTS         */
/* ============================== */

/**
 * Trigger an rx event
 */
void nn_sofi_in_rx_event( struct nn_sofi_in *self, 
    struct fi_cq_data_entry * cq_entry )
{
    struct nn_sofi_in_chunk * chunk = 
        nn_cont (cq_entry->op_context, struct nn_sofi_in_chunk, context);
    _ofi_debug("OFI[i]: Got CQ event for the received frame, ctx=%p (ptr=%p)\n", chunk, cq_entry->op_context);

    /* The chunk is not posted any more */
    chunk->flags &= ~NN_SOFI_IN_MR_FLAG_POSTED;

    /* Update chunk size */
    nn_assert( cq_entry->len <= self->msg_size );
    nn_sofi_in_set_chunk_size( chunk->chunk, cq_entry->len );
    _ofi_debug("OFI[i]: Got incoming len=%lu\n", cq_entry->len);

    /* Continue according to state */
    switch (self->state) {
        case NN_SOFI_IN_STATE_READY:
        case NN_SOFI_IN_STATE_RECEIVING:
        case NN_SOFI_IN_STATE_ACKNOWLEGING:

            /* Enter critical section */
            {
                nn_mutex_lock( &self->mutex_ingress );

                /* Lock and stage message */
                chunk->flags |= NN_SOFI_IN_MR_FLAG_LOCKED;
                nn_queue_item_init( &chunk->item );
                nn_queue_push( &self->queue_ingress, &chunk->item );

                nn_mutex_unlock( &self->mutex_ingress );
            }

            /* If in READY state, switch to receiving */
            if (self->state == NN_SOFI_IN_STATE_READY)
                self->state = NN_SOFI_IN_STATE_RECEIVING;
    
            /* Trigger worker task */
            nn_worker_execute (self->worker, &self->task_rx);

            break;

        default:
            _ofi_debug("OFI[i]: Discarding incoming event because state=%i\n", self->state);
            break;
    }

}

/**
 * Trigger an rx error event
 */
void nn_sofi_in_rx_error_event( struct nn_sofi_in *self, 
    struct fi_cq_err_entry * cq_err )
{
    struct nn_sofi_in_chunk * chunk = 
        nn_cont (cq_err->op_context, struct nn_sofi_in_chunk, context);
    _ofi_debug("OFI[i]: Got CQ error for the received frame, ctx=%p\n", chunk);

    /* The chunk is not posted any more. */
    chunk->flags &= ~NN_SOFI_IN_MR_FLAG_POSTED;

    /* Continue according to state */
    switch (self->state) {
        case NN_SOFI_IN_STATE_READY:
        case NN_SOFI_IN_STATE_RECEIVING:
        case NN_SOFI_IN_STATE_ACKNOWLEGING:

            /* Keep error */
            self->error = cq_err->err;

            /* If in READY state, switch to receiving */
            if (self->state == NN_SOFI_IN_STATE_READY)
                self->state = NN_SOFI_IN_STATE_RECEIVING;

            /* Trigger error */
            nn_worker_execute (self->worker, &self->task_rx_error);
            break;

        default:
            return;
    }

}

/**
 * Acknowledge an rx event
 */
int nn_sofi_in_rx_event_ack( struct nn_sofi_in *self, struct nn_msg *msg )
{

    /* If there is no message, return error */
    if (self->chunk_ingress == NULL) {
        _ofi_debug("OFI[i]: Acknowledging an event wihout a pending rx event!\n");
        return -EFSM;
    }

    /* Initialize and unlock message */
    _ofi_debug("OFI[i]: Acknowledged rx chunk=%p\n",self->chunk_ingress )
    nn_chunk_addref( self->chunk_ingress->chunk, 1 );
    nn_msg_init_chunk ( msg, self->chunk_ingress->chunk );

    /* Release chunk */
    self->chunk_ingress->flags &= ~NN_SOFI_IN_MR_FLAG_LOCKED;
    self->chunk_ingress= NULL;

    /* Trigger worker task */
    nn_worker_execute (self->worker, &self->task_rx_ack);

    /* Works good */
    return 0;
}

/**
 * Synchronous (blocking) rx request
 */
size_t nn_sofi_in_rx( struct nn_sofi_in *self, void * ptr, 
    size_t max_sz, int timeout )
{
    struct fi_cq_data_entry entry;
    size_t rx_size;
    struct iovec iov[1];
    void * desc[1];
    int ret;

    _ofi_debug("OFI[i]: Blocking RX max_sz=%lu, timeout=%i\n", max_sz, timeout);

    /* Check if message does not fit in the buffer */
    if (max_sz > NN_OFI_SMALLMR_SIZE)
        return -ENOMEM;

    /* Move data to small MR */
    memcpy( self->mr_small.ptr, ptr, max_sz );

    /* Prepare msg structure */
    iov[0].iov_base = self->mr_small.ptr;
    iov[0].iov_len = max_sz;
    desc[0] = OFI_MR_DESC(self->mr_small);
    struct fi_msg msg = {
        .msg_iov = iov,
        .desc = desc,
        .iov_count = 1,
        .addr = self->ep->remote_fi_addr,
        .context = &self->context,
        .data = 0
    };

    /* Receive data */
    ret = fi_recvmsg( self->ep->ep, &msg, 0 );
    if (ret) {
        FT_PRINTERR("fi_recvmsg", ret);
        // return ret;
        return 0;
    }

    /* Wait for synchronous event */
    ret = fi_cq_sread( self->ep->rx_cq, &entry, 1, NULL, timeout );
    if ((ret == 0) || (ret == -FI_EAGAIN)) {
        _ofi_debug("OFI[i]: Rx operation timed out\n");
        // return -ETIMEDOUT;
        return 0;
    }
    if (ret < 0) {
        FT_PRINTERR("fi_cq_sread", ret);
        // return ret;
        return 0;
    }

    _ofi_debug("OFI[i]: Blocking RX Completed, len=%zu, ctx=%p\n", entry.len, &self->context);

    /* Move data to ptr */
    rx_size = entry.len;
    memcpy( ptr, self->mr_small.ptr, rx_size );

    /* Return bytes read */
    return rx_size;
}

/* ============================== */
/*          FSM HANDLERS          */
/* ============================== */

/**
 * SHUTDOWN State Handler
 */
static void nn_sofi_in_shutdown (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{

    /* Get pointer to sofi structure */
    struct nn_sofi_in *self;
    self = nn_cont (fsm, struct nn_sofi_in, fsm);

    /* If this is part of the FSM action, start shutdown */
    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {

        /* Stop FSM, triggering a final event according to state */
        if (self->error == 0) {
            _ofi_debug("OFI[i]: Stopping FSM with CLOSE event\n");
            nn_fsm_stopped(&self->fsm, NN_SOFI_IN_EVENT_CLOSE);
        } else {
            _ofi_debug("OFI[i]: Stopping FSM with ERROR (error=%i) "
                "event\n", self->error);
            nn_fsm_stopped(&self->fsm, NN_SOFI_IN_EVENT_ERROR);
        }
        return;

    }

    /* Invalid state */
    nn_fsm_bad_state (self->state, src, type);
}

/**
 * ACTIVE State Handler
 */
static void nn_sofi_in_handler (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{

    /* Get pointer to sofi structure */
    struct nn_sofi_in *self;
    self = nn_cont (fsm, struct nn_sofi_in, fsm);

    /* Handle state transitions */
    switch (self->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_SOFI_IN_STATE_IDLE:
        switch (src) {

        /* ========================= */
        /*  FSM Action               */
        /* ========================= */
        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:

                /* Post buffers */
                self->error = nn_sofi_in_post_buffers( self );
                if (!self->error) {

                    /* When successful switch to POSTED state */
                    _ofi_debug("OFI[i]: Input buffers posted\n");
                    NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_READY );

                    /* That's the first acknowledement event */
                    nn_fsm_raise(&self->fsm, &self->event_started, 
                        NN_SOFI_IN_EVENT_STARTED);

                } else {

                    /* When unsuccessful, raise ERROR event */
                    _ofi_debug("OFI[i]: Error trying to post input buffers\n");
                    nn_sofi_in_stop( self );

                }

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  READY state.                                                              */
/*  We have posted the input buffers and we are waiting for events from the   */
/*  polling thread that runs in sofi. It triggers events through worker tasks */
/*  in order to traverse threads.                                             */
/******************************************************************************/
    case NN_SOFI_IN_STATE_READY:

        /* This is never handled, that's an intermediate state */    
        nn_fsm_bad_state (self->state, src, type);

/******************************************************************************/
/*  RECEIVING state.                                                          */
/*  There are one or more messages pending in the input queue that must be    */
/*  forwarded to nanomsg internals.                                           */
/******************************************************************************/
    case NN_SOFI_IN_STATE_RECEIVING:
        switch (src) {

        /* ========================= */
        /*  TASK : Rx Event          */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Move data to rx buffer */
                _ofi_debug("OFI[i]: Handing Rx event\n");
                nn_sofi_in_pop_buffers( self );
    
                /* Post another rx buffer */                
                self->error = nn_sofi_in_post_buffers( self );

                /* Notify SOFI for this fact that a message is waiting */
                _ofi_debug("OFI[i]: Notifying nanomsg for pending event (rx)\n");
                nn_fsm_raise(&self->fsm, &self->event_received, 
                    NN_SOFI_IN_EVENT_RECEIVED);

                /* Check errors on buffer posting */
                if (!self->error) {
                    _ofi_debug("OFI[i]: Input buffers posted\n");

                    _ofi_debug("OFI[i]: Waiting for acknowledgement\n");
                    NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_ACKNOWLEGING );
                    
                } else {
                    _ofi_debug("OFI[i]: Error trying to post input buffers\n");
                    nn_sofi_in_stop( self );

                }

                /* We are waiting for an ACK */
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  TASK : Rx Error Event    */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX_ERROR:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Trigger error event */
                _ofi_debug("OFI[i]: Error Rx event\n");
                nn_sofi_in_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  ACKNOWLEGING state.                                                       */
/*  We have informed nanomsg that there is a message pending and we are       */
/*  waiting for an acknowledgement when the mssage is handled before resuming */
/*  operations.                                                               */
/******************************************************************************/
    case NN_SOFI_IN_STATE_ACKNOWLEGING:
        switch (src) {

        /* ========================= */
        /*  TASK : Rx Ack Event      */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX_ACK:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                _ofi_debug("OFI[i]: Acknowledged receive event\n");

                /* Check if there are more messages pending in the queue */
                nn_mutex_lock( &self->mutex_ingress );
                if (!nn_queue_empty( &self->queue_ingress )) {
                    nn_mutex_unlock( &self->mutex_ingress );

                    /* Notify SOFI for this fact that a message is waiting */
                    _ofi_debug("OFI[i]: Notifying nanomsg for pending event (queued)\n");
                    nn_fsm_raise(&self->fsm, &self->event_received, 
                        NN_SOFI_IN_EVENT_RECEIVED);

                    /* Don't continue */
                    return;

                }

                /* When successful switch to POSTED state */
                _ofi_debug("OFI[i]: Switching to NN_SOFI_IN_STATE_READY after RX_ACK & empty ingress queue\n");
                self->state = NN_SOFI_IN_STATE_READY;
                nn_mutex_unlock( &self->mutex_ingress );

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  TASK : Rx Event          */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Move data to rx buffer */
                _ofi_debug("OFI[i]: Handing Rx event\n");
                nn_sofi_in_pop_buffers( self );
    
                /* Post another rx buffer, but don't send a RECEIVED event
                   in until we get an acknowledgement */                
                self->error = nn_sofi_in_post_buffers( self );
                if (!self->error) {
                    _ofi_debug("OFI[i]: Input buffers posted\n"); 

                    /* Don't change anything */

                } else {
                    _ofi_debug("OFI[i]: Error trying to post input buffers\n");
                    nn_sofi_in_stop( self );

                }

                /* We are waiting for an ACK */
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  TASK : Rx Error Event    */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX_ERROR:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Trigger error event */
                _ofi_debug("OFI[i]: Error Rx event while waiting Rx ACK\n");
                nn_sofi_in_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  ABORTING state.                                                           */
/*  We are waiting for receive completion or for timeout events.              */
/******************************************************************************/
    case NN_SOFI_IN_STATE_ABORTING:
        switch (src) {

        /* ========================= */
        /*  TASK : Rx Ack or Error   */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX_ERROR:
        case NN_SOFI_IN_SRC_TASK_RX_ACK:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Stop timer, we are good to cleanup */
                _ofi_debug("OFI[i]: Acknowledged receive event, continuing "
                            "abort\n");

                /* Don't switch state if there are items pending */
                nn_mutex_lock( &self->mutex_ingress );
                if (!nn_queue_empty( &self->queue_ingress )) {
                    /* Dont' forget to release the mutex */
                    nn_mutex_unlock( &self->mutex_ingress );
                    return;
                }

                /* Clean-up if there are no messages left */
                self->state = NN_SOFI_IN_STATE_ABORT_CLEANUP;
                nn_mutex_unlock( &self->mutex_ingress );

                /* Cleanup */
                nn_timer_stop( &self->timer_abort );

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  Abort Timer Timeout      */
        /* ========================= */
        case NN_SOFI_IN_SRC_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* Stop timer, there was an error */
                _ofi_debug("OFI[i]: Acknowledgement timed out, waiting for "
                            "timer to stop\n");
                NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_ABORT_TIMEOUT );
                nn_timer_stop( &self->timer_abort );

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }


        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  ABORT TIMEOUT state.                                                      */
/*  We are waiting for timer to stop before rising error.                     */
/******************************************************************************/
    case NN_SOFI_IN_STATE_ABORT_TIMEOUT:
        switch (src) {

        /* ========================= */
        /*  Abort Timer Timeout      */
        /* ========================= */
        case NN_SOFI_IN_SRC_TIMER:
            switch (type) {
            case NN_TIMER_STOPPED:

                /* Timer stopped, but there was an error */
                _ofi_debug("OFI[i]: Timeout timer stopped, going for error\n");
                nn_sofi_in_stop( self );

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }


        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  ABORT CLEANUP state.                                                      */
/*  We are waiting for timer to stop before closing.                          */
/******************************************************************************/
    case NN_SOFI_IN_STATE_ABORT_CLEANUP:
        switch (src) {

        /* ========================= */
        /*  Abort Timer Timeout      */
        /* ========================= */
        case NN_SOFI_IN_SRC_TIMER:
            switch (type) {
            case NN_TIMER_STOPPED:

                /* Timer stopped, and we are good */
                _ofi_debug("OFI[i]: Timeout timer stopped, closed\n");
                NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_CLOSED );
                nn_sofi_in_stop( self );

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }


        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (self->state, src, type);

    }

}


