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
#include "../../ofi.h"

#include "../../aio/ctx.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/wire.h"

/* Payload of the keepalive paket (having an invalid nanomsg protocol header, to distinguish) */
const uint8_t FT_PACKET_KEEPALIVE[8] = {0xFF, 0xFF, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0xFF };


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
#define NN_SOFI_SRC_SHUTDOWN_TIMER      1100
#define NN_SOFI_SRC_KEEPALIVE_TIMER     1101

/* Configurable times for keepalive */
#define NN_SOFI_IO_TIMEOUT_SEC              5
#define NN_SOFI_KEEPALIVE_INTERVAL          1000
#define NN_SOFI_KEEPALIVE_COUNTER           1
#define NN_SOFI_KEEPALIVE_TIMEOUT_COUNTER   5

/* Shutdown reasons */
#define NN_SOFI_SHUTDOWN_DISCONNECT         1
#define NN_SOFI_SHUTDOWN_ERROR              2

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

    /* Prepare thread resources */
    nn_efd_init( &self->sync );

    /* ==================== */

    /* Initialize pipe base */
    _ofi_debug("OFI: SOFI: Replacing pipebase\n");
    nn_pipebase_init (&self->pipebase, &nn_sofi_pipebase_vfptr, epbase);

    /* ==================== */

    /* Get configured slab size */
    size_t opt_sz = sizeof(self->slab_size);
    nn_epbase_getopt (epbase, NN_SOL_SOCKET, NN_OFI_SLABMR_SIZE,
        &self->slab_size, &opt_sz);
    nn_epbase_getopt (epbase, NN_SOL_SOCKET, NN_RCVBUF,
        &self->recv_buffer_size, &opt_sz);

    /* Allocate slab buffer */
    self->mr_slab_ptr = nn_alloc( self->slab_size * 2 + sizeof( struct nn_ofi_sys_ptrs ), "ofi (slab memory)" );
    if (!self->mr_slab_ptr) {
        printf("OFI: SOFI: ERROR: Unable to allocate slab memory region!\n");
        return;
    }

    /* Get pointer to slab user/slab data */
    self->mr_slab_data_in = self->mr_slab_ptr + sizeof( struct nn_ofi_sys_ptrs );
    self->mr_slab_data_out = self->mr_slab_data_in + self->slab_size;
    self->mr_sys_ptr = (struct nn_ofi_sys_ptrs *) self->mr_slab_ptr;

    /* Allocate a new memory region management helper */
    ret = ofi_mr_alloc( self->ep, &self->mr_slab );
    if (ret) {
       /* TODO: Handle error */
       printf("OFI: SOFI: ERROR: Unable to allocate an MR management object for slab objects!\n");
       return;
    }

    /* Mark the memory region */
    ret = ofi_mr_manage( self->ep, self->mr_slab, self->mr_slab_ptr, self->slab_size + NN_OFI_SLABMR_SIZE, MR_SEND | MR_RECV );
    if (ret) {
       /* TODO: Handle error */
       printf("OFI: SOFI: ERROR: Unable to mark the slab memory MR region!\n");
       return;
    }

    /* Allocate a new memory region management helper */
    ret = ofi_mr_alloc( self->ep, &self->mr_user );
    if (ret) {
       /* TODO: Handle error */
       printf("OFI: SOFI: ERROR: Unable to allocate an MR management object for user objects!\n");
       return;
    }

    /* ==================== */

    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_handler, nn_sofi_shutdown,
        src, self, owner);
    self->state = NN_SOFI_STATE_IDLE;

    /* Initialize timer */
    nn_timer_init(&self->shutdown_timer, NN_SOFI_SRC_SHUTDOWN_TIMER, &self->fsm);
    self->shutdown_reason = 0;
    nn_timer_init(&self->keepalive_timer,  NN_SOFI_SRC_KEEPALIVE_TIMER, &self->fsm);
    self->keepalive_tx_ctr = 0;
    self->keepalive_rx_ctr = 0;

    /* Start FSM */
    _ofi_debug("OFI: SOFI: Start \n");
    nn_fsm_start (&self->fsm);

}

/**
 * Cleanup all the SOFI resources
 */
