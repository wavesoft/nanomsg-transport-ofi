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

#include "../../utils/alloc.h"
#include "../../utils/cont.h"
#include "../../utils/err.h"
#include "../../utils/fast.h"

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
#define NN_SOFI_OUT_SRC_TASK_TX         3102
#define NN_SOFI_OUT_SRC_TASK_TX_ERROR   3103
#define NN_SOFI_OUT_SRC_TASK_TX_SEND    3104

/* Timeout values */
#define NN_SOFI_OUT_TIMEOUT_ABORT       1000

/* Forward Declarations */
static void nn_sofi_out_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_out_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);

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
    _ofi_debug("OFI[o]: Initializing Output FSM\n");

    /* Initialize properties */
    self->state = NN_SOFI_OUT_STATE_IDLE;
    self->error = 0;
    self->ofi = ofi;
    self->ep = ep;

    /* Initialize events */
    nn_fsm_event_init (&self->event_started);
    nn_fsm_event_init (&self->event_sent);

    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_out_handler, nn_sofi_out_shutdown,
        src, self, owner);

    /*  Choose a worker thread to handle this socket. */
    self->worker = nn_fsm_choose_worker (&self->fsm);

    /* Initialize worker tasks */
    nn_worker_task_init (&self->task_tx_send, NN_SOFI_OUT_SRC_TASK_TX_SEND,
        &self->fsm);
    nn_worker_task_init (&self->task_tx, NN_SOFI_OUT_SRC_TASK_TX,
        &self->fsm);
    nn_worker_task_init (&self->task_tx_error, NN_SOFI_OUT_SRC_TASK_TX_ERROR,
        &self->fsm);

    /* Initialize timer */
    nn_timer_init(&self->timer_abort, NN_SOFI_OUT_SRC_TIMER, 
        &self->fsm);

    /* Manage a new MR for small blocking calls */
    ofi_mr_init( ep, &self->mr_small );
    ofi_mr_manage( ep, &self->mr_small, 
        nn_alloc(NN_OFI_SMALLMR_SIZE, "mr_small"), 
        NN_OFI_SMALLMR_SIZE,
        NN_SOFI_OUT_MR_SMALL, MR_RECV );

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
    nn_free( self->mr_small.ptr );
    ofi_mr_free( self->ep, &self->mr_small );

    /* Free MRM */
    nn_ofi_mrm_term( &self->mrm );

    /* Abort timer */
    nn_timer_term (&self->timer_abort);

    /* Cleanup events */
    nn_fsm_event_term (&self->event_started);
    nn_fsm_event_term (&self->event_sent);

    /* Cleanup worker tasks */
    nn_worker_cancel (self->worker, &self->task_tx_error);
    nn_worker_cancel (self->worker, &self->task_tx_send);
    nn_worker_cancel (self->worker, &self->task_tx);
    nn_worker_task_term (&self->task_tx_error);
    nn_worker_task_term (&self->task_tx_send);
    nn_worker_task_term (&self->task_tx);

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
    struct nn_ofi_mrm_chunk * chunk = 
        nn_cont (cq_entry->op_context, struct nn_ofi_mrm_chunk, context);
    _ofi_debug("OFI[o]: Got CQ event for the sent frame, ctx=%p\n", chunk);

    /* Unlock mrm chunk */
    nn_ofi_mrm_unlock( chunk );

    /* Trigger worker task */
    nn_worker_execute (self->worker, &self->task_tx);
}

/**
 * Trigger a tx error event
 */
void nn_sofi_out_tx_error_event( struct nn_sofi_out *self, 
    struct fi_cq_err_entry * cq_err )
{
    struct nn_ofi_mrm_chunk * chunk = 
        nn_cont (cq_err->op_context, struct nn_ofi_mrm_chunk, context);
    _ofi_debug("OFI[o]: Got CQ event for the sent frame, ctx=%p\n", chunk);

    /* Unlock mrm chunk */
    nn_ofi_mrm_unlock( chunk );

    /* Keep error */
    self->error = cq_err->err;

    /* Trigger worker task */
    nn_worker_execute (self->worker, &self->task_tx_error);
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
    struct iovec iov[2];
    void * desc[2];

    /* We can only send if we are in POSTED or SENDING state */
    if ( ( self->state != NN_SOFI_OUT_STATE_READY ) &&
         ( self->state != NN_SOFI_OUT_STATE_SENDING ) ) {
        return -ETERM;
    }

    /* Populate size fields */
    iov[1].iov_len = nn_chunkref_size( &msg->body );
    sz_hdrs = nn_chunkref_size( &msg->hdrs );
    sz_sphdr = nn_chunkref_size( &msg->sphdr );

    _ofi_debug("OFI[o]: Sending frame sphdr=%lu, hdr=%lu, body=%lu\n",
        sz_sphdr, sz_hdrs, iov[1].iov_len);

    /* Acquire an MRM lock & get pointers */
    ret = nn_ofi_mrm_lock( &self->mrm, &chunk, &msg->body,
        &iov[1].iov_base, &desc[1], 
        &iov[0].iov_base, &desc[0] );
    if (ret) return ret;

