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
#define NN_SOFI_STATE_CONNECTING         1002
#define NN_SOFI_STATE_ACTIVE             1003
#define NN_SOFI_STATE_CLOSING            1004

/* FSM OUT State */
#define NN_SOFI_STAGEOUT_STATE_IDLE      0
#define NN_SOFI_STAGEOUT_STATE_STAGED    1

/* FSM OUT State */
#define NN_SOFI_OUT_STATE_IDLE           0
#define NN_SOFI_OUT_STATE_ACTIVE         1

/* FSM Sources */
#define NN_SOFI_SRC_ENDPOINT             1101
#define NN_SOFI_SRC_KEEPALIVE_TIMER      1102

/* Timeout values */
#define NN_SOFI_TIMEOUT_HANDSHAKE        1000
#define NN_SOFI_TIMEOUT_KEEPALIVE_TICK   500

/* How many ticks to wait before sending
   an keepalive packet to remote end. */
#define NN_SOFI_KEEPALIVE_OUT_TICKS      5

/* How many ticks to wait for any incoming
   message (assumed keepalive) from remote end */
#define NN_SOFI_KEEPALIVE_IN_TICKS       10

/* Memory registration keys */
#define NN_SOFI_MRM_SEND_KEY            0xF101
#define NN_SOFI_MRM_RECV_KEY            0xF201

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

/* ########################################################################## */
/*  Utility Functions                                                         */
/* ########################################################################## */

/**
 * A critical SOFI error occured, that must result to the termination
 * of the connection.
 */
static void nn_sofi_critical_error( struct nn_sofi * self, int error )
{
    _ofi_debug("OFI[S]: Unrecoverable error #%i: %s\n", error,
        fi_strerror((int) -error));
    /* Stop the FSM */
    nn_fsm_stop( &self->fsm );
}

/* ########################################################################## */
/*  Egress Functions                                                          */
/* ########################################################################## */

/**
 * Post output buffers (AKA "send data"), and return
 * the number of bytes sent or the error occured.
 */
static int nn_sofi_post_egress_buffers( struct nn_sofi * self, 
    struct nn_msg * outmsg )
{   
    int ret;
    struct iovec iov [2];
    struct fi_msg msg;

    /* Prepare IOVs */
    iov [0].iov_base = nn_chunkref_data (&outmsg->sphdr);
    iov [0].iov_len = nn_chunkref_size (&outmsg->sphdr);
    iov [1].iov_base = nn_chunkref_data (&outmsg->body);
    iov [1].iov_len = nn_chunkref_size (&outmsg->body);

    /* Prepare message */
    memset( &msg, 0, sizeof(msg) );
    msg.msg_iov = iov;
    msg.iov_count = 2;

    /* Populate MR descriptions through MRM */
    ret = ofi_mr_describe( &self->mrm_egress, &msg );
    if (ret) {
        FT_PRINTERR("ofi_mr_describe", ret);
        return ret;
    }

    /* TODO: Libfabric SEND */

    return 0;
}

/**
 * Acknowledge the fact that the ougoing data are sent
 */
static void nn_sofi_egress_handle( struct nn_sofi * self,
    struct fi_cq_data_entry * cq_entry )
{

    /* Release the MR resources associated with this MR */
    ofi_mr_release( &cq_entry->op_context );

}

/**
 * Send staged data
 */
static int nn_sofi_egress_send( struct nn_sofi * self )
{
    int ret;
    nn_assert(self->out_state == NN_SOFI_OUT_STATE_ACTIVE);
    nn_assert(self->stageout_state == NN_SOFI_STAGEOUT_STATE_STAGED);

    /* Post egress buffers */
    ret = nn_sofi_post_egress_buffers( self, &self->outmsg );
    if (ret) {
        FT_PRINTERR("nn_sofi_post_egress_buffers", ret);
        return ret;
    }

    /* Release staged data */
    self->stageout_state = NN_SOFI_STAGEOUT_STATE_IDLE;

    /* TODO: Check for backpressure before calling the next fn */

    /* Data are sent, unlock pipebase for next request */
    nn_pipebase_sent( &self->pipebase );
    return 0;
}

/**
 * Stage data for outgoing transmission
 */
static int nn_sofi_egress_stage( struct nn_sofi * self, 
    struct nn_msg * msg )
{
    int ret;
    nn_assert( self->stageout_state == NN_SOFI_STAGEOUT_STATE_IDLE );

    /* If we are shutting down, don't accept staged messages */
    if (self->state == NN_SOFI_STATE_CLOSING)
        return -EPIPE;

