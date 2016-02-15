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

#include <unistd.h>
#include <errno.h>
#include "../../ofi.h"
#include "ofi.h"
#include "sofi.h"

#include "../../utils/cont.h"

/* FSM States */
#define NN_SOFI_STATE_IDLE               1001
#define NN_SOFI_STATE_INIT_IN            1002
#define NN_SOFI_STATE_INIT_OUT           1003
#define NN_SOFI_STATE_HANDSHAKE_HALF     1004
#define NN_SOFI_STATE_HANDSHAKE_FULL     1005
#define NN_SOFI_STATE_HANDSHAKE_COMPLETE 1006
#define NN_SOFI_STATE_RUNNING            1007
#define NN_SOFI_STATE_CLOSING            1008
#define NN_SOFI_STATE_CLOSED             1009

/* FSM Sources */
#define NN_SOFI_SRC_IN_FSM              1101
#define NN_SOFI_SRC_OUT_FSM             1102
#define NN_SOFI_SRC_HANDSHAKE_TIMER     1103
#define NN_SOFI_SRC_KEEPALIVE_TIMER     1104

/* Timeout values */
#define NN_SOFI_TIMEOUT_HANDSHAKE       1000

/* Forward Declarations */
static void nn_sofi_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_poller_thread (void *arg);

/* Pipe definition */
static int nn_sofi_send (struct nn_pipebase *self, struct nn_msg *msg);
static int nn_sofi_recv (struct nn_pipebase *self, struct nn_msg *msg);
const struct nn_pipebase_vfptr nn_sofi_pipebase_vfptr = {
    nn_sofi_send,
    nn_sofi_recv
};

/* ============================== */
/*       HELPER FUNCTIONS         */
/* ============================== */

/**
 * Send handshake
 */
static void nn_sofi_send_handshake( struct nn_sofi *self )
{
    _ofi_debug("OFI[S]: Sending handshake\n");

    /* TODO: Send data */
}

/**
 * Complete handshake
 */
static void nn_sofi_handle_handshake( struct nn_sofi *self )
{
    _ofi_debug("OFI[S]: Handling handshake\n");

    /* TODO: Handle received data */
}

/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/*  Initialize the state machine */
void nn_sofi_init (struct nn_sofi *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep,
    const uint8_t ng_direction, int src, struct nn_fsm *owner)
{

    /* Keep references */
    self->ofi = ofi;
    self->ep = ep;
    self->ng_direction = ng_direction;

    /* ----------------------------------- */
    /*  libFabric/HLAPI Initialization     */
    /* ----------------------------------- */

    /* ----------------------------------- */
    /*  NanoMSG Core Initialization        */
    /* ----------------------------------- */

    /* Initialize list item */
    nn_list_item_init (&self->item);

    /* Initialize pipe base */
    nn_pipebase_init (&self->pipebase, &nn_sofi_pipebase_vfptr, epbase);

    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_handler, nn_sofi_shutdown,
        src, self, owner);

    /* Reset properties */
    self->state = NN_SOFI_STATE_IDLE;
    self->error = 0;

    /*  Choose a worker thread to handle this socket. */
    self->worker = nn_fsm_choose_worker (&self->fsm);

    /* Initialize worker tasks */
    nn_worker_task_init (&self->task_rx, NN_SOFI_TASK_RX,
        &self->fsm);
    nn_worker_task_init (&self->task_tx, NN_SOFI_TASK_TX,
        &self->fsm);
    nn_worker_task_init (&self->task_error, NN_SOFI_TASK_ERROR,
        &self->fsm);
    nn_worker_task_init (&self->task_disconnected, NN_SOFI_TASK_DISCONNECTED,
        &self->fsm);

    /* ----------------------------------- */
    /*  NanoMsg Component Initialization   */
    /* ----------------------------------- */

    nn_timer_init(&self->handshake_timer, NN_SOFI_SRC_HANDSHAKE_TIMER, 
        &self->fsm);
    nn_timer_init(&self->keepalive_timer, NN_SOFI_SRC_KEEPALIVE_TIMER,
        &self->fsm);

    /* ----------------------------------- */
    /*  OFI Sub-Component Initialization   */
    /* ----------------------------------- */

    /* Initialize INPUT Sofi */
    nn_sofi_in_init( &self->sofi_in, ofi, ep, direction, &self->pipebase,
        NN_SOFI_SRC_IN_FSM, &self->fsm );

    /* Initialize OUTPUT Sofi */
    nn_sofi_out_init( &self->sofi_out, ofi, ep, direction, &self->pipebase,
        NN_SOFI_SRC_OUT_FSM, &self->fsm );

    /* ----------------------------------- */
    /*  Bootstrap FSM                      */
    /* ----------------------------------- */

    /* Start FSM */
    _ofi_debug("OFI[S]: Starting FSM \n");
    nn_fsm_start (&self->fsm);

}

