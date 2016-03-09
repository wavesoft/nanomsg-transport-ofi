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
#include "sofi.h"
#include "sofi_in.h"

#include <string.h>

#include "../../aio/ctx.h"
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
#define NN_SOFI_IN_TIMEOUT_RXMR_FREE    1000

/* Incoming MR flags */
#define NN_SOFI_IN_MR_FLAG_POSTED       0x00000001
#define NN_SOFI_IN_MR_FLAG_LOCKED       0x00000002
#define NN_SOFI_IN_MR_FLAG_KEEPALIVE    0x00000004

/* State change with mutex lock */
#define NN_SOFI_IN_SET_STATE(self,val) \
    _ofi_debug("OFI[i]: Switching to %s\n", #val); \
    self->state = val;

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
    uint32_t id;
    nn_ofi_chunk_free_fn_copy ffn;
};

/* Hack-update chunk size 
   WARNING: This code shares a major part with `nn_chunk_getptr`
   TODO: Perhaps expose the private API from `chunk.c` ? */
static void * nn_sofi_in_set_chunk_size( void * p, size_t size )
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

    /* TODO: We should also reset the empty space to zero */

    /* Update size */
    header->size = size;

    /* Return pointer */
    return p;

}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* END DANGEROUPS REGION                                                     */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* Post buffers */
static int nn_sofi_in_post_buffers(struct nn_sofi_in *self)
{
    int ret;
    uint32_t pick_age;
    struct nn_sofi_in_chunk * pick_chunk = NULL;

    // nn_mutex_lock( &self->mutex_ingress );
    while (!pick_chunk) {

        /* Pick the first available free input chunk */
        pick_chunk = NULL;
        for (struct nn_sofi_in_chunk * chunk = self->mr_chunks, 
                * chunk_end = self->mr_chunks + self->queue_size;
             chunk < chunk_end; chunk++ ) {

            /* If we found a free chunk (not posted and not locked), use it */
            _ofi_debug("OFI[i]: chunk=%p, flags=%i\n", chunk, chunk->flags);
            if (chunk->flags == 0) {
                if ((pick_chunk == NULL) || (chunk->age < pick_age)) {
                    pick_age = chunk->age;
                    pick_chunk = chunk;
                }
            }
        }

        /* If nothing found, we ran out of memory, wait until we have a free */
        if (!pick_chunk) {
            _ofi_debug("OFI[i]: Ran out of RxMR regions!\n");
            // nn_mutex_unlock( &self->mutex_ingress );
            return -EAGAIN;
        }

    }
    // nn_mutex_unlock( &self->mutex_ingress );

    _ofi_debug("OFI[i]: >>>>>> Grab MR %p\n", pick_chunk);

    /* Mark chunk as posted */
    pick_chunk->flags |= NN_SOFI_IN_MR_FLAG_POSTED;
    pick_chunk->age = ++self->age_ingress;

    /* Post receive buffers */
    _ofi_debug("OFI[i]: Posting buffers from RxMR chunk=%p (ctx=%p, buf=%p)\n", 
        pick_chunk, &pick_chunk->context, pick_chunk->chunk );
    _ofi_debug("OFI[i] ### POSTING RECEIVE BUFFER len=%lu, ctx=%p\n", self->msg_size, &pick_chunk->context);
    ret = fi_recv(self->ep->ep, pick_chunk->chunk, self->msg_size,
            fi_mr_desc( pick_chunk->mr ), self->ep->remote_fi_addr, &pick_chunk->context);
    if (ret) {

        /* If we are in a bad state, we were remotely disconnected */
        if (ret == -FI_EOPBADSTATE) {
            _ofi_debug("OFI[i]: fi_recv() returned %i, considering "
                "shutdown.\n", ret);
            return -EINTR;
        }

        /* Otherwise display error */
        FT_PRINTERR("nn_sofi_in_post_buffers->fi_recv", ret);
        return -EBADF;
    }

    return 0;
}

