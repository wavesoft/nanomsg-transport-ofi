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
#define NN_SOFI_STATE_IDLE              1001
#define NN_SOFI_STATE_INIT_IN           1002
#define NN_SOFI_STATE_INIT_OUT          1003
#define NN_SOFI_STATE_HANDSHAKE_HALF    1004
#define NN_SOFI_STATE_HANDSHAKE_FULL    1005
#define NN_SOFI_STATE_RUNNING           1006
#define NN_SOFI_STATE_CLOSING           1007
#define NN_SOFI_STATE_CLOSED            1009

/* FSM Sources */
#define NN_SOFI_SRC_IN_FSM              1101
#define NN_SOFI_SRC_OUT_FSM             1102
#define NN_SOFI_SRC_KEEPALIVE_TIMER     1103

/* Forward Declarations */
static void nn_sofi_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_poller_thread (void *arg);

/* ============================== */
/*       HELPER FUNCTIONS         */
/* ============================== */

/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/*  Initialize the state machine */
void nn_sofi_init (struct nn_sofi_in *self, 
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
    /*  NanoMsg Components Initialization  */
    /* ----------------------------------- */

    /* Initialize list item */
    nn_list_item_init (&self->item);

    /* ----------------------------------- */
    /*  NanoMSG Core Initialization        */
    /* ----------------------------------- */

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

    /* Start FSM */
    _ofi_debug("OFI[S]: Starting FSM \n");
    nn_fsm_start (&self->fsm);

}

/* Check if FSM is idle */
int nn_sofi_isidle (struct nn_sofi_in *self)
{

}

/*  Stop the state machine */
void nn_sofi_stop (struct nn_sofi_in *self)
{

}

/*  Cleanup the state machine */
void nn_sofi_term (struct nn_sofi_in *self)
{

}

/* ============================== */
/*         INPUT EVENTS           */
/* ============================== */

/* Data needs to be sent */
void nn_sofi_event__tx_data (struct nn_sofi_in *self, /* Data */)
{

}

/* Acknowledge transmission of data */
void nn_sofi_event__tx_ack (struct nn_sofi_in *self)
{

}

