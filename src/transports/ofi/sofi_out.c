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
#include "sofi_out.h"
#include <string.h>

#include "../../utils/alloc.h"
#include "../../utils/cont.h"
#include "../../utils/err.h"
#include "../../utils/fast.h"
#include "../../utils/msg.h"
#include "../../utils/chunkref.h"

/* FSM States */
#define NN_SOFI_OUT_STATE_IDLE          3001
#define NN_SOFI_OUT_STATE_READY         3002
#define NN_SOFI_OUT_STATE_SENDING       3003
#define NN_SOFI_OUT_STATE_ERROR         3004
#define NN_SOFI_OUT_STATE_CLOSED        3005
#define NN_SOFI_OUT_STATE_ABORTING      3006
#define NN_SOFI_OUT_STATE_ABORT_CLEANUP 3007
#define NN_SOFI_OUT_STATE_ABORT_TIMEOUT 3008

/* FSM Sources */
#define NN_SOFI_OUT_SRC_TIMER           3101
#define NN_SOFI_OUT_SRC_TASK_CNTR       3102

/* Timeout values */
#define NN_SOFI_OUT_TIMEOUT_ABORT       1000

/* Forward Declarations */
static void nn_sofi_out_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_out_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);

/* Schedule a task */
#define NN_SOFI_OUT_SCHED_CNTR(self,task) \
    (self)-> task ++; \
    _ofi_debug("OFI[o]: >> %s >> (tx=%i, tx_send=%i, tx_error=%i)\n", #task, \
        (self)->cntr_task_tx, (self)->cntr_task_tx_send, (self)->cntr_task_tx_error ); \
    if ((self)->cntr_pending++ == 0) \
        nn_worker_execute ((self)->worker, &(self)->task_cntr); \

/* ============================== */
/*       HELPER FUNCTIONS         */
/* ============================== */


/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/*  Initialize the state machine */
void nn_sofi_out_init ( struct nn_sofi_out *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep,
    const uint8_t ng_direction, int queue_size,
    struct nn_pipebase * pipebase, int src, struct nn_fsm *owner )
{
    int ret;
    _ofi_debug("OFI[o]: Initializing Output FSM\n");

    /* Initialize properties */
    self->state = NN_SOFI_OUT_STATE_IDLE;
    self->error = 0;
    self->ofi = ofi;
    self->ep = ep;
    self->pend_sent = 0;

    /* Initialize events */
    nn_fsm_event_init (&self->event_started);
    nn_fsm_event_init (&self->event_sent);

    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_out_handler, nn_sofi_out_shutdown,
        src, self, owner);

    /*  Choose a worker thread to handle this socket. */
    self->worker = nn_fsm_choose_worker (&self->fsm);

    /* Initialize worker tasks for handling multiple events */
    nn_mutex_init (&self->mutex_cntr);
    nn_worker_task_init (&self->task_cntr, NN_SOFI_OUT_SRC_TASK_CNTR,
        &self->fsm);
    self->cntr_pending = 0;
    self->cntr_task_tx = 0;
    self->cntr_task_tx_error = 0;
    self->cntr_task_tx_send = 0;
    self->cntr_event_send = 0;

    /* Initialize timer */
    nn_timer_init(&self->timer_abort, NN_SOFI_OUT_SRC_TIMER, 
        &self->fsm);

    /* Manage a new MR for small blocking calls */
    self->small_ptr = nn_alloc(NN_OFI_SMALLMR_SIZE, "mr_small");
    nn_assert( self->small_ptr );
    memset( self->small_ptr, 0, NN_OFI_SMALLMR_SIZE );
    ret = fi_mr_reg(ep->domain, self->small_ptr, NN_OFI_SMALLMR_SIZE, 
        FI_RECV | FI_READ | FI_REMOTE_WRITE, 0, NN_SOFI_OUT_MR_SMALL, 0, 
        &self->small_mr, NULL);
    nn_assert( ret == 0 );

    /* Initialize MRM for managing multiple memory regions */
    nn_ofi_mrm_init( &self->mrm, ep, queue_size, NN_SOFI_OUT_MR_OUTPUT_BASE,
        FI_SEND | FI_WRITE | FI_REMOTE_READ );

}