    /* Move the message to the local storage. */
    nn_msg_term (&self->outmsg);
    nn_msg_mv (&self->outmsg, msg);
    self->stageout_state = NN_SOFI_STAGEOUT_STATE_STAGED;

    /* Check if we can send right away */
    if (nn_fast( self->out_state == NN_SOFI_OUT_STATE_ACTIVE )) {
        ret = nn_sofi_egress_send( self );
        if (ret) {
            FT_PRINTERR("nn_sofi_egress_send", ret);
            return ret;
        }
    }

    /* Success */
    return 0;
}

/**
 * Check if there are no outstanding items on the egress queue
 */
static int nn_sofi_egress_empty( struct nn_sofi * self )
{
    return 0;
}

/* ########################################################################## */
/*  Ingress Functions                                                         */
/* ########################################################################## */

/**
 * Post receive buffers
 *
 * This function should pick one of the available pre-allocated message buffers
 * and post them to libfabric. After that, we are expecting a CQ event to 
 * trigger the `sofi_ingress_handle` in order to receive the incoming data.
 */
static void nn_sofi_ingress_post( struct nn_sofi * self )
{

}

/**
 * Process input data
 *
 * Upon completion, this function should return 0 if there are input free
 * buffers available for re-posting or -EAGAIN otherwise. In case an error
 * occurs, this function will return the appropriate error code.
 */
static int nn_sofi_ingress_handle( struct nn_sofi * self, 
    struct fi_cq_data_entry * cq_entry )
{
    return -EAGAIN;
}

/**
 * Pop a message from the ingress queue
 */
static int nn_sofi_ingress_fetch( struct nn_sofi * self,
    struct nn_msg * msg )
{
    return 0;
}

/**
 * Check if there are no outstanding items on the ingress queue
 */
static int nn_sofi_ingress_empty( struct nn_sofi * self )
{
    return 1;
}

/* ########################################################################## */
/*  Implementation  Functions                                                 */
/* ########################################################################## */

/*  Initialize the state machine */
void nn_sofi_init ( struct nn_sofi *self, struct ofi_domain *domain,
    struct nn_epbase *epbase, int src, struct nn_fsm *owner )
{
    int ret;

    /* Initialize properties */
    self->domain = domain;
    self->ep = NULL;
    self->epbase = epbase;
    self->stageout_state = NN_SOFI_STAGEOUT_STATE_IDLE;
    self->out_state = NN_SOFI_OUT_STATE_IDLE;

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

    /* Reset properties */
    self->ticks_in = 0;
    self->ticks_out = 0;

    /* Outgoing message */
    nn_msg_init (&self->outmsg, 0);

    /* ----------------------------------- */
    /*  OFI Sub-Component Initialization   */
    /* ----------------------------------- */

    /* Get options */
    int rx_queue, tx_queue, rx_msg_size;
    size_t opt_sz = sizeof(int);
    nn_epbase_getopt (epbase, NN_OFI, NN_OFI_TX_QUEUE_SIZE, &tx_queue, &opt_sz);
    nn_epbase_getopt (epbase, NN_OFI, NN_OFI_RX_QUEUE_SIZE, &rx_queue, &opt_sz);
    nn_epbase_getopt (epbase, NN_SOL_SOCKET, NN_RCVBUF, &rx_msg_size, &opt_sz);

    /* Get an OFI worker */
    self->worker = ofi_fabric_getworker( domain->parent, &self->fsm );

    /* Initialize egress MR Manager with 32 banks */
    ofi_mr_init( &self->mrm_egress, self->domain, 32, OFI_MR_DIR_SEND, 
        NN_SOFI_MRM_SEND_KEY );

}

/**
 * Start SOFI on the accepting side
 */
int nn_sofi_start_accept( struct nn_sofi *self, struct fi_eq_cm_entry * conreq )
{
    int ret;

    /* Open active endpoint */
    ret = ofi_active_endpoint_open( self->domain, self->worker,
        NN_SOFI_SRC_ENDPOINT, NULL, conreq->info, &self->ep );
    if (ret) {
        FT_PRINTERR("ofi_active_endpoint_open", ret);
        return ret;
    }

    /* Accept incoming connection */
    ret = ofi_cm_accept( self->ep, 
        NULL, 0 ); /* TODO: <<< Add handhsake information */
    if (ret) {
        FT_PRINTERR("ofi_cm_accept", ret);
        return ret;
    }

    /* Start FSM */
    _ofi_debug("OFI[S]: Starting Accepted FSM \n");
    nn_fsm_start (&self->fsm);

    /* Success */
    return 0;
}