void nn_sofi_term (struct nn_sofi *self)
{

    /* Cleanup instantiated resources */
    nn_list_item_term (&self->item);
    nn_timer_term (&self->shutdown_timer);
    nn_timer_term (&self->keepalive_timer);
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

        /* Stop keepalive timer */
        nn_timer_stop( &sofi->keepalive_timer);
    }
    if (nn_slow (sofi->state == NN_SOFI_STATE_STOPPING )) {

        /* Wait for keepalive timer to stop */
        if (!nn_timer_isidle(&sofi->keepalive_timer))
            return;

        /* Cleanups */
        nn_thread_term (&sofi->thread);

        /*  Unmanage memory regions. */
        _ofi_debug("OFI: Freeing memory resources\n");
        ofi_mr_free( sofi->ep, &sofi->mr_slab );
        ofi_mr_free( sofi->ep, &sofi->mr_user );
        nn_free( sofi->mr_slab_ptr );

        /*  Stop endpoint and wait for worker. */
        _ofi_debug("OFI: Freeing endpoint resources\n");
        ofi_shutdown_ep( sofi->ep );
        ofi_free_ep( sofi->ep );

        /* Stop child objects */
        nn_pipebase_stop (&sofi->pipebase);

        /* We are stopped */
        nn_fsm_stopped(&sofi->fsm, NN_SOFI_STOPPED);
        return;
    }

    nn_fsm_bad_state (sofi->state, src, type);
}

/**
 * Helper function to either memcpy data to mark the region as shared
 */
void nn_sofi_mr_outgoing ( struct nn_sofi *self, void * ptr, size_t len, void ** sendptr, void ** descptr )
{
    if (len < self->slab_size)
    {
        /* Copy to already allocated user-region of shared MR */
        memcpy( self->mr_slab_data_out, ptr, len );
        /* Update pointer */
        *sendptr = self->mr_slab_data_out;
        *descptr = fi_mr_desc( self->mr_slab->mr );
    }
    else
    {
        /* Manage this memory region */
        ofi_mr_manage( self->ep, self->mr_user, ptr, len, MR_SEND );
        /* Update pointer */
        *sendptr = ptr;
        *descptr = fi_mr_desc( self->mr_user->mr );
    }
}

/**
 * This function is called by the nanomsg core when some data needs to be sent.
 * It's important to call the 'nn_pipebase_sent' function when ready!
 */
static int nn_sofi_send (struct nn_pipebase *self, struct nn_msg *msg)
{
    int ret;
    struct ofi_mr * mr;
    struct nn_sofi *sofi;
    struct iovec iov [3];
    void * iov_desc  [3];
    sofi = nn_cont (self, struct nn_sofi, pipebase);

    /*  Start async sending. */
    size_t sz_outhdr = sizeof(sofi->mr_sys_ptr->outhdr);
    size_t sz_sphdr = nn_chunkref_size (&msg->sphdr);
    size_t sz_body = nn_chunkref_size (&msg->body);

    /*  Serialise the message header. */
    nn_putll (sofi->mr_sys_ptr->outhdr, sz_sphdr + sz_body);

    /*  Move the message to the local storage. */
    nn_msg_term (&sofi->outmsg);
    nn_msg_mv (&sofi->outmsg, msg);

    /*  IOV[0] : Output Header. */
    iov [0].iov_base = sofi->mr_sys_ptr->outhdr;
    iov [0].iov_len = sz_outhdr;
    iov_desc[0] = fi_mr_desc( sofi->mr_slab->mr );

    /* Send header */
    _ofi_debug("OFI: SOFI: Sending header [type=data,size=%lu] (len=%lu)\n", sz_sphdr+sz_body, sz_outhdr );
    ret = ofi_tx_msg( sofi->ep, iov, iov_desc, 1, 0, NN_SOFI_IO_TIMEOUT_SEC );
    if (ret) {
        printf("OFI: Error sending data!\n");

        /* Shutdown because of error */        
        sofi->shutdown_reason = NN_SOFI_SHUTDOWN_DISCONNECT;
        nn_timer_start( &sofi->shutdown_timer, 1 );

        /* This did not work out, but don't let nanomsg know */
        return 0;
    }

    /* IOV[1] : SPHDR to shared MR */
    memcpy( sofi->mr_sys_ptr->sphdr, nn_chunkref_data (&sofi->outmsg.sphdr), sz_sphdr );
    iov [0].iov_base = sofi->mr_sys_ptr->sphdr;
    iov [0].iov_len = sz_sphdr;
    iov_desc[0] = fi_mr_desc( sofi->mr_slab->mr );

    /* IOV[2] : Smart management (copy or tag) of the body pointer */
    iov [1].iov_len = sz_body;
    nn_sofi_mr_outgoing( sofi, nn_chunkref_data (&sofi->outmsg.body), sz_body,
                     &iov[1].iov_base, &iov_desc[2]);

    /* Send payload */
    _ofi_debug("OFI: SOFI: Sending payload (len=%lu)\n", sz_sphdr+sz_body );
    ret = ofi_tx_msg( sofi->ep, iov, iov_desc, 2, 0, NN_SOFI_IO_TIMEOUT_SEC );
    if (ret) {
        printf("OFI: Error sending data!\n");

        /* Shutdown because of error */        
        sofi->shutdown_reason = NN_SOFI_SHUTDOWN_DISCONNECT;
        nn_timer_start( &sofi->shutdown_timer, 1 );

        /* This did not work out, but don't let nanomsg know */
        return 0;
    }


    /* Success */
    nn_pipebase_sent (&sofi->pipebase);

    /* Restart keepalive tx counter */
    sofi->keepalive_tx_ctr = 0;

    /* Success */
    return 0;
}