/* Check if FSM is idle */
int nn_sofi_out_isidle (struct nn_sofi_out *self)
{
    return nn_fsm_isidle (&self->fsm);
}

/*  Cleanup the state machine */
void nn_sofi_out_term (struct nn_sofi_out *self)
{
    _ofi_debug("OFI[o]: Terminating Output FSM\n");

    /* Free MR */
    FT_CLOSE_FID( self->small_mr );
    nn_free( self->small_ptr );

    /* Free MRM */
    nn_ofi_mrm_term( &self->mrm );

    /* Abort timer */
    nn_timer_term (&self->timer_abort);

    /* Cleanup events */
    nn_fsm_event_term (&self->event_started);
    nn_fsm_event_term (&self->event_sent);

    /* Cleanup worker tasks */
    nn_worker_cancel (self->worker, &self->task_cntr);
    nn_worker_task_term (&self->task_cntr);
    nn_mutex_term (&self->mutex_cntr);

    /* Terminate fsm */
    nn_fsm_term (&self->fsm);

}

/*  Start the state machine */
void nn_sofi_out_start (struct nn_sofi_out *self)
{
    _ofi_debug("OFI[o]: Starting Output FSM\n");
    nn_fsm_start( &self->fsm );
}

/*  Stop the state machine */
void nn_sofi_out_stop (struct nn_sofi_out *self)
{
    _ofi_debug("OFI[o]: Stopping Output FSM\n");

    /* Handle stop according to state */
    switch (self->state) {

        /* This cases are safe to stop right away */
        case NN_SOFI_OUT_STATE_IDLE:
        case NN_SOFI_OUT_STATE_READY:

            /* These are safe to become 'closed' */
            _ofi_debug("OFI[o]: Switching state=%i to closed\n", self->state);
            self->state = NN_SOFI_OUT_STATE_CLOSED;

        case NN_SOFI_OUT_STATE_CLOSED:
        case NN_SOFI_OUT_STATE_ERROR:

            /* We are safe to stop right away */
            _ofi_debug("OFI[o]: Stopping right away\n");
            nn_fsm_stop( &self->fsm );
            break;

        /* Sending state switches to abording */
        case NN_SOFI_OUT_STATE_SENDING:

            /* Start timeout and start abording */
            _ofi_debug("OFI[o]: Switching to ABORTING state\n");
            self->state = NN_SOFI_OUT_STATE_ABORTING;
            nn_timer_start (&self->timer_abort, NN_SOFI_OUT_TIMEOUT_ABORT);
            break;

        /* Critical states, don't do anything here, we'll stop in the end */
        case NN_SOFI_OUT_STATE_ABORTING:
        case NN_SOFI_OUT_STATE_ABORT_TIMEOUT:
        case NN_SOFI_OUT_STATE_ABORT_CLEANUP:
            _ofi_debug("OFI[o]: Ignoring STOP command\n");
            break;

    };

}

/* ============================== */
/*        EXTERNAL EVENTS         */
/* ============================== */

/**
 * Trigger a tx event
 */
void nn_sofi_out_tx_event( struct nn_sofi_out *self, 
    struct fi_cq_data_entry * cq_entry )
{
    nn_mutex_lock(&self->mutex_cntr);
    struct nn_ofi_mrm_chunk * chunk = 
        nn_cont (cq_entry->op_context, struct nn_ofi_mrm_chunk, context);
    _ofi_debug("OFI[o]: Got CQ event for the sent frame, ctx=%p\n", chunk);

    /* Unlock mrm chunk */
    nn_ofi_mrm_unlock( &self->mrm, chunk );

    /* Hack to have only one worker task in queue */
    NN_SOFI_OUT_SCHED_CNTR( self, cntr_task_tx );
    nn_mutex_unlock(&self->mutex_cntr);
}

/**
 * Trigger a tx error event
 */