/**
 * Check if FSM is idle 
 */
int nn_sofi_isidle (struct nn_sofi *self)
{
    return nn_fsm_isidle (&self->fsm);
}

/**
 * Stop the state machine 
 */
void nn_sofi_stop (struct nn_sofi *self)
{
    /* Switch to closing */
    int fsm_state = self->state;
    self->state = NN_SOFI_STATE_CLOSING;

    /* Stop components according to state */
    switch (fsm_state) {
    case NN_SOFI_STATE_IDLE:
        _ofi_debug("OFI[S]: No components to stop\n");
        break;

    case NN_SOFI_STATE_INIT_IN:
        _ofi_debug("OFI[S]: Stopping Input FSM\n");
        nn_sofi_in_stop( &self->sofi_in );
        break;

    case NN_SOFI_STATE_INIT_OUT:
        _ofi_debug("OFI[S]: Stopping Input & Output FSM\n");
        nn_sofi_in_stop( &self->sofi_in );
        nn_sofi_out_stop( &self->sofi_out );
        break;

    case NN_SOFI_STATE_HANDSHAKE_HALF:
        _ofi_debug("OFI[S]: Stopping Handshake Timer, Input & Output FSM\n");
        nn_timer_stop( &self->handshake_timer );
        nn_sofi_in_stop( &self->sofi_in );
        nn_sofi_out_stop( &self->sofi_out );
        break;

    case NN_SOFI_STATE_HANDSHAKE_FULL:
        _ofi_debug("OFI[S]: Stopping Handshake Timer, Input & Output FSM\n");
        nn_timer_stop( &self->handshake_timer );
        nn_sofi_in_stop( &self->sofi_in );
        nn_sofi_out_stop( &self->sofi_out );
        break;

    case NN_SOFI_STATE_HANDSHAKE_COMPLETE:
        _ofi_debug("OFI[S]: Stopping Input & Output FSM\n");
        nn_sofi_in_stop( &self->sofi_in );
        nn_sofi_out_stop( &self->sofi_out );
        break;

    case NN_SOFI_STATE_RUNNING:
        _ofi_debug("OFI[S]: Stopping Keepalive Timer, Input & Output FSM\n");
        nn_timer_stop( &self->keepalive_timer );
        nn_sofi_in_stop( &self->sofi_in );
        nn_sofi_out_stop( &self->sofi_out );
        break;

    case NN_SOFI_STATE_CLOSING:
    case NN_SOFI_STATE_CLOSED:
        _ofi_debug("OFI[S]: No components to stop\n");
        break;

    }

    /* Stop FSM & Switch to shutdown handler */
    _ofi_debug("OFI[S]: Stopping core\n");
    nn_fsm_stop (&self->fsm);

}

/**
 * Cleanup the state machine
 */
