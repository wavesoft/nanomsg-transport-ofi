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

/* FSM States */
#define NN_SOFI_IN_STATE_IDLE           2001
#define NN_SOFI_IN_STATE_POSTED         2002
#define NN_SOFI_IN_STATE_PROCESSING     2003
#define NN_SOFI_IN_STATE_ERROR          2004
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

/* Forward Declarations */
static void nn_sofi_in_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_in_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);

/* ============================== */
/*       HELPER FUNCTIONS         */
/* ============================== */

/* Post buffers */
static int nn_sofi_in_post_buffers(struct nn_sofi_in *self)
{
    /* Successful */
    return 0;
}

/* Move buffers for pick-up by SOFI */
static void nn_sofi_in_move_buffers(struct nn_sofi_in *self)
{

}

/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/**
 * Initialize the state machine 
 */
void nn_sofi_in_init (struct nn_sofi_in *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep,
    const uint8_t ng_direction, struct nn_pipebase * pipebase,
    int src, struct nn_fsm *owner)
{
    _ofi_debug("OFI[i]: Initializing Input FSM\n");

    /* Reset properties */
    self->state = NN_SOFI_IN_STATE_IDLE;
    self->error = 0;
    self->ofi = ofi;
    self->ep = ep;

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

    /* Initialize timer */
    nn_timer_init(&self->timer_abort, NN_SOFI_IN_SRC_TIMER, 
        &self->fsm);

    /* Manage a new MR for small blocking calls */
    ofi_mr_init( ep, &self->mr_small );
    ofi_mr_manage( ep, &self->mr_small, 
        nn_alloc(NN_OFI_SMALLMR_SIZE, "mr_small"), 
        NN_OFI_SMALLMR_SIZE,
        NN_SOFI_IN_MR_SMALL, MR_RECV );

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
        case NN_SOFI_IN_STATE_POSTED:

            /* These are safe to become 'closed' */
            _ofi_debug("OFI[i]: Switching state=%i to closed\n", self->state);
            self->state = NN_SOFI_IN_STATE_CLOSED;

        case NN_SOFI_IN_STATE_CLOSED:
        case NN_SOFI_IN_STATE_ERROR:

            /* We are safe to stop right away */
            _ofi_debug("OFI[i]: Stopping right away\n");
            nn_fsm_stop( &self->fsm );
            break;

        /* Processing switches to abording */
        case NN_SOFI_IN_STATE_PROCESSING:

            /* Start timeout and start abording */
            _ofi_debug("OFI[i]: Switching to ABORTING state\n");
            self->state = NN_SOFI_IN_STATE_ABORTING;
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
void nn_sofi_in_rx_event( struct nn_sofi_in *self )
{
    /* Trigger worker task */
    nn_worker_execute (self->worker, &self->task_rx);
}

/**
 * Trigger an rx error event
 */
void nn_sofi_in_rx_error_event( struct nn_sofi_in *self, int err_number )
{
    /* Trigger worker task */
    nn_worker_execute (self->worker, &self->task_rx_error);
}

/**
 * Acknowledge an rx event
 */
void nn_sofi_in_rx_error_ack( struct nn_sofi_in *self )
{
    /* Trigger worker task */
    nn_worker_execute (self->worker, &self->task_rx_ack);
}

/**
 * Synchronous (blocking) rx request
 */
size_t nn_sofi_in_rx( struct nn_sofi_in *self, void * ptr, 
    size_t max_sz, int timeout )
{
    size_t rx_size;
    int ret;

    /* Check if message does not fit in the buffer */
    if (max_sz > NN_OFI_SMALLMR_SIZE)
        return -ENOMEM;

    /* Receive data synchronously */
    ret = ofi_rx_data( self->ep, self->mr_small.ptr, max_sz, 
        OFI_MR_DESC(self->mr_small), &rx_size, timeout );

    /* Return on error */
    if (ret)
        return ret;

    /* Move data to ptr */
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
        if (self->state == NN_SOFI_IN_STATE_CLOSED) {
            _ofi_debug("OFI[i]: Stopping FSM with CLOSE event\n");
            nn_fsm_stopped(&self->fsm, NN_SOFI_IN_EVENT_CLOSE);

        } else if (self->state == NN_SOFI_IN_STATE_ERROR) {
            _ofi_debug("OFI[i]: Stopping FSM with ERROR event\n");
            nn_fsm_stopped(&self->fsm, NN_SOFI_IN_EVENT_ERROR);

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
                if (!nn_sofi_in_post_buffers( self )) {

                    /* When successful switch to POSTED state */
                    _ofi_debug("OFI[i]: Input buffers posted\n");
                    self->state = NN_SOFI_IN_STATE_POSTED;

                    /* That's the first acknowledement event */
                    nn_fsm_raise(&self->fsm, &self->event_started, 
                        NN_SOFI_IN_EVENT_STARTED);

                } else {

                    /* When unsuccessful, raise ERROR event */
                    _ofi_debug("OFI[i]: Error trying to post input buffers\n");
                    self->state = NN_SOFI_IN_STATE_ERROR;
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
/*  POSTED state.                                                             */
/*  We have posted the input buffers and we are waiting for events from the   */
/*  polling thread that runs in sofi. It triggers events through worker tasks */
/*  in order to traverse threads.                                             */
/******************************************************************************/
    case NN_SOFI_IN_STATE_POSTED:
        switch (src) {

        /* ========================= */
        /*  TASK : Rx Event          */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Move data to rx buffer */
                _ofi_debug("OFI[i]: Data Rx event\n");
                nn_sofi_in_move_buffers( self );

                /* Switch into processing */
                self->state = NN_SOFI_IN_STATE_PROCESSING;

                /* Notify SOFI for this fact */
                nn_fsm_raise(&self->fsm, &self->event_received, 
                    NN_SOFI_IN_EVENT_RECEIVED);

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
                self->state = NN_SOFI_IN_STATE_ERROR;
                nn_sofi_in_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  PROCESSING state.                                                         */
/*  We are waiting for an acknowledgement from SOFI when it's ready for us    */
/*  to reuse the rx buffers.                                                  */
/******************************************************************************/
    case NN_SOFI_IN_STATE_PROCESSING:
        switch (src) {

        /* ========================= */
        /*  TASK : Rx Ack Event      */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX_ACK:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Post buffers */
                _ofi_debug("OFI[i]: Acknowledged receive event\n");
                if (!nn_sofi_in_post_buffers( self )) {

                    /* When successful switch to POSTED state */
                    _ofi_debug("OFI[i]: Input buffers posted\n");
                    self->state = NN_SOFI_IN_STATE_POSTED;

                } else {

                    /* When unsuccessful, raise ERROR event */
                    _ofi_debug("OFI[i]: Error trying to post input buffers\n");
                    self->state = NN_SOFI_IN_STATE_ERROR;
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
/*  ABORTING state.                                                           */
/*  We are waiting for receive completion or for timeout events.              */
/******************************************************************************/
    case NN_SOFI_IN_STATE_ABORTING:
        switch (src) {

        /* ========================= */
        /*  TASK : Rx Ack Event      */
        /* ========================= */
        case NN_SOFI_IN_SRC_TASK_RX_ACK:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Stop timer, we are good to cleanup */
                _ofi_debug("OFI[i]: Acknowledged receive event, continuing "
                            "abort\n");
                self->state = NN_SOFI_IN_STATE_ABORT_CLEANUP;
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
                self->state = NN_SOFI_IN_STATE_ABORT_TIMEOUT;
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
                self->state = NN_SOFI_IN_STATE_ERROR;
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
                self->state = NN_SOFI_IN_STATE_CLOSED;
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