void nn_sofi_out_tx_error_event( struct nn_sofi_out *self, 
    struct fi_cq_err_entry * cq_err )
{
    nn_mutex_lock(&self->mutex_cntr);
    struct nn_ofi_mrm_chunk * chunk = 
        nn_cont (cq_err->op_context, struct nn_ofi_mrm_chunk, context);
    _ofi_debug("OFI[o]: Got CQ event for the sent frame, ctx=%p\n", chunk);

    /* Unlock mrm chunk */
    nn_ofi_mrm_unlock( &self->mrm, chunk );

    /* Keep error */
    self->error = cq_err->err;

    /* Trigger worker task */
    NN_SOFI_OUT_SCHED_CNTR( self, cntr_task_tx_error );
    nn_mutex_unlock(&self->mutex_cntr);
}

/**
 * Trigger the transmission of a packet
 */
int nn_sofi_out_tx_event_send( struct nn_sofi_out *self, struct nn_msg *msg )
{
    int ret;
    int iov_count;
    size_t sz_sphdr, sz_hdrs;
    struct nn_ofi_mrm_chunk * chunk;
    nn_mutex_lock(&self->mutex_cntr);

    /* We can only send if we are in POSTED or SENDING state */
    if ( ( self->state != NN_SOFI_OUT_STATE_READY ) &&
         ( self->state != NN_SOFI_OUT_STATE_SENDING ) ) {
        nn_mutex_unlock(&self->mutex_cntr);
        return -ETERM;
    }

    /* Acquire an MRM lock & get pointers */
    ret = nn_ofi_mrm_lock( &self->mrm, &chunk, &msg->body );
    if (ret) {
        nn_mutex_unlock(&self->mutex_cntr);
        return ret;
    }

    /* Populate size fields */
    chunk->data.mr_iov[1].iov_len = nn_chunkref_size( &msg->body );
    sz_hdrs = nn_chunkref_size( &msg->hdrs );
    sz_sphdr = nn_chunkref_size( &msg->sphdr );

    _ofi_debug("OFI[o]: Sending frame sphdr=%lu, hdr=%lu, body=%lu\n",
        sz_sphdr, sz_hdrs, chunk->data.mr_iov[1].iov_len);

    /* If no header exists, skip population of iov[0] */
    if (sz_hdrs + sz_sphdr == 0) {

        /* Move iov[1] to iov[0] */
        chunk->data.mr_iov[0].iov_base = chunk->data.mr_iov[1].iov_base;
        chunk->data.mr_iov[0].iov_len = chunk->data.mr_iov[1].iov_len;
        chunk->data.mr_desc[0] = chunk->data.mr_desc[1];

        /* We have 1 iov */
        iov_count = 1;

    } else {

        /* Copy headers in the ancillary buffer */
        nn_assert( (sz_hdrs + sz_sphdr) <= NN_OFI_MRM_ANCILLARY_SIZE );
        chunk->data.mr_iov[0].iov_len = sz_hdrs + sz_sphdr;
        memcpy( chunk->data.mr_iov[0].iov_base, nn_chunkref_data( &msg->sphdr ), sz_sphdr );
        memcpy( chunk->data.mr_iov[0].iov_base + sz_sphdr, 
            nn_chunkref_data( &msg->hdrs ), sz_hdrs );

        /* We have 2 iovs */
        iov_count = 2;

    }

    /* Increment body reference counter so it doesn't get disposed */
    nn_chunk_addref( nn_chunkref_getchunk(&msg->body), 1 );

    /* Prepare fi_msg */
    struct fi_msg egress_msg = {
        .msg_iov = (struct iovec*) &chunk->data.mr_iov,
        .iov_count = iov_count,
        .desc = (void**)&chunk->data.mr_desc,
        .addr = self->ep->remote_fi_addr,
        .context = &chunk->context,
        .data = 0
    };

    /* Send data and return when buffer can be reused */
    _ofi_debug("OFI[o]: Send context=%p\n", chunk);
    _ofi_debug("OFI[o] ### POSTING SEND BUFFER len=%lu\n", egress_msg.msg_iov[0].iov_len);
    ret = fi_sendmsg( self->ep->ep, &egress_msg, FI_INJECT_COMPLETE);
    if (ret) {

        /* If we are in a bad state, we were remotely disconnected */
        if (ret == -FI_EOPBADSTATE) {
            _ofi_debug("OFI[H]: ofi_tx_msg() returned -FI_EOPBADSTATE, "
                "considering shutdown.\n");
            nn_mutex_unlock(&self->mutex_cntr);
            return -EINTR;
        }

        /* Otherwise display error */
        FT_PRINTERR("ofi_tx_msg", ret);
        nn_mutex_unlock(&self->mutex_cntr);
        return -EBADF;
    }

    /* Notify FSM that a send confirmation must be sent */
    NN_SOFI_OUT_SCHED_CNTR( self, cntr_task_tx_send );
    self->state = NN_SOFI_OUT_STATE_SENDING;

    /* We are good */
    nn_mutex_unlock(&self->mutex_cntr);
    return 0;

}