/* Move buffers for pick-up by SOFI */
static struct nn_sofi_in_chunk * nn_sofi_in_pop_buffers(struct nn_sofi_in *self)
{
    /* Pop egress item from queue */
    struct nn_queue_item * item = nn_queue_pop( &self->queue_ingress );
    _ofi_debug("OFI[i]: <-- POPPING item from queue (item=%p)\n",item )
    nn_assert( item );

    /* Post message and remove locked flag */
    struct nn_sofi_in_chunk *chunk = nn_cont(item,struct nn_sofi_in_chunk,item);

    return chunk;
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
    self->age_ingress = 0;
    self->underrun_ingress = 0;

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
    nn_mutex_init( &self->mutex_rx );
    nn_mutex_init( &self->mutex_rx_ack );
    nn_mutex_init( &self->mutex_rx_error );

    /* Initialize timer */
    nn_timer_init(&self->timer_abort, NN_SOFI_IN_SRC_TIMER, 
        &self->fsm);

    /* Initialize queue */
    nn_queue_init( &self->queue_ingress );

    /* Allocate and manage a small MR for metadata I/O */
    self->small_ptr = nn_alloc(NN_OFI_SMALLMR_SIZE, "mr_small");
    nn_assert( self->small_ptr );
    memset( self->small_ptr, 0, NN_OFI_SMALLMR_SIZE );
    ret = fi_mr_reg(ep->domain, self->small_ptr, NN_OFI_SMALLMR_SIZE, 
        FI_RECV | FI_READ | FI_REMOTE_WRITE, 0, NN_SOFI_IN_MR_SMALL, 0, 
        &self->small_mr, NULL);
    nn_assert( ret == 0 );

    /* Allocate chunk buffer */
    self->mr_chunks = nn_alloc( sizeof (struct nn_sofi_in_chunk) * queue_size,
        "mr chunks" );
    memset( self->mr_chunks, 0, sizeof (struct nn_sofi_in_chunk) * queue_size );
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
        chunk->age = 0;

        /* Allocate message chunk */
        ret = nn_chunk_alloc( msg_size, 0, &chunk->chunk );
        memset( chunk->chunk, 0, msg_size );
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
    FT_CLOSE_FID( self->small_mr );
    nn_free( self->small_ptr );

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
    nn_mutex_term( &self->mutex_rx );
    nn_mutex_term( &self->mutex_rx_ack );
    nn_mutex_term( &self->mutex_rx_error );

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
        case NN_SOFI_IN_STATE_RECEIVING:

            /* These are safe to become 'closed' */
            _ofi_debug("OFI[i]: Switching from=%i to CLOSED\n", self->state);
            NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_CLOSED );

        case NN_SOFI_IN_STATE_CLOSED:

            /* We are safe to stop right away */
            _ofi_debug("OFI[i]: Stopping right away\n");
            nn_fsm_stop( &self->fsm );
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
    nn_mutex_lock( &self->mutex_rx );
    struct nn_sofi_in_chunk * chunk = 
        nn_cont (cq_entry->op_context, struct nn_sofi_in_chunk, context);
    _ofi_debug("OFI[i]: Got CQ event for the received frame, ctx=%p (ptr=%p)\n", 
        cq_entry->op_context, chunk);

    /* The chunk is not posted any more */
    chunk->flags &= ~NN_SOFI_IN_MR_FLAG_POSTED;

    /* Update chunk size */
    nn_assert( cq_entry->len <= self->msg_size );
    _ofi_debug("OFI[i]: Got incoming len=%lu, chunk=%p\n", cq_entry->len, chunk->chunk);
    chunk->chunk = nn_sofi_in_set_chunk_size( chunk->chunk, cq_entry->len );

    /* Continue according to state */
    switch (self->state) {
        case NN_SOFI_IN_STATE_READY:
        case NN_SOFI_IN_STATE_RECEIVING:
        case NN_SOFI_IN_STATE_ACKNOWLEGING:

            /* Check if this is a keepalive message */
            if (nn_slow(nn_chunk_size(chunk->chunk) == NN_SOFI_KEEPALIVE_PACKET_LEN)) {
                if (nn_fast( memcmp(chunk->chunk, NN_SOFI_KEEPALIVE_PACKET, 
                    NN_SOFI_KEEPALIVE_PACKET_LEN) == 0 )) {
                    _ofi_debug("OFI[i]: This chunk is a keepalive packet\n");
                    chunk->flags |= NN_SOFI_IN_MR_FLAG_KEEPALIVE;
                }
            }

            /* Lock and stage message */
            chunk->flags |= NN_SOFI_IN_MR_FLAG_LOCKED;
            _ofi_debug("OFI[i]: PUSHING --> item in queue (item=%p)\n", &chunk->item);
            nn_queue_item_init( &chunk->item );
            nn_queue_push( &self->queue_ingress, &chunk->item );
    
            /* Trigger worker task */
            nn_worker_execute (self->worker, &self->task_rx);

            break;

        default:
            _ofi_debug("OFI[i]: Discarding incoming event because state=%i\n", 
                self->state);
            break;
    }
    nn_mutex_unlock( &self->mutex_rx );
}