/**
 * Start SOFI on the connecting side
 */
int nn_sofi_start_connect( struct nn_sofi *self )
{
    int ret;

    /* Open active endpoint */
    ret = ofi_active_endpoint_open( self->domain, self->worker,
        NN_SOFI_SRC_ENDPOINT, NULL, NULL, &self->ep );
    if (ret) {
        FT_PRINTERR("ofi_active_endpoint_open", ret);
        return ret;
    }

    /* Connect to the remote endpoint */
    ret = ofi_cm_connect( self->ep, NULL,
        NULL, 0 ); /* TODO: <<< Add handhsake information */
    if (ret) {
        FT_PRINTERR("ofi_cm_connect", ret);
        return ret;
    }

    /* Start FSM */
    _ofi_debug("OFI[S]: Starting Connected FSM \n");
    nn_fsm_start (&self->fsm);

    /* Success */
    return 0;
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

    /* Terminate worker */
    nn_ofiw_term( self->worker );

    /* Terminate MR manager */
    ofi_mr_term( &self->mrm_egress );

    /* ----------------------------------- */
    /*  NanoMsg Component Termination      */
    /* ----------------------------------- */

    /* Stop timers */
    nn_timer_term (&self->timer_keepalive);

    /* ----------------------------------- */
    /*  NanoMSG Core Termination           */
    /* ----------------------------------- */

    /* Terminate list item */
    nn_list_item_term (&self->item);

    /* Outgoing message */
    nn_msg_term (&self->outmsg);

    /* Cleanup components */
    nn_pipebase_term (&self->pipebase);
    nn_fsm_term (&self->fsm);

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
    /* Stop FSM & Switch to shutdown handler */
    _ofi_debug("OFI[S]: Stopping FSM\n");
    nn_fsm_stop (&self->fsm);
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
    _ofi_debug("OFI[S]: NanoMsg SEND event\n");
    int ret;
    struct nn_sofi *self;
    self = nn_cont (pb, struct nn_sofi, pipebase);

    /* We sent something */
    self->ticks_out = 0;

    /* Push a message to the egress queue */
    return nn_sofi_egress_stage( self, msg );

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

    /* Fetch a message from the ingress queue */
    return nn_sofi_ingress_fetch( self, msg );
}

/**
 * SHUTDOWN State Handler
 */