/**
 * Synchronous (blocking) tx request 
 */
size_t nn_sofi_out_tx( struct nn_sofi_out *self, void * ptr, 
    size_t max_sz, int timeout )
{
    struct fi_cq_data_entry entry;
    int ret;

    _ofi_debug("OFI[o]: Blocking TX max_sz=%lu, timeout=%i\n", max_sz, timeout);

    /* Check if message does not fit in the buffer */
    if (max_sz > NN_OFI_SMALLMR_SIZE)
        return -ENOMEM;

    /* Move data to small MR */
    memcpy( self->small_ptr, ptr, max_sz );

    /* Send data */
    _ofi_debug("OFI[o] ### POSTING SEND BUFFER len=%lu\n", max_sz);
    ret = fi_send( self->ep->ep, self->small_ptr, max_sz,
        fi_mr_desc(self->small_mr), self->ep->remote_fi_addr,
        &self->context );
    if (ret) {
        FT_PRINTERR("fi_send", ret);
        return ret;
    }

    /* Wait for synchronous event */
    ret = fi_cq_sread( self->ep->tx_cq, &entry, 1, NULL, timeout );
    if ((ret == 0) || (ret == -FI_EAGAIN)) {
        _ofi_debug("OFI[o]: Tx operation timed out\n");
        return -ETIMEDOUT;
    }
    if (ret < 0) {

        /* read CQ Error */
        struct fi_cq_err_entry err_entry;
        ret = fi_cq_readerr(self->ep->rx_cq, &err_entry, 0);
        _ofi_debug("OFI[o]: %s (%s)\n",
            fi_strerror(err_entry.err),
            fi_cq_strerror(self->ep->rx_cq, err_entry.prov_errno, err_entry.err_data, NULL, 0)
        );

        /* Return CQ Error */
        return -err_entry.prov_errno;

    }

    _ofi_debug("OFI[o]: Blocking TX completed\n");

    /* Success */
    return 0;

}

/* ============================== */
/*          FSM HANDLERS          */
/* ============================== */

/**
 * SHUTDOWN State Handler
 */