/**
 * Trigger an rx error event
 */
void nn_sofi_in_rx_error_event( struct nn_sofi_in *self, 
    struct fi_cq_err_entry * cq_err )
{
    nn_mutex_lock( &self->mutex_rx_error );
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

            /* Trigger error */
            nn_worker_execute (self->worker, &self->task_rx_error);
            break;

        default:
            return;
    }
    nn_mutex_unlock( &self->mutex_rx_error );
}

/**
 * Acknowledge an rx event
 */
int nn_sofi_in_rx_event_ack( struct nn_sofi_in *self, struct nn_msg *msg )
{
    nn_mutex_lock( &self->mutex_rx_ack );

    /* If there is no message, return error */
    // nn_mutex_lock( &self->mutex_ingress );
    struct nn_sofi_in_chunk * chunk = nn_sofi_in_pop_buffers( self );
    // nn_mutex_unlock( &self->mutex_ingress );
    if (chunk == NULL) {
        _ofi_debug("OFI[i]: Acknowledging an Rx event without one pending!\n");
        nn_mutex_unlock( &self->mutex_rx_ack );
        return -EFSM;
    }

    /* Initialize and unlock message */
    _ofi_debug("OFI[i]: Acknowledged rx chunk=%p, size=%zu\n",chunk, nn_chunk_size(chunk->chunk) );

    nn_chunk_addref( chunk->chunk, 2 );
    nn_msg_init_chunk ( msg, chunk->chunk );

    /* Release chunk */
    chunk->flags &= ~NN_SOFI_IN_MR_FLAG_LOCKED;
    _ofi_debug("OFI[i]: <<<<<< Unlock MR %p\n", chunk);

    /* Trigger worker task */
    nn_worker_execute (self->worker, &self->task_rx_ack);

    /* Works good */
    nn_mutex_unlock( &self->mutex_rx_ack );
    return nn_chunk_size( chunk->chunk );
}

/**
 * Post receive buffers
 */
size_t nn_sofi_in_rx_post( struct nn_sofi_in *self, size_t max_sz )
{
    int ret;

    _ofi_debug("OFI[i]: Blocking RX Post max_sz=%lu\n", max_sz);

    /* Check if message does not fit in the buffer */
    if (max_sz > NN_OFI_SMALLMR_SIZE) { 
        return -ENOMEM;
    }

    /* Receive data */
    _ofi_debug("OFI[i] ### POSTING RECEIVE BUFFER len=%i, ctx=%p\n", NN_OFI_SMALLMR_SIZE, &self->context);
    ret = fi_recv( self->ep->ep, self->small_ptr, NN_OFI_SMALLMR_SIZE, fi_mr_desc(self->small_mr),
                    self->ep->remote_fi_addr, &self->context );
    if (ret) {
        FT_PRINTERR("fi_recv", ret);
        return ret;
    }

    /* Success */
    return 0;
}

