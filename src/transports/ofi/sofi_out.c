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
#define NN_SOFI_OUT_STATE_POSTED        3003
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
void nn_sofi_out_init (struct nn_sofi_out *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep,
    const uint8_t ng_direction, struct nn_pipebase * pipebase,
    int src, struct nn_fsm *owner)
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
    _ofi_debug("OFI[o]: Stopping Input FSM\n");

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

        /* Pending state switches to abording */
        case NN_SOFI_OUT_STATE_POSTED:

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


    _ofi_debug("OFI[o]: Stopping Output FSM\n");
    nn_fsm_stop( &self->fsm );
}

/* ============================== */
/*        EXTERNAL EVENTS         */
/* ============================== */

/**
 * Trigger a tx event
 */
void nn_sofi_out_tx_event( struct nn_sofi_out *self )
{
    /* Trigger worker task */
    nn_worker_execute (self->worker, &self->task_tx);
}

/**
 * Trigger a tx error event
 */
void nn_sofi_out_tx_error_event( struct nn_sofi_out *self, int err_number )
{
    /* Trigger worker task */
    nn_worker_execute (self->worker, &self->task_tx_error);
}

/**
 * Trigger the transmission of a packet
 */
void nn_sofi_out_tx_event_send( struct nn_sofi_out *self )
{
    /* Trigger worker task */
    nn_worker_execute (self->worker, &self->task_tx_send);
}

/**
 * Synchronous (blocking) tx request 
 */
size_t nn_sofi_out_tx( struct nn_sofi_out *self, void * ptr, 
    size_t max_sz, int timeout )
{
    int ret;

    /* Check if message does not fit in the buffer */
    if (max_sz > NN_OFI_SMALLMR_SIZE)
        return -ENOMEM;

    /* Move data to small MR */
    memcpy( self->mr_small.ptr, ptr, max_sz );

    /* Receive data synchronously */
    ret = ofi_tx_data( self->ep, self->mr_small.ptr, max_sz, 
        OFI_MR_DESC(self->mr_small), timeout );

    /* Return result */
    return ret;
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
            _ofi_debug("OFI[o]: Stopping FSM with ERROR event\n");
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
        switch (src) {

        /* ========================= */
        /*  TASK: Send Data          */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TASK_TX_SEND:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* We have posted the buffers */
                self->state = NN_SOFI_OUT_STATE_POSTED;

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  POSTED state.                                                             */
/*  Data are posted to the NIC, we are waiting for a send acknowledgement     */
/*  or for a send error.                                                      */
/******************************************************************************/
    case NN_SOFI_OUT_STATE_POSTED:
        switch (src) {

        /* ========================= */
        /*  TASK: Send Completed     */
        /* ========================= */
        case NN_SOFI_OUT_SRC_TASK_TX:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* The posted buffers are sent */
                self->state = NN_SOFI_OUT_STATE_READY;

                /* Notify that data are sent */
                _ofi_debug("OFI[o]: Acknowledged sent event\n");
                nn_fsm_raise(&self->fsm, &self->event_sent, 
                    NN_SOFI_OUT_EVENT_SENT);

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

                /* We got acknowledgement, continue with abort cleanup */
                _ofi_debug("OFI[o]: Acknowledged sent event\n");
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


