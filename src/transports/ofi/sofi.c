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

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../core/ep.h"
#include "../../core/sock.h"

/* FSM States */
#define NN_SOFI_STATE_IDLE               1001
#define NN_SOFI_STATE_INIT_IN            1002
#define NN_SOFI_STATE_INIT_OUT           1003
#define NN_SOFI_STATE_HANDSHAKE          1004
#define NN_SOFI_STATE_RUNNING            1005
#define NN_SOFI_STATE_ERROR              1007
#define NN_SOFI_STATE_CLOSING            1008
#define NN_SOFI_STATE_CLOSED             1009

/* FSM Sources */
#define NN_SOFI_SRC_IN_FSM               1101
#define NN_SOFI_SRC_OUT_FSM              1102
#define NN_SOFI_SRC_HANDSHAKE_TIMER      1103
#define NN_SOFI_SRC_KEEPALIVE_TIMER      1104
#define NN_SOFI_SRC_TASK_DISCONNECT      1105

/* Timeout values */
#define NN_SOFI_TIMEOUT_HANDSHAKE        1000
#define NN_SOFI_TIMEOUT_KEEPALIVE_TICK   1000

/* Local flags */
#define NN_SOFI_FLAGS_THREAD_ACTIVE      0x01

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

    /* Don't do anything on an invalid state */
    if (self->state != NN_SOFI_STATE_HANDSHAKE) return;

    /* Synchronous send */
    self->error = nn_sofi_out_tx( &self->sofi_out, &self->hs_local, 
        sizeof(self->hs_local), NN_SOFI_TIMEOUT_HANDSHAKE );

    /* Switch state if an error occured */
    if (self->error != 0) {
        self->state = NN_SOFI_STATE_ERROR;
    }

}

/**
 * Complete handshake
 */
static void nn_sofi_recv_handshake( struct nn_sofi *self )
{
    _ofi_debug("OFI[S]: Receiving handshake\n");

    /* Don't do anything on an invalid state */
    if (self->state != NN_SOFI_STATE_HANDSHAKE) return;

    /* Synchronous receive */
    self->error = nn_sofi_out_tx( &self->sofi_out, &self->hs_remote, 
        sizeof(self->hs_remote), NN_SOFI_TIMEOUT_HANDSHAKE );

    /* Switch state if an error occured */
    if (self->error != 0) {
        self->state = NN_SOFI_STATE_ERROR;
    }

}

/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/*  Initialize the state machine */
void nn_sofi_init ( struct nn_sofi *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep, 
    const uint8_t ng_direction, struct nn_epbase *epbase, int src, 
    struct nn_fsm *owner )
{
    /* Keep references */
    self->ofi = ofi;
    self->ep = ep;
    self->epbase = epbase;
    self->ng_direction = ng_direction;

    /* Initialize properties */
    self->flags = 0;

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

    /* ----------------------------------- */
    /*  NanoMsg Component Initialization   */
    /* ----------------------------------- */

    /* Initialize timers */
    nn_timer_init(&self->timer_keepalive, NN_SOFI_SRC_KEEPALIVE_TIMER,
        &self->fsm);

    /*  Choose a worker thread to handle this socket. */
    self->worker = nn_fsm_choose_worker (&self->fsm);

    /* Initialize worker tasks */
    nn_worker_task_init (&self->task_disconnect, NN_SOFI_SRC_TASK_DISCONNECT,
        &self->fsm);

    /* ----------------------------------- */
    /*  OFI Sub-Component Initialization   */
    /* ----------------------------------- */

    /* Get options */
    int rx_queue, tx_queue, rx_msg_size;
    size_t opt_sz = sizeof(int);
    nn_epbase_getopt (epbase, NN_OFI, NN_OFI_TX_QUEUE_SIZE, &tx_queue, &opt_sz);
    nn_epbase_getopt (epbase, NN_OFI, NN_OFI_RX_QUEUE_SIZE, &rx_queue, &opt_sz);
    nn_epbase_getopt (epbase, NN_SOL_SOCKET, NN_RCVBUF, &rx_msg_size, &opt_sz);

    /* Initialize INPUT Sofi */
    nn_sofi_in_init( &self->sofi_in, ofi, ep, ng_direction, rx_queue, rx_msg_size,
        &self->pipebase, NN_SOFI_SRC_IN_FSM, &self->fsm );

    /* Initialize OUTPUT Sofi */
    nn_sofi_out_init( &self->sofi_out, ofi, ep, ng_direction, tx_queue,
        &self->pipebase, NN_SOFI_SRC_OUT_FSM, &self->fsm );

    /* ----------------------------------- */
    /*  Bootstrap FSM                      */
    /* ----------------------------------- */