/**
 * Synchronous (blocking) rx request
 */
size_t nn_sofi_in_rx_recv( struct nn_sofi_in *self, void * ptr, 
    size_t max_sz, int timeout )
{
    struct fi_cq_data_entry entry;
    size_t rx_size;
    int ret;

    _ofi_debug("OFI[i]: Blocking RX Recv max_sz=%lu, timeout=%i\n", max_sz, timeout);

    /* Wait for synchronous event */
    ret = fi_cq_sread( self->ep->rx_cq, &entry, 1, NULL, timeout );
    if ((ret == 0) || (ret == -FI_EAGAIN)) {
        _ofi_debug("OFI[i]: Rx operation timed out\n");
        // return -ETIMEDOUT;
        return 0;
    }
    if (ret < 0) {

        /* read CQ Error */
        int lret = ret;
        struct fi_cq_err_entry err_entry;
        ret = fi_cq_readerr(self->ep->rx_cq, &err_entry, 0);
        _ofi_debug("OFI[i]: CQ Error (returned %i, %s): %s (%s)\n",
            lret, fi_strerror(lret),
            fi_strerror(err_entry.err),
            fi_cq_strerror(self->ep->rx_cq, err_entry.prov_errno, err_entry.err_data, NULL, 0)
        );

        // return ret;
        return 0;
    }

    _ofi_debug("OFI[i]: Blocking RX Completed, len=%zu, ctx=%p\n", entry.len, 
        entry.op_context);

    /* Move data to ptr */
    rx_size = entry.len;
    memcpy( ptr, self->small_ptr, rx_size );

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
    _ofi_debug("OFI[i]: nn_sofi_in_handler state=%i, src=%i, type=%i\n", 
        self->state, src, type);

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
                    NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_CLOSED );
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

        switch (src) {

        /* ========================= */
        /*  TASK : Rx Event          */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:
                nn_mutex_lock( &self->mutex_rx );

                _ofi_debug("OFI[i]: Rx event on empty queue\n");

                /* Post another rx buffer */                
                self->error = nn_sofi_in_post_buffers( self );

                /* Check for underrun */
                if (self->error == -EAGAIN) {
                    _ofi_debug("OFI[i]: Scheduling posting of Rx buffers again "
                        "after first ack\n");
                    self->error = 0;
                    self->underrun_ingress = 1;
                }

                /* Check errors on buffer posting */
                if (!self->error) {

                    /* Switch to receiving state, waiting for ACK */
                    NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_RECEIVING );

                    /* Notify SOFI for this fact that a message is waiting */
                    _ofi_debug("OFI[i]: Notifying nanomsg for pending event (rx)\n");
                    nn_fsm_raise(&self->fsm, &self->event_received, 
                        NN_SOFI_IN_EVENT_RECEIVED);

                } else {

                    /* Switching to aborting state */
                    NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_ABORTING );
                    nn_timer_start(&self->timer_abort,NN_SOFI_IN_TIMEOUT_ABORT);

                }

                nn_mutex_unlock( &self->mutex_rx );
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
                nn_mutex_lock( &self->mutex_rx_error );

                _ofi_debug("OFI[i]: Rx Error event on empty queue\n");

                /* Nothing to wait for, close */
                NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_CLOSED );
                nn_sofi_in_stop( self );

                nn_mutex_unlock( &self->mutex_rx_error );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  RECEIVING state.                                                          */