/**
 * This function is called by the nanomsg core when some data needs to be sent.
 * This is triggered only when 'nn_pipebase_received' is called!
 */
static int nn_sofi_recv (struct nn_pipebase *self, struct nn_msg *msg)
{
    int rc;
    struct nn_sofi *sofi;
    sofi = nn_cont (self, struct nn_sofi, pipebase);

    /* Move received message to the user. */
    nn_msg_mv (msg, &sofi->inmsg);
    nn_msg_init (&sofi->inmsg, 0);

    /* Unblock thread to receive next chunk */
    _ofi_debug("OFI: SOFI: Sending Rx Signal\n");
    nn_efd_signal( &sofi->sync );

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
    struct iovec iov [2];
    void * iov_desc  [2];
    struct nn_sofi * self = (struct nn_sofi *) arg;

    /* Infinite loop */
    while (1) {

        /* --------------------------------------------------- */

        /* Receive data from OFI */
        iov [0].iov_base = self->mr_sys_ptr->inhdr;
        iov [0].iov_len = sizeof(self->mr_sys_ptr->inhdr);
        iov_desc[0] = fi_mr_desc( self->mr_slab->mr );

        _ofi_debug("OFI: SOFI: Waiting for incoming header\n");
        ret = ofi_rx_msg( self->ep, iov, iov_desc, 1, 0, -1 );
        if (ret == -FI_REMOTE_DISCONNECT) { /* Remotely disconnected */
            _ofi_debug("OFI: Remotely disconnected!\n");
            break;
        }

        /* Handle errors */
        if (ret) {
            printf("OFI: Unable to receive header!\n");
            goto error;
        }

        /* If exited the connected state, stop thread */
        if (self->state != NN_SOFI_STATE_CONNECTED) {
            _ofi_debug("OFI: Exiting poller thread because changed state to %i\n", self->state);
            break;
        }

        /* --------------------------------------------------- */

        /* Restart keepalive rx timer */
        self->keepalive_rx_ctr = 0;

        /* Check if this is a polling message */
        if (memcmp(self->mr_sys_ptr->inhdr, FT_PACKET_KEEPALIVE, sizeof(FT_PACKET_KEEPALIVE)) == 0) {
            _ofi_debug("OFI: SOFI: Received keepalive packet\n");
            continue;
        }

        /*  Message header was received. Check that message size
            is acceptable by comparing with NN_RCVMAXSIZE;
            if it's too large, drop the connection. */
        size = nn_getll ( self->mr_sys_ptr->inhdr );
        _ofi_debug("OFI: SOFI: Got incoming message of %li bytes\n", size);

        /* Initialize msg with rx chunk */
        nn_msg_term (&self->inmsg);
        nn_msg_init( &self->inmsg, size );

        /* Decide how to receive the message */
        if (size < self->slab_size) {

            /* Use the memory slab as the receving endpoint */
            iov [0].iov_base = self->mr_slab_data_in;
            iov [0].iov_len = size;
            iov_desc[0] = fi_mr_desc( self->mr_slab->mr );
        
        } else {

            /* Manage this memory region */
            ofi_mr_manage( self->ep, self->mr_user, 
                nn_chunkref_data(&self->inmsg.body), size, MR_RECV );

            /* Use the message buffer as our new shared memory region */
            iov [0].iov_base = nn_chunkref_data(&self->inmsg.body);
            iov [0].iov_len = size;
            iov_desc[0] = fi_mr_desc( self->mr_user->mr );

        }

        /* Receive the actual message */
        _ofi_debug("OFI: SOFI: Receiving data\n");
        ret = ofi_rx_msg( self->ep, iov, iov_desc, 1, 0, -1 );
        if (ret == -FI_REMOTE_DISCONNECT) { /* Remotely disconnected */
            _ofi_debug("OFI: Remotely disconnected!\n");
            break;
        }

        /* Handle errors */
        if (ret) {
            printf("OFI: Unable to receive payload!\n");
            goto error;
        }

        /* Final part of small slab messages */
        if (size < self->slab_size) {
            memcpy( nn_chunkref_data(&self->inmsg.body), self->mr_slab_data_in, size );
        }

        /* Notify FSM for the fact that we have received data  */
        nn_ctx_enter( self->fsm.ctx );
        nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_DATA );
        nn_ctx_leave( self->fsm.ctx );

        /* Wait for sent confirmation */
        _ofi_debug("OFI: SOFI: Waiting for Rx signal\n");
        nn_efd_wait( &self->sync, -1 );
        nn_efd_unsignal( &self->sync );

    }

    /* Skip error routine */
    goto final;