static void nn_sofi_shutdown (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{
    struct nn_sofi *self;
    struct fi_cq_data_entry * cq_entry;
    struct fi_cq_err_entry * cq_error;

    /* Get pointer to sofi structure */
    self = nn_cont (fsm, struct nn_sofi, fsm);

    /* If this is part of the FSM action, start shutdown */
    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {
        _ofi_debug("OFI[S]: We are now closing\n");

        /* Shutdown the endpoint */
        self->state = NN_SOFI_STATE_CLOSING;

    } else if (nn_slow(src == (NN_SOFI_SRC_ENDPOINT | OFI_SRC_CQ_TX) )) {

        /* Handle Tx Events. Other Tx events won't be accepted because our
           state is now in NN_SOFI_STATE_CLOSING */
        _ofi_debug("OFI[S]: Handling drain egress event\n");
        nn_sofi_egress_handle( self, cq_entry );

    } else if (nn_slow(src == (NN_SOFI_SRC_ENDPOINT | OFI_SRC_CQ_RX) )) {

        /* Handle Rx Events, but don't post new input buffers */
        _ofi_debug("OFI[S]: Handling drain ingress event\n");
        cq_entry = (struct fi_cq_data_entry *) srcptr;
        nn_sofi_ingress_handle( self, cq_entry );

    } else if (nn_slow(src == NN_SOFI_SRC_KEEPALIVE_TIMER)) {

        /* Wait for timer to stop */
        if (nn_slow( type != NN_TIMER_STOPPED ))
            nn_fsm_bad_action (self->state, src, type);

        _ofi_debug("OFI[S]: Keepalive timer stopped\n");

    } else {
        nn_fsm_bad_source (self->state, src, type);
    }

    /* Wait for all outstanding transmissions or receptions to complete
       and for all resources to be stopped */
    if (!nn_sofi_egress_empty( self ) || 
        !nn_sofi_ingress_empty( self ) ||
        !nn_timer_isidle( &self->timer_keepalive )) {
        return;
    }

    /* Shutdown connection & close endpoint */
    _ofi_debug("OFI[S]: Stopping endpoint\n");
    ofi_cm_shutdown( self->ep );
    ofi_active_endpoint_close( self->ep );

    /* Stop nanomsg components */
    _ofi_debug("OFI[S]: Stopping pipebase\n");
    nn_pipebase_stop (&self->pipebase);
    nn_fsm_stopped(&self->fsm, NN_SOFI_STOPPED);

}

/**
 * ACTIVE State Handler
 */
static void nn_sofi_handler (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{
    int ret;
    struct nn_sofi *self;
    struct fi_cq_data_entry * cq_entry;
    struct fi_cq_err_entry * cq_error;

    /* Get pointer to sofi structure */
    self = nn_cont (fsm, struct nn_sofi, fsm);
    _ofi_debug("OFI[S]: nn_sofi_handler state=%i, src=%i, type=%i\n", 
        self->state, src, type);

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

                /* Wait for connection to be established before starting pipe */
                _ofi_debug("OFI[S]: FSM started, waiting for CONNECTED event\n");
                self->state = NN_SOFI_STATE_CONNECTING;
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  NN_SOFI_STATE_CONNECTING state.                                           */
/*  We are waiting a connection event form the endpoint.                      */
/******************************************************************************/
    case NN_SOFI_STATE_CONNECTING:
        switch (src) {

        /* ========================= */
        /*  Endpoint EQ Action       */
        /* ========================= */
        case NN_SOFI_SRC_ENDPOINT | OFI_SRC_EQ:
            switch (type) {
            case FI_CONNECTED:

                /* The connection is established, start pipe */
                _ofi_debug("OFI[S]: Endpoint connected, starting pipebase\n");
                self->state = NN_SOFI_STATE_ACTIVE;
                self->out_state = NN_SOFI_OUT_STATE_ACTIVE;
                nn_pipebase_start( &self->pipebase );

                /* Post input buffers */
                nn_sofi_ingress_post( self );

                /* Now it's time to send staged data */
                if (self->stageout_state == NN_SOFI_STAGEOUT_STATE_STAGED) {
                    ret = nn_sofi_egress_send( self );
                    if (ret) {
                        FT_PRINTERR("nn_sofi_egress_send", ret);
                        nn_sofi_critical_error( self, ret );
                    }
                }
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }


/******************************************************************************/
/*  NN_SOFI_STATE_ACTIVE state.                                               */
/*  We have an established connection, all events here are regarding the data */
/*  I/O and the one shutdown endpoint event.                                  */
/******************************************************************************/
    case NN_SOFI_STATE_ACTIVE:
        switch (src) {

        /* ========================= */
        /*  Endpoint EQ Action       */
        /* ========================= */
        case NN_SOFI_SRC_ENDPOINT | OFI_SRC_EQ:
            switch (type) {
            case FI_SHUTDOWN:

                /* The connection is dropped from the remote end.
                   This is an unrecoverable error and we should terminate */
                nn_sofi_critical_error( self, -EPIPE );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  Endpoint RX CQ Event     */
        /* ========================= */
        case NN_SOFI_SRC_ENDPOINT | OFI_SRC_CQ_RX:
            switch (type) {
            case NN_OFIW_COMPLETED:

                /* Get CQ Event */
                cq_entry = (struct fi_cq_data_entry *) srcptr;

                /* Process incoming data */
                ret = nn_sofi_ingress_handle( self, cq_entry );

                /* If there is a buffer available, post input
                   buffers again, right away. */
                if (ret == 0) {

                    /* Post input buffers */
                    nn_sofi_ingress_post( self );

                } else if (ret == -EAGAIN) {
                    /* No buffers are avaiable, this is no error */
                } else {

                    /* There was an error posting receive buffer, we
                       cannot recover from such error */
                    nn_sofi_critical_error( self, ret );

                }

                return;

            case NN_OFIW_ERROR:

                /* Get CQ Error */
                cq_error = (struct fi_cq_err_entry *) srcptr;

                /* Unrecoverable error while receiving data */
                nn_sofi_critical_error( self, -cq_error->err );

                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  Endpoint TX CQ Event     */
        /* ========================= */
        case NN_SOFI_SRC_ENDPOINT | OFI_SRC_CQ_TX:
            switch (type) {
            case NN_OFIW_COMPLETED:

                /* Get CQ Event */
                cq_entry = (struct fi_cq_data_entry *) srcptr;

                /* Data from the output buffer are sent */
                nn_sofi_egress_handle( self, cq_entry );

                return;

            case NN_OFIW_ERROR:

                /* Get CQ Error */
                cq_error = (struct fi_cq_err_entry *) srcptr;

                /* Unrecoverable error while sending data */
                nn_sofi_critical_error( self, -cq_error->err );
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

                /* TODO: Handle keepalive ticks */

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
