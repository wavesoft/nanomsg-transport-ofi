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
#include "sofi.h"

#include "../../aio/ctx.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/wire.h"


/* Helper macro to enable or disable verbose logs on console */
#ifdef OFI_DEBUG_LOG
    /* Enable debug */
    #define _ofi_debug(...)   printf(__VA_ARGS__)
#else
    /* Disable debug */
    #define _ofi_debug(...)
#endif

/* State machine states */
#define NN_SOFI_STATE_IDLE              1
#define NN_SOFI_STATE_CONNECTED         2
#define NN_SOFI_STATE_STOPPING          3
#define NN_SOFI_STATE_DISCONNECTED      4

/* Private SOFI events */
#define NN_SOFI_ACTION_DATA             2010

/* Private SOFI sources */
#define NN_SOFI_SRC_DISCONNECT_TIMER    1100

/*  State machine functions. */
static void nn_sofi_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);

/* Pipe definition */
static int nn_sofi_send (struct nn_pipebase *self, struct nn_msg *msg);
static int nn_sofi_recv (struct nn_pipebase *self, struct nn_msg *msg);
const struct nn_pipebase_vfptr nn_sofi_pipebase_vfptr = {
    nn_sofi_send,
    nn_sofi_recv
};

static void nn_sofi_poller_thread (void *arg);

/**
 * Create a streaming (connected) OFI Socket
 */
void nn_sofi_init (struct nn_sofi *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep, 
    struct nn_epbase *epbase, int src, struct nn_fsm *owner)
{    
    int ret;

    /* Keep OFI resources */
    self->ofi = ofi;
    self->ep = ep;

    /* ==================== */

    /* Initialize list item */
    nn_list_item_init (&self->item);

    /* Initialize fsm events */ 
    nn_fsm_event_init (&self->disconnected);

    /* Initialize buffes */
    nn_msg_init (&self->inmsg, 0);

    /* ==================== */

    /* Initialize pipe base */
    _ofi_debug("OFI: SOFI: Replacing pipebase\n");
    nn_pipebase_init (&self->pipebase, &nn_sofi_pipebase_vfptr, epbase);

    /* ==================== */

    /* Get maximum size of receive buffer */
    int recv_size;
    int send_size;
    size_t opt_sz = sizeof (recv_size);

    /* Get buffer sizes */
    nn_epbase_getopt (epbase, NN_SOL_SOCKET, NN_SNDBUF,
        &send_size, &opt_sz);
    nn_epbase_getopt (epbase, NN_SOL_SOCKET, NN_RCVBUF,
        &recv_size, &opt_sz);

    /* Initialize OFI memory region */
    _ofi_debug("OFI: SOFI: Initializing MR with tx_size=%i, rx_size=%i\n",
        send_size, recv_size);
    ret = ofi_active_ep_init_mr( self->ofi, self->ep, (unsigned)recv_size, 
        (unsigned)send_size );
    if (ret) {
        /* TODO: Handle error */
        printf("OFI: SOFI: ERROR: Unable to allocate memory region for EP!\n");
        return;
    }

    /* ==================== */

    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_handler, nn_sofi_shutdown,
        src, self, owner);
    self->state = NN_SOFI_STATE_IDLE;

    /* Initialize tiemr */
    nn_timer_init(&self->disconnect_timer, NN_SOFI_SRC_DISCONNECT_TIMER, &self->fsm);

    /* Start FSM */
    _ofi_debug("OFI: SOFI: Start \n");
    nn_fsm_start (&self->fsm);

}

/**
 * Cleanup all the SOFI resources
 */
void nn_sofi_term (struct nn_sofi *self)
{

    /* Free OFI Endpoint */
    ofi_free_ep( self->ep );

    /* Cleanup instantiated resources */
    nn_list_item_term (&self->item);
    nn_timer_term (&self->disconnect_timer);
    nn_fsm_event_term (&self->disconnected);
    nn_msg_term (&self->inmsg);
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
    _ofi_debug("OFI: Stopping SOFI\n");

    /* Stop FSM */
    nn_fsm_stop (&self->fsm);
}

/**
 * Shutdown OFI FSM Handler
 *
 * Depending on the state the FSM is currently in, this 
 * function should perform the appropriate clean-up operations.
 */