static void nn_sofi_out_shutdown (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{
    /* Get pointer to sofi structure */
    struct nn_sofi_out *self;
    self = nn_cont (fsm, struct nn_sofi_out, fsm);

    /* If this is part of the FSM action, start shutdown */
    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {

        /* Stop FSM, triggering a final event according to state */
        if (self->state == NN_SOFI_OUT_STATE_CLOSED) {
            _ofi_debug("OFI[o]: Stopping FSM with CLOSE event\n");
            nn_fsm_stopped(&self->fsm, NN_SOFI_OUT_EVENT_CLOSE);

        } else if (self->state == NN_SOFI_OUT_STATE_ERROR) {
            _ofi_debug("OFI[o]: Stopping FSM with ERROR (error=%i) "
                "event\n", self->error);
            nn_fsm_stopped(&self->fsm, NN_SOFI_OUT_EVENT_ERROR);

        } else {
            nn_fsm_bad_state (self->state, src, type);    

        }

        return;

    }

    /* Invalid state */
    nn_fsm_bad_state (self->state, src, type);

}

/**
 * ACTIVE State Handler
 */
static void nn_sofi_out_handler (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{

    /* Get pointer to sofi structure */
    struct nn_sofi_out *self;
    self = nn_cont (fsm, struct nn_sofi_out, fsm);
    _ofi_debug("OFI[o]: nn_sofi_out_handler state=%i, src=%i, type=%i\n", 
        self->state, src, type);

    /* Handle state transitions */
    switch (self->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_SOFI_OUT_STATE_IDLE:
        switch (src) {

        /* ========================= */
        /*  FSM Action               */
        /* ========================= */
        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:

                /* That's the first acknowledement event */
                self->state = NN_SOFI_OUT_STATE_READY;
                nn_fsm_raise(&self->fsm, &self->event_started, 
                    NN_SOFI_OUT_EVENT_STARTED);

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  READY state.                                                              */
/*  We are ready to send data to the remote endpoint.                         */
/******************************************************************************/
    case NN_SOFI_OUT_STATE_READY:
        switch (src) {

        /* ========================= */
        /*  TASK: Task counter tick  */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TASK_CNTR:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                nn_assert( self->cntr_task_tx == 0 );
                nn_assert( self->cntr_task_tx_send == 0 );
                nn_assert( self->cntr_task_tx_error == 0 );
                nn_assert( self->cntr_event_send == 0 );
                nn_assert( self->cntr_pending == 0 );

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

        /* Intermediate state, should not be reachable */
        // nn_fsm_bad_source (self->state, src, type);

/******************************************************************************/
/*  SENDING state.                                                            */
/*  There are outgoing data on the NIC, wait until there are no more data     */
/*  pending or a timeout occurs.                                              */
/******************************************************************************/
    case NN_SOFI_OUT_STATE_SENDING:
        switch (src) {

        /* ========================= */
        /*  TASK: Task counter tick  */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TASK_CNTR:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                nn_mutex_lock(&self->mutex_cntr);
                if (self->cntr_pending > 0) {

                    /* =================================== */
                    /*  Tx CQ Event                        */
                    if (self->cntr_task_tx > 0) {
                    /* =================================== */
                        _ofi_debug("OFI[o]: << cntr_task_tx <<\n");
                        self->cntr_task_tx--;
                        self->cntr_pending--;

                        /* Notify that data are sent */
                        _ofi_debug("OFI[o]: Acknowledged sent event\n");

                        /* If there are send() requests left, send them now */
                        if (self->pend_sent) {

                            /* Check for late send events */
                            _ofi_debug("OFI[o]: Pending %i sends\n", self->pend_sent);
                            self->pend_sent--;

                            /* Send delayed send event */
                            _ofi_debug("OFI[o]: Delayed return from send()\n");
                            nn_fsm_raise(&self->fsm, &self->event_sent, 
                                NN_SOFI_OUT_EVENT_SENT);

                        }
                    }

                    /* =================================== */
                    /*  Tx Error Event                     */
                    if (self->cntr_task_tx_error > 0) {
                    /* =================================== */
                        _ofi_debug("OFI[o]: << cntr_task_tx_error <<\n");
                        self->cntr_task_tx_error--;
                        self->cntr_pending--;

                        /* There was an acknowledgement error */
                        _ofi_debug("OFI[o]: Error acknowledging the send event\n");
                        self->state = NN_SOFI_OUT_STATE_ERROR;
                        nn_sofi_out_stop( self );

                        /* Do not do anything */
                        return;

                    }

                    /* =================================== */
                    /*  Tx Send Request Event              */
                    if (self->cntr_task_tx_send > 0) {
                    /* =================================== */
                        _ofi_debug("OFI[o]: << cntr_task_tx_send <<\n");
                        self->cntr_task_tx_send--;
                        self->cntr_pending--;

                        /* Send acknowledgement if mrm are not full */
                        if (nn_ofi_mrm_isfull(&self->mrm)) {
                            _ofi_debug("OFI[o]: Delaying send() return MRM found free\n");
                            self->pend_sent++;
                        } else {
                            _ofi_debug("OFI[o]: Returning from send()\n");
                            nn_fsm_raise(&self->fsm, &self->event_sent, 
                                NN_SOFI_OUT_EVENT_SENT);
                        }
                    }

                    /* Check if we should re-schedule */
                    if (self->cntr_pending) {
                        _ofi_debug("OFI[o]: More counters pending, queuing-up\n");
                        nn_worker_execute ((self)->worker, &(self)->task_cntr);
                    }

                }

                /* If there are no pending tasks, exit now */
                _ofi_debug("OFI[o]: CNTR: (tx=%i, tx_send=%i, tx_error=%i)\n",
                    self->cntr_task_tx, self->cntr_task_tx_send, self->cntr_task_tx_error );
                if ((self->cntr_pending == 0) && nn_ofi_mrm_isidle(&self->mrm)) {
                    _ofi_debug("OFI[o]: No pending CNTRs and all MRMs are idle\n");
                    self->state = NN_SOFI_OUT_STATE_READY;
                }

                nn_mutex_unlock(&self->mutex_cntr);
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  ABORTING state.                                                           */
/*  There was a pending acknowledgement but we were requested to abort, so    */
/*  wait for ack or error (or timeout) events.                                */
/******************************************************************************/
    case NN_SOFI_OUT_STATE_ABORTING:
        switch (src) {

        /* ========================= */
        /*  TASK: Task counter tick  */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TASK_CNTR:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                nn_mutex_lock(&self->mutex_cntr);
                while (self->cntr_pending > 0) {

                    /* =================================== */
                    /*  Tx CQ Event                        */
                    if (self->cntr_task_tx > 0) {
                    /* =================================== */
                        self->cntr_task_tx--;

                        /* Notify that data are sent */
                        _ofi_debug("OFI[o]: Acknowledged sent event, cleaning-up\n");

                        /* Check for idle MRM */
                        if (nn_ofi_mrm_isidle(&self->mrm)) {
                            _ofi_debug("OFI[o]: MRM is idle, cleaning-up\n");

                            /* We got acknowledgement, continue with abort cleanup */
                            self->state = NN_SOFI_OUT_STATE_ABORT_CLEANUP;
                            nn_timer_stop (&self->timer_abort);
                            return;

                        };

                    /* =================================== */
                    /*  Tx Error Event                     */
                    } else if (self->cntr_task_tx_error > 0) {
                    /* =================================== */
                        self->cntr_task_tx_error--;

                        /* Send error, cleanup */
                        self->state = NN_SOFI_OUT_STATE_ABORT_CLEANUP;
                        nn_timer_stop (&self->timer_abort);
                        return;

                    }

                    self->cntr_pending--;
                }
                nn_mutex_unlock(&self->mutex_cntr);
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }


        /* ========================= */
        /*  Abort Timer Timeout      */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* Stop timer, there was an error */
                _ofi_debug("OFI[o]: Acknowledgement timed out, waiting for "
                            "timer to stop\n");
                self->state = NN_SOFI_OUT_STATE_ABORT_TIMEOUT;
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
    case NN_SOFI_OUT_STATE_ABORT_TIMEOUT:
        switch (src) {

        /* ========================= */
        /*  Abort Timer Timeout      */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TIMER:
            switch (type) {
            case NN_TIMER_STOPPED:

                /* Timer stopped, but there was an error */
                _ofi_debug("OFI[o]: Timeout timer stopped, going for error\n");
                self->state = NN_SOFI_OUT_STATE_ERROR;
                nn_sofi_out_stop( self );

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
    case NN_SOFI_OUT_STATE_ABORT_CLEANUP:
        switch (src) {

        /* ========================= */
        /*  Abort Timer Timeout      */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TIMER:
            switch (type) {
            case NN_TIMER_STOPPED:

                /* Timer stopped, and we are good */
                _ofi_debug("OFI[o]: Timeout timer stopped, closed\n");
                self->state = NN_SOFI_OUT_STATE_CLOSED;
                nn_sofi_out_stop( self );

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