error:

    /* Just a placeholder for error handling */
    /* TODO: Properly handle errors */
    _ofi_debug("OFI: Error handling routine is missing!\n");

final:

    /* Notify FSM for the fact that we are disconnected  */
    if (self->state == NN_SOFI_STATE_CONNECTED) {
        _ofi_debug("OFI: Triggering discconect because poller thread exited\n");

        /* We are using the disconnect timer trick in order to change threads,
           and therfore allow a clean stop() of the fsm. */
        self->shutdown_reason = NN_SOFI_SHUTDOWN_DISCONNECT;
        nn_timer_start( &self->shutdown_timer, 1 );
    }

}

/**
 * Streaming OFI FSM Handler
 */
static void nn_sofi_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct iovec iov [1];
    void * iov_desc  [1];
    struct nn_sofi *sofi;
    int ret;

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

                /* Start keepalive timer */
                nn_timer_start( &sofi->keepalive_timer, NN_SOFI_KEEPALIVE_INTERVAL );

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

        /* Keepalive timer */
        case NN_SOFI_SRC_KEEPALIVE_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* Check if we RECEIVED a keepalive in time */
                if (++sofi->keepalive_rx_ctr > NN_SOFI_KEEPALIVE_TIMEOUT_COUNTER) {
                    printf("OFI: SOFI: Connection timed out!\n");
                    sofi->shutdown_reason = NN_SOFI_SHUTDOWN_DISCONNECT;
                    nn_timer_start( &sofi->shutdown_timer, 1 );
                }

                /* Check if we have to SEND keepalive */
                else if (++sofi->keepalive_tx_ctr > NN_SOFI_KEEPALIVE_COUNTER) {
                    sofi->keepalive_tx_ctr = 0;

                    /* Send keepalive message */
                    _ofi_debug("OFI: SOFI: Sending keepalive!\n");
                    memcpy( sofi->mr_sys_ptr->outhdr, FT_PACKET_KEEPALIVE, sizeof(FT_PACKET_KEEPALIVE) );
                    iov [0].iov_base = sofi->mr_sys_ptr->outhdr;
                    iov [0].iov_len = sizeof(FT_PACKET_KEEPALIVE);
                    iov_desc[0] = fi_mr_desc( sofi->mr_slab->mr );
                    ret = ofi_tx_msg( sofi->ep, iov, iov_desc, 1, 0, NN_SOFI_KEEPALIVE_INTERVAL / 1000 );
                    if (ret) {
                        /* TODO: Handle errors */
                        printf("OFI: SOFI: Error sending keepalive! Assuming disconnected remote endpoint.\n");
                        sofi->shutdown_reason = NN_SOFI_SHUTDOWN_ERROR;
                        nn_timer_start( &sofi->shutdown_timer, 1 );
                        return;
                    }

                }

                /* Stop timer */
                nn_timer_stop( &sofi->keepalive_timer);
                return;

            case NN_TIMER_STOPPED:

                /* Restart timer when stopped */
                nn_timer_start( &sofi->keepalive_timer, NN_SOFI_KEEPALIVE_INTERVAL );
                return;

            default:
                nn_fsm_bad_action (sofi->state, src, type);
            }

        /* Zombie timer */
        case NN_SOFI_SRC_SHUTDOWN_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* We are now disconnected, stop timer */
                sofi->state = NN_SOFI_STATE_DISCONNECTED;
                nn_timer_stop( &sofi->keepalive_timer);
                nn_timer_stop(&sofi->shutdown_timer);
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
        case NN_SOFI_SRC_KEEPALIVE_TIMER:
        case NN_SOFI_SRC_SHUTDOWN_TIMER:
            switch (type) {
            case NN_TIMER_STOPPED:

                /* Wait until both timers are idle */
                if (!nn_timer_isidle(&sofi->keepalive_timer)) return;
                if (!nn_timer_isidle(&sofi->shutdown_timer)) return;
                _ofi_debug("OFI: SOFI: All timers are idle, we are safe to shutdown.\n");

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