void nn_sofi_term (struct nn_sofi *self)
{

    /* ----------------------------------- */
    /*  OFI Sub-Component Termination      */
    /* ----------------------------------- */

    /* Terminate sub-components */
    nn_sofi_in_term (&self->sofi_in);
    nn_sofi_out_term (&self->sofi_out);

    /* ----------------------------------- */
    /*  NanoMsg Component Termination      */
    /* ----------------------------------- */

    /* Stop timers */
    nn_timer_term (&self->handshake_timer);
    nn_timer_term (&self->keepalive_timer);

    /* ----------------------------------- */
    /*  NanoMSG Core Termination           */
    /* ----------------------------------- */

    /* Cleanup worker tasks */
    nn_worker_cancel (self->worker, &self->task_delayed_disconnect);
    nn_worker_cancel (self->worker, &self->task_disconnected);
    nn_worker_cancel (self->worker, &self->task_error);
    nn_worker_cancel (self->worker, &self->task_tx);
    nn_worker_cancel (self->worker, &self->task_rx);
    nn_worker_task_term (&self->task_delayed_disconnect);
    nn_worker_task_term (&self->task_disconnected);
    nn_worker_task_term (&self->task_error);
    nn_worker_task_term (&self->task_tx);
    nn_worker_task_term (&self->task_rx);

    /* Terminate list item */
    nn_list_item_term (&self->item);

    /* Cleanup components */
    nn_pipebase_term (&self->pipebase);
    nn_fsm_term (&self->fsm);

    /* ----------------------------------- */
    /*  libFabric/HLAPI Termination        */
    /* ----------------------------------- */

}

/* ============================== */
/*         WORKER THREAD          */
/* ============================== */

/**
 * The internal poller thread, since OFI does not 
 * have blocking UNIX file descriptors
 */
static void nn_sofi_poller_thread (void *arg)
{
    struct nn_sofi * self = (struct nn_sofi *) arg;
    struct fi_eq_cm_entry   eq_entry;
    struct fi_cq_err_entry  err_entry;
    struct fi_cq_data_entry cq_entry;
    uint8_t fastpoller = 200;
    uint32_t event;
    int ret;

    /* Keep thread alive while  */
    _ofi_debug("OFI[S]: Starting poller thread\n");
    while ( self->state == NN_SOFI_STATE_CONNECTED ) {

        /* ========================================= */
        /* Wait for Rx CQ event */
        /* ========================================= */
        ret = fi_cq_read( self->ep->rx_cq, &cq_entry, 1 );
        if (nn_slow(ret > 0)) {

        } else if (nn_slow(ret != -FI_EAGAIN)) {

        }

        /* ========================================= */
        /* Wait for Tx CQ event */
        /* ========================================= */
        ret = fi_cq_read( self->ep->tx_cq, &cq_entry, 1 );
        if (nn_slow(ret > 0)) {

        } else if (nn_slow(ret != -FI_EAGAIN)) {

        }

        /* ========================================= */
        /* Wait for EQ events */
        /* ========================================= */
        ret = fi_eq_read( self->ep->eq, &event, &eq_entry, sizeof eq_entry, 0);
        if (nn_fast(ret != -FI_EAGAIN)) {

        }

        /* Microsleep for lessen the CPU load */
        if (!--fastpoller) {
            usleep(10);
            fastpoller = 200;
        }

    }
    _ofi_debug("OFI[S]: Exited poller thread\n");

}

/* ============================== */
/*    INTERFACE IMPLEMENTATION    */
/* ============================== */

/**
 * This function is called by the nanomsg core when some data needs to be sent.
 * It's important to call the 'nn_pipebase_sent' function when ready!
 */
static int nn_sofi_send (struct nn_pipebase *pb, struct nn_msg *msg)
{
    int ret;
    struct nn_sofi *self;
    self = nn_cont (pb, struct nn_sofi, pipebase);

    /* TODO: Forward event to OUT FSM */

    /* Success */
    return 0;
}

/**
 * This function is called by the nanomsg core when some data needs to be sent.
 * This is triggered only when 'nn_pipebase_received' is called!
 */