/*  There are one or more messages pending in the input queue that must be    */
/*  forwarded to nanomsg internals.                                           */
/******************************************************************************/
    case NN_SOFI_IN_STATE_RECEIVING:
        switch (src) {

        /* ========================= */
        /*  TASK : Rx Ack Event      */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX_ACK:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:
                nn_mutex_lock( &self->mutex_rx_ack );

                /* Was the item popped the last in the queue? */
                if (!nn_queue_empty( &self->queue_ingress )) {
                    // nn_mutex_unlock( &self->mutex_ingress );

                    /* Notify SOFI for this fact that a message is waiting */
                    _ofi_debug("OFI[i]: Notifying nanomsg for pending event "
                        "(rx)\n");
                    nn_fsm_raise(&self->fsm, &self->event_received, 
                        NN_SOFI_IN_EVENT_RECEIVED);

                } else {
                    // nn_mutex_unlock( &self->mutex_ingress );

                    /* Last message popped, jump to ready */
                    NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_READY );

                }

                /* If underrun try to post input buffers now */
                if (self->underrun_ingress) {
                    _ofi_debug("OFI[i]: Retrying posting input buffers\n");
                    self->error = nn_sofi_in_post_buffers( self );
                    if (self->error < 0) {

                        _ofi_debug("OFI[i]: Error posting input buffers\n");

                        /* Switching to aborting state */
                        NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_ABORTING );
                        nn_timer_start (&self->timer_abort,
                            NN_SOFI_IN_TIMEOUT_ABORT);

                        /* Don't continue */
                        nn_mutex_unlock( &self->mutex_rx_ack );
                        return;

                    } else {

                        _ofi_debug("OFI[i]: Successfuly posted buffers\n");
                        self->underrun_ingress = 0;

                    }
                }

                nn_mutex_unlock( &self->mutex_rx_ack );
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
                nn_mutex_lock( &self->mutex_rx );

                _ofi_debug("OFI[i]: Rx event on populated queue\n");

                /* Post another rx buffer & check for errors */
                self->error = nn_sofi_in_post_buffers( self );

                /* Check for underrun */
                if (self->error == -EAGAIN) {
                    _ofi_debug("OFI[i]: Scheduling posting of Rx buffers again"
                        " after first ack\n");
                    self->error = 0;
                    self->underrun_ingress = 1;
                }

                /* Handle errors */
                if (!self->error) {

                    /* Switch to receiving state, waiting for ACK */
                    NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_RECEIVING );

                } else {

                    /* Switching to aborting state */
                    NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_ABORTING );
                    nn_timer_start(&self->timer_abort,NN_SOFI_IN_TIMEOUT_ABORT);

                }

                nn_mutex_unlock( &self->mutex_rx );
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
                nn_mutex_lock( &self->mutex_rx_error );

                _ofi_debug("OFI[i]: Error Rx event on conditioned queue\n");

                /* Switching to aborting state */
                NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_ABORTING );
                nn_timer_start (&self->timer_abort, NN_SOFI_IN_TIMEOUT_ABORT);

                nn_mutex_unlock( &self->mutex_rx_error );
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
        case NN_SOFI_IN_SRC_TASK_RX_ACK:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:
                nn_mutex_lock( &self->mutex_rx_ack );

                /* Stop timer, we are good to cleanup */
                _ofi_debug("OFI[i]: Acknowledged receive event, waiting for "
                            "timer to stop\n");

                /* Drain remaining queue items */
                // nn_mutex_lock( &self->mutex_ingress );
                while (!nn_queue_empty( &self->queue_ingress )) {
                    struct nn_sofi_in_chunk * chunk = 
                        nn_sofi_in_pop_buffers( self );
                    _ofi_debug("OFI[i]: Discarding pending chunk=%p\n", chunk);
                }
                // nn_mutex_unlock( &self->mutex_ingress );

                /* Cleanup */
                NN_SOFI_IN_SET_STATE( self, NN_SOFI_IN_STATE_ABORT_CLEANUP );
                nn_timer_stop( &self->timer_abort );

                nn_mutex_unlock( &self->mutex_rx_ack );
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


        /* ========================= */
        /*  TASK : Rx Ack or Error   */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX_ERROR:

            /* Ignore Rx Errors */
            return;


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
                self->error = -ETIMEDOUT;
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