    /* If no header exists, skip population of iov[0] */
    if (sz_hdrs + sz_sphdr == 0) {

        /* Move iov[1] to iov[0] */
        iov[0].iov_base = iov[1].iov_base;
        iov[0].iov_len = iov[1].iov_len;

        /* We have 1 iov */
        iov_count = 1;

    } else {

        /* Copy headers in the ancillary buffer */
        nn_assert( (sz_hdrs + sz_sphdr) <= NN_OFI_MRM_ANCILLARY_SIZE );
        iov[0].iov_len = sz_hdrs + sz_sphdr;
        memcpy( iov[0].iov_base, nn_chunkref_data( &msg->sphdr ), sz_sphdr );
        memcpy( iov[0].iov_base + sz_sphdr, 
            nn_chunkref_data( &msg->hdrs ), sz_hdrs );

        /* We have 2 iovs */
        iov_count = 2;

    }

    /* Increment body reference counter so it doesn't get disposed */
    nn_chunk_addref( nn_chunkref_getchunk(&msg->body), 1 );

    /* Prepare fi_msg */
    struct fi_msg egress_msg = {
        .msg_iov = iov,
        .iov_count = iov_count,
        .desc = desc,
        .addr = self->ep->remote_fi_addr,
        .context = &chunk->context,
        .data = 0
    };

    /* We are now sending */
    self->state = NN_SOFI_OUT_STATE_SENDING;

    /* Send data and return when buffer can be reused */
    _ofi_debug("OFI[o]: Send context=%p\n", chunk);
    ret = fi_sendmsg( self->ep->ep, &egress_msg, FI_INJECT_COMPLETE);
    if (ret) {

        /* If we are in a bad state, we were remotely disconnected */
        if (ret == -FI_EOPBADSTATE) {
            _ofi_debug("OFI[H]: ofi_tx_msg() returned -FI_EOPBADSTATE, "
                "considering shutdown.\n");
            return -EINTR;           
        }

        /* Otherwise display error */
        FT_PRINTERR("ofi_tx_msg", ret);
        return -EBADF;
    }

    /* Trigger worker task */
    // nn_worker_execute (self->worker, &self->task_tx_send);

    /* We are good */
    return 0;
}

/**
 * Synchronous (blocking) tx request 
 */
size_t nn_sofi_out_tx( struct nn_sofi_out *self, void * ptr, 
    size_t max_sz, int timeout )
{
    struct fi_context context;
    struct fi_cq_data_entry entry;
    int ret;

    _ofi_debug("OFI[o]: Blocking TX max_sz=%lu, timeout=%i\n", max_sz, timeout);

    /* Check if message does not fit in the buffer */
    if (max_sz > NN_OFI_SMALLMR_SIZE)
        return -ENOMEM;

    /* Move data to small MR */
    memcpy( self->mr_small.ptr, ptr, max_sz );

    /* Send data */
    ret = fi_send( self->ep->ep, self->mr_small.ptr, max_sz,
        OFI_MR_DESC(self->mr_small), self->ep->remote_fi_addr,
        &context );
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
        FT_PRINTERR("fi_cq_sread", ret);
        return ret;
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

        /* This is never handled, that's an intermediate state */    
        nn_fsm_bad_state (self->state, src, type);

/******************************************************************************/
/*  SENDING state.                                                            */
/*  There are outgoing data on the NIC, wait until there are no more data     */
/*  pending or a timeout occurs.                                              */
/******************************************************************************/
    case NN_SOFI_OUT_STATE_SENDING:
        switch (src) {

        /* ========================= */
        /*  TASK: Send Completed     */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TASK_TX:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Notify that data are sent */
                _ofi_debug("OFI[o]: Acknowledged sent event\n");
                nn_fsm_raise(&self->fsm, &self->event_sent, 
                    NN_SOFI_OUT_EVENT_SENT);

                /* Wait until all mrm buffers are unlocked, meaning
                   that there are no pending outgoing operations. */
                if (!nn_ofi_mrm_isidle(&self->mrm)) {
                    _ofi_debug("OFI[o]: MRM not idle, waiting for it\n");
                    return;
                }
                self->state = NN_SOFI_OUT_STATE_READY;
                nn_sofi_out_stop( self );

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  TASK: Send Error         */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TASK_TX_ERROR:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* There was an acknowledgement error */
                _ofi_debug("OFI[o]: Error acknowledging the send event\n");
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
/*  ABORTING state.                                                           */
/*  There was a pending acknowledgement but we were requested to abort, so    */
/*  wait for ack or error (or timeout) events.                                */
/******************************************************************************/
    case NN_SOFI_OUT_STATE_ABORTING:
        switch (src) {

        /* ========================= */
        /*  TASK: Send Acknowledged  */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TASK_TX:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Notify that data are sent */
                _ofi_debug("OFI[o]: Acknowledged sent event, cleaning-up\n");
                nn_fsm_raise(&self->fsm, &self->event_sent, 
                    NN_SOFI_OUT_EVENT_SENT);

                /* Wait until all mrm buffers are unlocked, meaning
                   that there are no pending outgoing operations. */
                if (!nn_ofi_mrm_isidle(&self->mrm)) {
                    _ofi_debug("OFI[o]: MRM not idle, waiting for it\n");
                    return;
                }

                /* We got acknowledgement, continue with abort cleanup */
                self->state = NN_SOFI_OUT_STATE_ABORT_CLEANUP;
                nn_timer_stop (&self->timer_abort);

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  TASK: Send Error         */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TASK_TX_ERROR:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* We got acknowledgement, continue with abort cleanup */
                _ofi_debug("OFI[o]: Acknowledged sent event\n");
                self->state = NN_SOFI_OUT_STATE_ABORT_CLEANUP;
                nn_timer_stop (&self->timer_abort);

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