    /* Start FSM */
    _ofi_debug("OFI[S]: Starting FSM \n");
    nn_fsm_start (&self->fsm);

}

/**
 * Cleanup the state machine
 */
void nn_sofi_term (struct nn_sofi *self)
{
    _ofi_debug("OFI[S]: Cleaning-up SOFI\n");

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
    nn_timer_term (&self->timer_keepalive);

    /* Cleanup worker tasks */
    nn_worker_cancel (self->worker, &self->task_disconnect);
    nn_worker_task_term (&self->task_disconnect);

    /* ----------------------------------- */
    /*  NanoMSG Core Termination           */
    /* ----------------------------------- */

    /* Terminate list item */
    nn_list_item_term (&self->item);

    /* Cleanup components */
    nn_pipebase_term (&self->pipebase);
    nn_fsm_term (&self->fsm);

    /* ----------------------------------- */
    /*  libFabric/HLAPI Termination        */
    /* ----------------------------------- */

    /* Free endpoint resources */
    _ofi_debug("OFI[S]: Freeing OFI resources\n");
    ofi_free_ep( self->ep );

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

    /* This should NEVER be reached on intermediate states */
    nn_assert( self->state != NN_SOFI_STATE_HANDSHAKE );

    /* Switch to closing */
    int fsm_state = self->state;
    self->state = NN_SOFI_STATE_CLOSING;

    /* Stop components according to state */
    switch (fsm_state) {
    case NN_SOFI_STATE_IDLE:
        _ofi_debug("OFI[S]: Stopping []\n");
        break;

    case NN_SOFI_STATE_INIT_IN:
        _ofi_debug("OFI[S]: Stopping [fsm_in]\n");
        nn_sofi_in_stop( &self->sofi_in );
        break;

    case NN_SOFI_STATE_INIT_OUT:
        _ofi_debug("OFI[S]: Stopping [fsm_in, fsm_out]\n");
        nn_sofi_in_stop( &self->sofi_in );
        nn_sofi_out_stop( &self->sofi_out );
        break;

    case NN_SOFI_STATE_RUNNING:
    case NN_SOFI_STATE_ERROR:
        _ofi_debug("OFI[S]: Stopping [fsm_in, fsm_out, timer_keepalive, "
            "worker]\n");
        nn_timer_stop( &self->timer_keepalive );
        nn_sofi_in_stop( &self->sofi_in );
        nn_sofi_out_stop( &self->sofi_out );
        break;

    case NN_SOFI_STATE_CLOSING:
    case NN_SOFI_STATE_CLOSED:
        _ofi_debug("OFI[S]: Stopping []\n");
        break;

    }

    /* Stop FSM & Switch to shutdown handler */
    _ofi_debug("OFI[S]: Stopping core\n");
    nn_fsm_stop (&self->fsm);

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

    _ofi_debug("OFI[p]: Starting poller thread\n");

    /* Keep thread alive while the THREAD_ACTIVE flag is set  */
    while ( self->flags & NN_SOFI_FLAGS_THREAD_ACTIVE ) {

        /* ========================================= */
        /* Wait for Rx CQ event */
        /* ========================================= */
        ret = fi_cq_read( self->ep->rx_cq, &cq_entry, 1 );
        if (nn_slow(ret > 0)) {

            /* Trigger rx worker task */
            _ofi_debug("OFI[p]: Rx CQ Event (ret=%i)\n", ret);
            nn_sofi_in_rx_event( &self->sofi_in, &cq_entry );

        } else if (nn_slow(ret != -FI_EAGAIN)) {

            /* Get error details */
            ret = fi_cq_readerr( self->ep->rx_cq, &err_entry, 0 );
            _ofi_debug("OFI[p]: Rx CQ Error (ret=%i)\n", err_entry.err );

            /* Trigger rx error worker task */
            nn_sofi_in_rx_error_event( &self->sofi_in, &err_entry );

        }

        /* ========================================= */
        /* Wait for Tx CQ event */
        /* ========================================= */
        ret = fi_cq_read( self->ep->tx_cq, &cq_entry, 1 );
        if (nn_slow(ret > 0)) {

            /* Trigger tx worker task */
            _ofi_debug("OFI[p]: Tx CQ Event (ret=%i)\n", ret);
            nn_sofi_out_tx_event( &self->sofi_out, &cq_entry );

        } else if (nn_slow(ret != -FI_EAGAIN)) {

            /* Get error details */
            ret = fi_cq_readerr( self->ep->tx_cq, &err_entry, 0 );
            _ofi_debug("OFI[p]: Tx CQ Error (ret=%i)\n", err_entry.err );

            /* Trigger tx error worker task */
            nn_sofi_out_tx_error_event( &self->sofi_out, &err_entry );

        }

        /* ========================================= */
        /* Wait for EQ events */
        /* ========================================= */
        ret = fi_eq_read( self->ep->eq, &event, &eq_entry, sizeof eq_entry, 0);
        if (nn_fast(ret != -FI_EAGAIN)) {
            _ofi_debug("OFI[p]: Endpoint EQ Event (event=%i)\n", event);

            /* Check for socket disconnection */
            if (event == FI_SHUTDOWN) {
                _ofi_debug("OFI[p]: Endpoint Disconnected\n");
                if (self->state == NN_SOFI_STATE_RUNNING)
                    nn_worker_execute (self->worker, &self->task_disconnect);
                break;
            }

        }

        /* Microsleep for lessen the CPU load */
        if (!--fastpoller) {
            usleep(10);
            fastpoller = 200;
        }

    }
    _ofi_debug("OFI[p]: Exited poller thread\n");

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
    _ofi_debug("OFI[S]: NanoMsg SEND event\n");

    /* Return error codes on erroreus states */
    if (nn_slow(self->state == NN_SOFI_STATE_ERROR)) {
        return self->error;
    } else if (nn_slow(self->state != NN_SOFI_STATE_RUNNING)) {
        return -EFSM;
    }

    /* Forward event to Output FSM */
    return nn_sofi_out_tx_event_send( &self->sofi_out, msg );

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
    _ofi_debug("SOFI[S]: NanoMsg RECV event\n");

    /* Acknowledge event and pull msg from in FSM */
    return nn_sofi_in_rx_event_ack( &self->sofi_in, msg );

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
        _ofi_debug("OFI[S]: We are now closing\n");
        self->state = NN_SOFI_STATE_CLOSING;
    }

    /* Before closing, ensure everything is idle */
    if (self->state == NN_SOFI_STATE_CLOSING) {

        /* Wait for all components to be idle (each one of this
           component will trigger an event when it becomes idle) */
        _ofi_debug("OFI[S]: Waiting for all components to be idle\n");
        if (!nn_timer_isidle(&self->timer_keepalive)) return;
        if (!nn_sofi_in_isidle(&self->sofi_in)) return;
        if (!nn_sofi_out_isidle(&self->sofi_out)) return;

        /* Switch to closed */
        self->state = NN_SOFI_STATE_CLOSED;

    }

    /* If closed, clean-up */
    if (self->state == NN_SOFI_STATE_CLOSED) {

        /* Wait for thread to stop */
        if (self->flags & NN_SOFI_FLAGS_THREAD_ACTIVE) {
            _ofi_debug("OFI[S]: Stopping worker thread\n");
            self->flags &= ~NN_SOFI_FLAGS_THREAD_ACTIVE;
            nn_thread_term (&self->thread_worker);
        }

        /*  Stop endpoint and wait for worker. */
        _ofi_debug("OFI[S]: Stopping OFI endpoint\n");
        ofi_shutdown_ep( self->ep );

        /* Stop nanomsg components */
        _ofi_debug("OFI[S]: Stopping pipebase\n");
        nn_pipebase_stop (&self->pipebase);
        nn_fsm_stopped(&self->fsm, NN_SOFI_STOPPED);
        return;

    }

    /* Invalid state */
    nn_fsm_bad_state (self->state, src, type);
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

        /* ========================= */
        /*  FSM Action               */
        /* ========================= */
        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:

                /* Initialize input FSM */
                _ofi_debug("OFI[S]: Initializing OFI-Input\n");

                /* Start IN SOFI */
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

        /* ========================= */
        /*  INPUT FSM Events         */
        /* ========================= */
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
                self->error = EINTR;
                nn_sofi_stop(self);
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

        /* ========================= */
        /*  OUTPUT FSM Events        */
        /* ========================= */
        case NN_SOFI_SRC_OUT_FSM:
            switch (type) {
            case NN_SOFI_OUT_EVENT_STARTED:

                /* Rx/Tx Initialized, start handshaking */
                _ofi_debug("OFI[S]: Performing handshake\n");
                self->state = NN_SOFI_STATE_HANDSHAKE;

                /* Half part of handshake */
                if (self->ng_direction == NN_SOFI_NG_SEND) {
                    nn_sofi_send_handshake( self );
                    nn_sofi_recv_handshake( self );
                } else if (self->ng_direction == NN_SOFI_NG_RECV) {
                    nn_sofi_recv_handshake( self );
                    nn_sofi_send_handshake( self );
                }

                /* No matter what's the outocome, start the pipebase */
                nn_pipebase_start( &self->pipebase );

                /* Switch to running only if handshake did not cause error */
                if (self->state == NN_SOFI_STATE_HANDSHAKE) {

                    /* Switch to running state */
                    self->state = NN_SOFI_STATE_RUNNING;

                    /* Start poller thread */
                    self->flags |= NN_SOFI_FLAGS_THREAD_ACTIVE;
                    nn_thread_init (&self->thread_worker, nn_sofi_poller_thread, 
                        self);

                    /* Start keepalive timer */
                    nn_timer_start( &self->timer_keepalive, 
                        NN_SOFI_TIMEOUT_KEEPALIVE_TICK );

                } else {

                    /* Stop FSM through worker in order to allow other tasks,
                       such as pending nn_send events, to complete */
                    nn_worker_execute (self->worker, &self->task_disconnect);

                }

                return;

            case NN_SOFI_OUT_EVENT_ERROR:

                /* Unable to initialize output SOFI, shutdown */
                _ofi_debug("OFI[S]: Error while initializing OFI-Output\n");
                self->error = EINTR;
                nn_sofi_stop(self);
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  RUNNING state.                                                            */
/*  Negotiation is finished, components are running. We are now just          */
/*  delegating events.                                                        */
/******************************************************************************/
    case NN_SOFI_STATE_RUNNING:
        switch (src) {

        /* ========================= */
        /*  OUTPUT FSM Events        */
        /* ========================= */
        case NN_SOFI_SRC_OUT_FSM:
            switch (type) {
            case NN_SOFI_OUT_EVENT_SENT:

                /* Notify socket base that data are sent */
                _ofi_debug("OFI[S]: Data are sent\n");
                nn_pipebase_sent(&self->pipebase);

                return;

            case NN_SOFI_OUT_EVENT_ERROR:

                /* Unable to send data, shutdown */
                _ofi_debug("OFI[S]: Error sending data\n");
                self->error = EIO;
                nn_sofi_stop(self);
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  INPUT FSM Events         */
        /* ========================= */
        case NN_SOFI_SRC_IN_FSM:
            switch (type) {
            case NN_SOFI_IN_EVENT_RECEIVED:

                /* Notify socket base that data are available */
                _ofi_debug("OFI[S]: Data are available\n");
                nn_pipebase_received (&self->pipebase);

                return;

            case NN_SOFI_IN_EVENT_ERROR:

                /* Unable to receive data, shutdown */
                _ofi_debug("OFI[S]: Error receiving data\n");
                self->error = EIO;
                nn_sofi_stop(self);
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }


        /* ========================= */
        /*  Keepalive Ticks Timer    */
        /* ========================= */
        case NN_SOFI_SRC_KEEPALIVE_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* Handhsake phase is completely terminated */
                _ofi_debug("OFI[S]: Keepalive tick\n");

                /* Stop Keepalive timer only to be started later */
                nn_timer_stop( &self->timer_keepalive );
                return;

            case NN_TIMER_STOPPED:

                /* Restart Keepalive timer */
                _ofi_debug("OFI[S]: Keepalive stopped, restarting\n");
                nn_timer_start( &self->timer_keepalive, 
                    NN_SOFI_TIMEOUT_KEEPALIVE_TICK );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }


        /* ========================= */
        /*  Worker : Disconnected    */
        /* ========================= */
        case NN_SOFI_SRC_TASK_DISCONNECT:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Socket was disconnected, close */
                _ofi_debug("OFI[S]: Disconnectingn from socket event\n");
                self->error = 0;
                nn_sofi_stop(self);

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  ERROR state.                                                              */
/*  An error occured and all the events are just drained in the sink.         */
/******************************************************************************/
    case NN_SOFI_STATE_ERROR:
        switch (src) {

        /* ========================= */
        /*  IN/OUT FSM Events       */
        /* ========================= */
        case NN_SOFI_SRC_OUT_FSM:
        case NN_SOFI_SRC_IN_FSM:
            return;

        /* ========================= */
        /*  Keepalive Ticks Timer    */
        /* ========================= */
        case NN_SOFI_SRC_KEEPALIVE_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* Stop Keepalive timer */
                _ofi_debug("OFI[S]: Stopping keepalive timer\n");
                nn_timer_stop( &self->timer_keepalive );
                return;

            case NN_TIMER_STOPPED:
                /* Restart Keepalive timer */
                _ofi_debug("OFI[S]: Keepalive stopped, restarting\n");
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  Worker : Disconnected    */
        /* ========================= */
        case NN_SOFI_SRC_TASK_DISCONNECT:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:

                /* Socket was disconnected, close */
                _ofi_debug("OFI[S]: Disconnectingn from socket event\n");
                nn_sofi_stop(self);

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