/* ============================== */
/*          WORKER THREAD         */
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

            /* Make sure we have posted a message */
            // nn_mutex_lock (&self->rx_sync);

            /* Initialize a new message on the shared pointer */
            nn_msg_init_chunk (&self->inmsg, self->inmsg_chunk);
            _ofi_debug("OFI[S]: Got incoming message of %li bytes\n", cq_entry.len);

            /* Hack to force new message size on the chunkref */
            if (cq_entry.len <= self->recv_buffer_size) {
                nn_sofi_DANGEROUS_hack_chunk_size( self->inmsg_chunk, cq_entry.len );
            } else {
                printf("WARNING: Silent data truncation from %lu to %d (increase your receive buffer size!)\n", cq_entry.len, self->recv_buffer_size );
                nn_sofi_DANGEROUS_hack_chunk_size( self->inmsg_chunk, self->recv_buffer_size );
            }

            /* Handle the fact that the data are received  */
            _ofi_debug("OFI[S]: Rx CQ Event\n");
            nn_worker_execute (self->worker, &self->task_rx);
            // nn_sofi_input_action( self, NN_SOFI_INACTION_RX_DATA );

            // nn_mutex_unlock (&self->rx_sync);

            // nn_fsm_feed ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_RX, NULL );

            /* Wait for Rx Ack */
            // nn_efd_wait( &self->rx_sync, -1 );
            // nn_efd_unsignal( &self->rx_sync );

        } else if (nn_slow(ret != -FI_EAGAIN)) {
            if (ret == -FI_EAVAIL) {

                /* Get error details */
                ret = fi_cq_readerr( self->ep->rx_cq, &err_entry, 0 );
                if (err_entry.err == FI_ECANCELED) {

                    /* The socket operation was cancelled, we were disconnected */
                    _ofi_debug("OFI[S]: Rx CQ Error (-FI_ECANCELED) caused disconnection\n");
                    nn_worker_execute (self->worker, &self->task_disconnected);
                    // nn_sofi_input_action( self, NN_SOFI_INACTION_CLOSE );

                    // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_DISCONNECT, NULL );
                    break;

                }

                /* Handle error */
                _ofi_debug("OFI[S]: Rx CQ Error (%i)\n", -err_entry.err);
                self->error = -err_entry.err;
                // nn_sofi_input_action( self, NN_SOFI_INACTION_ERROR );
                nn_worker_execute (self->worker, &self->task_error);
                // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_ERROR, NULL );
                break;

            } else {

                /* Unexpected CQ Read error */
                FT_PRINTERR("fi_cq_read<rx_cq>", ret);
                self->error = ret;
                nn_worker_execute (self->worker, &self->task_error);
                // nn_sofi_input_action( self, NN_SOFI_INACTION_ERROR );
                // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_ERROR, NULL );
                break;

            }
        }

        /* ========================================= */
        /* Wait for Tx CQ event */
        /* ========================================= */
        ret = fi_cq_read( self->ep->tx_cq, &cq_entry, 1 );
        if (nn_slow(ret > 0)) {

            /* Make sure we have posted a message */
            // nn_assert( self->outstate == NN_SOFI_OUTSTATE_POSTED );
            // self->outstate = NN_SOFI_OUTSTATE_PENDING;

            /* Handle the fact that the data are sent */
            _ofi_debug("OFI[S]: Tx CQ Event\n");
            nn_worker_execute (self->worker, &self->task_tx);
            // nn_sofi_output_action( self, NN_SOFI_OUTACTION_TX_ACK );
            // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_TX, NULL );

        } else if (nn_slow(ret != -FI_EAGAIN)) {

            if (ret == -FI_EAVAIL) {

                /* Get error details */
                ret = fi_cq_readerr( self->ep->tx_cq, &err_entry, 0 );
                if (err_entry.err == FI_ECANCELED) {

                    /* The socket operation was cancelled, we were disconnected */
                    _ofi_debug("OFI[S]: Tx CQ Error (-FI_ECANCELED) caused disconnection\n");
                    nn_worker_execute (self->worker, &self->task_disconnected);
                    // nn_sofi_output_action( self, NN_SOFI_OUTACTION_CLOSE );
                    // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_DISCONNECT, NULL );
                    break;

                }

                /* Handle error */
                _ofi_debug("OFI[S]: Tx CQ Error (%i)\n", -err_entry.err);
                self->error = -err_entry.err;
                nn_worker_execute (self->worker, &self->task_error);
                // nn_sofi_output_action( self, NN_SOFI_OUTACTION_ERROR );
                // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_ERROR, NULL );
                break;

            } else {

                /* Unexpected CQ Read error */
                FT_PRINTERR("fi_cq_read<tx_cq>", ret);
                self->error = ret;
                nn_worker_execute (self->worker, &self->task_error);
                // nn_sofi_output_action( self, NN_SOFI_OUTACTION_ERROR );
                // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_ERROR, NULL );
                break;

            }

        }

        /* ========================================= */
        /* Wait for EQ events */
        /* ========================================= */
        ret = fi_eq_read( self->ep->eq, &event, &eq_entry, sizeof eq_entry, 0);
        if (nn_fast(ret != -FI_EAGAIN)) {

            /* Check for socket disconnection */
            if (event == FI_SHUTDOWN) {
                _ofi_debug("OFI[S]: Got shutdown EQ event\n");
                nn_worker_execute (self->worker, &self->task_disconnected);
                // nn_sofi_disconnect( self );
                // nn_sofi_output_action( self, NN_SOFI_OUTACTION_CLOSE );
                // nn_sofi_input_action( self, NN_SOFI_INACTION_CLOSE );
                // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_DISCONNECT, NULL );
                break;
            }

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
/*          FSM HANDLERS          */
/* ============================== */

/**
 * SHUTDOWN State Handler
 */
static void nn_sofi_shutdown (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{

}

/**
 * ACTIVE State Handler
 */
static void nn_sofi_handler (struct nn_fsm *fsm, int src, int type, 
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
    case NN_SOFI_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:


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