static void nn_sofi_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    _ofi_debug("OFI: SOFI: Shutdown\n");

    /* Get pointer to sofi structure */
    struct nn_sofi *sofi;
    sofi = nn_cont (self, struct nn_sofi, fsm);

    /* Switch to shutdown if this was an fsm action */
    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {

        /* Abort OFI Operations */
        sofi->state = NN_SOFI_STATE_STOPPING;
        ofi_shutdown_ep( sofi->ep );

        /* Stop child objects */
        nn_pipebase_stop (&sofi->pipebase);

        /*  Wait till worker thread terminates. */
        nn_thread_term (&sofi->thread);

        /* We are stopped */
        nn_fsm_stopped(&sofi->fsm, NN_SOFI_STOPPED);
        return;
    }

    nn_fsm_bad_state (sofi->state, src, type);
}

static int nn_sofi_send (struct nn_pipebase *self, struct nn_msg *msg)
{
    int ret;
    struct nn_sofi *sofi;
    sofi = nn_cont (self, struct nn_sofi, pipebase);

    /* * * * * * * * * * * * * * * * * * * * * * * * * * * */
    /* WARNING : WE ARE BREAKING THE ZERO-COPY PRINCIPLE!  */
    /* * * * * * * * * * * * * * * * * * * * * * * * * * * */

    /*  Start async sending. */
    size_t sz_outhdr = sizeof(sofi->outhdr);
    size_t sz_sphdr = nn_chunkref_size (&msg->sphdr);
    size_t sz_body = nn_chunkref_size (&msg->body);

    /* Check overflow */
    if (sz_sphdr + sz_body > sofi->ep->tx_size) {
        _ofi_debug("OFI: SOFI: Trying to send len=%lu, when tx_size=%zu\n", sz_sphdr+sz_body, sofi->ep->tx_size);        
        return -EOVERFLOW;
    }

    /*  Serialise the message header. */
    nn_putll (sofi->outhdr, sz_sphdr + sz_body);

    /* Serialize data to the tx buffer */
    memcpy( sofi->ep->tx_buf, &sofi->outhdr, sizeof(sofi->outhdr) );
    memcpy( sofi->ep->tx_buf + sz_outhdr, 
        nn_chunkref_data (&msg->sphdr), sz_sphdr );
    memcpy( sofi->ep->tx_buf + sz_outhdr + sz_sphdr, 
        nn_chunkref_data (&msg->body), sz_body );

    /* Send buffer */
    _ofi_debug("OFI: SOFI: Sending data (size=%lu)\n", sz_outhdr+sz_sphdr+sz_body );
    ret = ofi_tx( sofi->ep, sz_outhdr+sz_sphdr+sz_body );
    if (ret) {
        /* TODO: Handle errors */
        printf("OFI: SOFI: Error sending data!\n");
        return -ECONNRESET;
    }

    /* Success */
    nn_pipebase_sent (&sofi->pipebase);

    /* Success */
    return 0;
}

static int nn_sofi_recv (struct nn_pipebase *self, struct nn_msg *msg)
{
    int rc;
    struct nn_sofi *sofi;
    sofi = nn_cont (self, struct nn_sofi, pipebase);

    /* Move received message to the user. */
    nn_msg_mv (msg, &sofi->inmsg);
    nn_msg_init (&sofi->inmsg, 0);

    /* Success */
    return 0;
}

/**
 * The internal poller thread, since OFI does not 
 * have blocking UNIX file descriptors
 */