static int nn_sofi_recv (struct nn_pipebase *pb, struct nn_msg *msg)
{
    int ret;
    struct nn_sofi *self;
    self = nn_cont (pb, struct nn_sofi, pipebase);

    /* TODO: Forward event to IN FSM */

    /* Success */
    return 0;
}

/**
 * SHUTDOWN State Handler
 */
static void nn_sofi_shutdown (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{
    /* Get pointer to sofi structure */
    struct nn_sofi *self;
    self = nn_cont (fsm, struct nn_sofi, fsm);

    /* If this is part of the FSM action, start shutdown */
    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {

    }

}

/**
 * ACTIVE State Handler
 */
static void nn_sofi_handler (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{

    /* Get pointer to sofi structure */
    struct nn_sofi *self;
    self = nn_cont (fsm, struct nn_sofi, fsm);

    /* Handle state transitions */
    switch (self->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_SOFI_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:

                /* Initialize input FSM */
                _ofi_debug("OFI[S]: Initializing OFI-Input\n");
                self->state = NN_SOFI_STATE_INIT_IN;
                nn_sofi_in_start( &self->sofi_in );

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  INIT_IN state.                                                            */
/*  We are waiting for a completion notification from the INPUT FSM, or for   */
/*  an error that will trigger the shutdown of the FSM.                       */
/******************************************************************************/
    case NN_SOFI_STATE_INIT_IN:
        switch (src) {

        case NN_SOFI_SRC_IN_FSM:
            switch (type) {
            case NN_SOFI_IN_EVENT_STARTED:

                /* Initialize output FSM */
                _ofi_debug("OFI[S]: Initializing OFI-Output\n");
                self->state = NN_SOFI_STATE_INIT_OUT;
                nn_sofi_out_start( &self->sofi_out );
                
                return;

            case NN_SOFI_IN_EVENT_ERROR:

                /* Unable to initialize input SOFI, shutdown */
                _ofi_debug("OFI[S]: Error while initializing OFI-Input\n");
                nn_sofi_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  INIT_OUT state.                                                           */
/*  We are waiting for a completion notification from the OUTPUT FSM, or for  */
/*  an error that will trigger the shutdown of the FSM.                       */
/*  Upon successful start of the output FSM this will send handshake and bing */
/*  FSM in half-handshake state.                                              */
/******************************************************************************/
    case NN_SOFI_STATE_INIT_OUT:
        switch (src) {

        case NN_SOFI_SRC_OUT_FSM:
            switch (type) {
            case NN_SOFI_OUT_EVENT_STARTED:

                /* Rx/Tx Initialized, start handshaking */
                _ofi_debug("OFI[S]: Performing handshake\n");
                self->state = NN_SOFI_STATE_HANDSHAKE_HALF;

                /* Require finite time for the handshake,
                   otherwise abort! */
                nn_timer_start( &self->handshake_timer, 
                    NN_SOFI_TIMEOUT_HANDSHAKE );

                /* Send handshake if we are on the sending side */
                if (self->ng_direction == NN_SOFI_NG_SEND) {
                    nn_sofi_send_handshake( self );
                }

                return;

            case NN_SOFI_OUT_EVENT_ERROR:

                /* Unable to initialize output SOFI, shutdown */
                _ofi_debug("OFI[S]: Error while initializing OFI-Output\n");
                nn_sofi_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  HANDSHAKE_HALF state.                                                     */
/*  We are waiting for a completion of the first phase of the handshake phase */
/******************************************************************************/
    case NN_SOFI_STATE_HANDSHAKE_HALF:
        switch (src) {

        case NN_SOFI_SRC_OUT_FSM:
            switch (type) {
            case NN_SOFI_OUT_EVENT_SENT:

                /* We were on the sending direction */
                _ofi_debug("OFI[S]: Completing handshake\n");
                nn_assert( self->ng_direction == NN_SOFI_NG_SEND );

                /* We are waiting for a receive event (or timeout) */
                self->state = NN_SOFI_STATE_HANDSHAKE_FULL;

                return;

            case NN_SOFI_IN_EVENT_ERROR:

                /* Unable to complete handshake, shutdown */
                _ofi_debug("OFI[S]: Send error while completing handshake\n");
                nn_sofi_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        case NN_SOFI_SRC_IN_FSM:
            switch (type) {
            case NN_SOFI_IN_EVENT_RECEIVED:

                /* Handle incoming phase of handshake */
                _ofi_debug("OFI[S]: Completing handshake\n");
                nn_assert( self->ng_direction == NN_SOFI_NG_RECV );

                /* Handle reception of data */
                nn_sofi_handle_handshake( self );

                /* Send data and wait for Tx event (or timeout) */
                nn_sofi_send_handshake( self );
                self->state = NN_SOFI_STATE_HANDSHAKE_FULL;

                return;

            case NN_SOFI_IN_EVENT_ERROR:

                /* Unable to complete handshake, shutdown */
                _ofi_debug("OFI[S]: Receive error while completing handshake\n");
                nn_sofi_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

        case NN_SOFI_SRC_HANDSHAKE_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* Unable to complete handshake, shutdown */
                _ofi_debug("OFI[S]: Handshake timed out!\n");
                nn_sofi_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  HANDSHAKE_FULL state.                                                     */
/*  We have already sent/received the first packet in the previous state, now */
/*  it's time to complete our part of the negotiation.                        */
/******************************************************************************/
    case NN_SOFI_STATE_HANDSHAKE_FULL:
        switch (src) {

        case NN_SOFI_SRC_OUT_FSM:
            switch (type) {
            case NN_SOFI_OUT_EVENT_SENT:

                /* We must be on the receiving direction */
                nn_assert( self->ng_direction == NN_SOFI_NG_RECV );
                _ofi_debug("OFI[S]: Handshake is completed, stopping "
                           "timeout timer\n");

                /* We completed handshake, stop timer */
                self->state = NN_SOFI_STATE_HANDSHAKE_COMPLETE;
                nn_timer_stop( &self->handshake_timer );

                return;

            case NN_SOFI_IN_EVENT_ERROR:

                /* Unable to complete handshake, shutdown */
                _ofi_debug("OFI[S]: Send error while completing handshake\n");
                nn_sofi_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        case NN_SOFI_SRC_IN_FSM:
            switch (type) {
            case NN_SOFI_IN_EVENT_RECEIVED:

                /* We must be on the sending direction */
                nn_assert( self->ng_direction == NN_SOFI_NG_SEND );
                _ofi_debug("OFI[S]: Handshake is completed, stopping "
                           "timeout timer\n");

                /* Handle reception of data */
                nn_sofi_handle_handshake( self );

                /* We completed handshake, stop timer */
                self->state = NN_SOFI_STATE_HANDSHAKE_COMPLETE;
                nn_timer_stop( &self->handshake_timer );

                return;

            case NN_SOFI_IN_EVENT_ERROR:

                /* Unable to complete handshake, shutdown */
                _ofi_debug("OFI[S]: Receive error while completing handshake\n");
                nn_sofi_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

        case NN_SOFI_SRC_HANDSHAKE_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* Unable to complete handshake, shutdown */
                _ofi_debug("OFI[S]: Handshake timed out!\n");
                nn_sofi_stop( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  HANDSHAKE_COMPLETE state.                                                 */
/*  We are only waiting for the handshake timer to be closed.                 */
/******************************************************************************/
    case NN_SOFI_STATE_HANDSHAKE_COMPLETE:
        switch (src) {

        case NN_SOFI_SRC_HANDSHAKE_TIMER:
            switch (type) {
            case NN_TIMER_STOPPED:

                /* Handhsake phase is completely terminated */
                _ofi_debug("OFI[S]: Handshake sequence completed\n");

                /* Start pipebase */
                self->state = NN_SOFI_STATE_RUNNING;
                nn_pipebase_start( &sofi->pipebase );

                /* TODO: Start Keepalive */

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