static void nn_sofi_poller_thread (void *arg)
{
    ssize_t ret;
    size_t size;
    struct nn_sofi * self = (struct nn_sofi *) arg;

    /* Infinite loop */
    while (1) {

        /* Receive data from OFI */
        ret = ofi_rx( self->ep, MAX_MSG_SIZE );
        if (ret == -FI_REMOTE_DISCONNECT) { /* Remotely disconnected */
            _ofi_debug("OFI: Remotely disconnected!\n");
            break;
        }

        /* Handle errors */
        if (ret) {
            printf("OFI: SOFI: Receive Error!\n");
            /* TODO: Properly handle errors */
            break;
        }

        /* If exited the connected state, stop thread */
        if (self->state != NN_SOFI_STATE_CONNECTED) {
            _ofi_debug("OFI: Exiting poller thread because changed state to %i\n", self->state);
            break;
        }

        /*  Message header was received. Check that message size
            is acceptable by comparing with NN_RCVMAXSIZE;
            if it's too large, drop the connection. */
        size = nn_getll ( self->ep->rx_buf );

        /* Check for invalid sizes */
        if (size > self->ep->rx_size) {
            printf("OFI: SOFI: Discarding incoming packaget due to invalid size"
                    " (len=%lu, max=%lu)\n", size, self->ep->rx_size );
            /* TODO: Properly handle errors */
            continue;
        }

        /* Initialize msg with rx chunk */
        nn_msg_term (&self->inmsg);
        nn_msg_init( &self->inmsg, size );

        /* * * * * * * * * * * * * * * * * * * * * * * * * * * */
        /* WARNING : WE ARE BREAKING THE ZERO-COPY PRINCIPLE!  */
        /* * * * * * * * * * * * * * * * * * * * * * * * * * * */

        /* Copy body */
        _ofi_debug("OFI: SOFI: Received data (len=%lu)\n", size);
        memcpy( nn_chunkref_data (&self->inmsg.body), 
                self->ep->rx_buf + 8, size );

        /* Notify FSM for the fact that we have received data  */
        nn_ctx_enter( self->fsm.ctx );
        nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_DATA );
        nn_ctx_leave( self->fsm.ctx );

    }

    /* Notify FSM for the fact that we are disconnected  */
    if (self->state == NN_SOFI_STATE_CONNECTED) {
        _ofi_debug("OFI: Triggering discconect because poller thread exited\n");

        /* We are using the disconnect timer trick in order to change threads,
           and therfore allow a clean stop() of the fsm. */
        nn_timer_start( &self->disconnect_timer, 1 );
    }

}

/**
 * Streaming OFI FSM Handler
 */
static void nn_sofi_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_sofi *sofi;

    /* Continue with the next OFI Event */
    sofi = nn_cont (self, struct nn_sofi, fsm);
    _ofi_debug("OFI: nn_sofi_handler state=%i, src=%i, type=%i\n", sofi->state, src, 
        type);

    /* Handle new state */
    switch (sofi->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_SOFI_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:

                /* Start pipe */
                _ofi_debug("OFI: SOFI: Started!\n");
                nn_pipebase_start( &sofi->pipebase );

                /* Start poller thread */
                sofi->state = NN_SOFI_STATE_CONNECTED;
                nn_thread_init (&sofi->thread, nn_sofi_poller_thread, sofi);

                return;
            default:
                nn_fsm_bad_action (sofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (sofi->state, src, type);
        }

/******************************************************************************/
/*  CONNECTED state.                                                          */
/******************************************************************************/

    case NN_SOFI_STATE_CONNECTED:
        switch (src) {

        /* Zombie timer */
        case NN_SOFI_SRC_DISCONNECT_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* We are now disconnected, stop timer */
                sofi->state = NN_SOFI_STATE_DISCONNECTED;
                nn_timer_stop(&sofi->disconnect_timer);
                return;

            default:
                nn_fsm_bad_action (sofi->state, src, type);
            }

        /* Local Actions */
        case NN_FSM_ACTION:
            switch (type) {
            case NN_SOFI_ACTION_DATA:

                /* Notify pipebase that we have some data */
                nn_pipebase_received (&sofi->pipebase);

                return;
            default:
                nn_fsm_bad_action (sofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (sofi->state, src, type);
        }

/******************************************************************************/
/*  DISCONNECTED state.                                                       */
/******************************************************************************/

    case NN_SOFI_STATE_DISCONNECTED:
        switch (src) {

        /* Zombie timer */
        case NN_SOFI_SRC_DISCONNECT_TIMER:
            switch (type) {
            case NN_TIMER_STOPPED:

                /* Notify parent fsm that we are disconnected */
                nn_fsm_raise(&sofi->fsm, &sofi->disconnected, 
                    NN_SOFI_DISCONNECTED);
                return;

            default:
                nn_fsm_bad_action (sofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (sofi->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (sofi->state, src, type);

    }

}
