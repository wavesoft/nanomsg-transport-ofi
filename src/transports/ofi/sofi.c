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
#include "ofi.h"
#include "sofi.h"

#include "../../ofi.h"

#include "../../aio/ctx.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/wire.h"

/* Controlling payload headers */
const uint8_t FT_PACKET_KEEPALIVE[8] = {0xFF, 0xFF, 0xFF, 0xFF, 
                                        0xFF, 0xFF, 0xFF, 0xF0 };
const uint8_t FT_PACKET_SHUTDOWN[8]  = {0xFF, 0xFF, 0xFF, 0xFF, 
                                        0xFF, 0xFF, 0xFF, 0xF1 };

#define FI_MR_DESC_OFFSET(mr,base,reference) \
        ((void*)fi_mr_desc(mr) + (((void*)reference) - ((void*)base)))


/* Helper macro to enable or disable verbose logs on console */
#ifdef OFI_DEBUG_LOG
    /* Enable debug */
    #define _ofi_debug(...)   printf(__VA_ARGS__)
#else
    /* Disable debug */
    #define _ofi_debug(...)
#endif

/* State machine states */
#define NN_SOFI_STATE_IDLE                  1
#define NN_SOFI_STATE_CONNECTED             2
#define NN_SOFI_STATE_STOPPING              3
#define NN_SOFI_STATE_DISCONNECTED          4
#define NN_SOFI_STATE_DISCONNECTING         5

/* Private SOFI events */
#define NN_SOFI_ACTION_PRE_RX               2010
#define NN_SOFI_ACTION_RX                   2011
#define NN_SOFI_ACTION_TX                   2012
#define NN_SOFI_ACTION_ERROR                2013
#define NN_SOFI_ACTION_DISCONNECT           2014

/* Private SOFI sources */
#define NN_SOFI_SRC_SHUTDOWN_TIMER          1100
#define NN_SOFI_SRC_KEEPALIVE_TIMER         1101

/* Configurable times for keepalive */
#define NN_SOFI_KEEPALIVE_TIMEOUT           1000

/* Shutdown reasons */
#define NN_SOFI_SHUTDOWN_DISCONNECT         1
#define NN_SOFI_SHUTDOWN_ERROR              2

/* MR Keys */
#define NN_SOFI_MR_KEY_USER                 1
#define NN_SOFI_MR_KEY_SLAB                 2
#define NN_SOFI_MR_KEY_INMSG                3

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

/* Polling function forward declaration */
static void nn_sofi_poller_thread (void *arg);

/* =============================================================== */
/* == BEGIN HACKING ============================================== */
/* =============================================================== */

#include "../../utils/atomic.h"
#include <assert.h>

#define NN_SOFI_CHUNK_TAG 0xdeadcafe

typedef void (*nn_ofi_chunk_free_fn) (void *p);

/* Local description of nn_chunk for hacking */
struct nn_sofi_chunk {

    /*  Number of places the chunk is referenced from. */
    struct nn_atomic refcount;

    /*  Size of the message in bytes. */
    size_t size;

    /*  Deallocation function. */
    nn_ofi_chunk_free_fn ffn;

    /*  The structure if followed by optional empty space, a 32 bit unsigned
        integer specifying the size of said empty space, a 32 bit tag and
        the message data itself. */
};

static struct nn_sofi_chunk *nn_sofi_chunk_getptr (void *p)
{
    uint32_t off;

    nn_assert (nn_getl ((uint8_t*) p - sizeof (uint32_t)) == NN_SOFI_CHUNK_TAG);
    off = nn_getl ((uint8_t*) p - 2 * sizeof (uint32_t));

    return (struct  nn_sofi_chunk*) ((uint8_t*) p - 2 *sizeof (uint32_t) - off -
        sizeof (struct nn_sofi_chunk));
}

/**
 * Hack to update chunkref size
 */
void nn_sofi_DANGEROUS_hack_chunk_size( void * ptr, size_t size )
{
    /* Access the internals of the chunk */
    struct nn_sofi_chunk * chunk = nn_sofi_chunk_getptr(ptr);
    /* Fake size without reallocation */
    // printf("!!!! Hacking from %lu to %lu (ptr=%p) !!!!\n", 
    //     chunk->size, size, ptr);
    chunk->size = size;
}

/* =============================================================== */
/* == END HACKING ================================================ */
/* =============================================================== */

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
    // nn_efd_init( &self->sync );

    /* ==================== */

    /* Initialize pipe base */
    _ofi_debug("OFI: SOFI: Replacing pipebase\n");
    nn_pipebase_init (&self->pipebase, &nn_sofi_pipebase_vfptr, epbase);

    /* ==================== */

    /* Get configured slab size */
    size_t opt_sz = sizeof(self->slab_size);
    nn_epbase_getopt (epbase, NN_OFI, NN_OFI_SLABMR_SIZE,
        &self->slab_size, &opt_sz);
    nn_epbase_getopt (epbase, NN_SOL_SOCKET, NN_RCVBUF,
        &self->recv_buffer_size, &opt_sz);
    _ofi_debug("OFI: SOFI: Socket options NN_OFI_SLABMR_SIZE=%i, NN_RCVBUF=%i\n"
        , self->slab_size, self->recv_buffer_size);

    /* Allocate slab buffer */
    size_t slab_size = sizeof( struct nn_ofi_sys_ptrs ) + self->slab_size;
    self->mr_slab_ptr = nn_alloc( slab_size, "ofi (slab memory)" );
    if (!self->mr_slab_ptr) {
        printf("OFI: SOFI: ERROR: Unable to allocate slab memory region!\n");
        return;
    }

    /* Get pointer to slab user/slab data */
    self->ptr_slab_sysptr = (struct nn_ofi_sys_ptrs *) self->mr_slab_ptr;
    // self->mr_slab_data_in = self->mr_slab_ptr + sizeof( struct nn_ofi_sys_ptrs );
    // self->ptr_slab_out = self->mr_slab_data_in + self->slab_size;
    self->ptr_slab_out = self->mr_slab_ptr + sizeof( struct nn_ofi_sys_ptrs );

    /* [1] MR Helper : For SLAB */
    ret = ofi_mr_alloc( self->ep, &self->mr_slab );
    if (ret) {
       /* TODO: Handle error */
       printf("OFI: SOFI: ERROR: Unable to alloc an MR obj for slab slab!\n");
       return;
    }

    /* Mark the memory region */
    ret = ofi_mr_manage( self->ep, self->mr_slab, self->mr_slab_ptr, 
        slab_size, NN_SOFI_MR_KEY_SLAB, MR_SEND | MR_RECV );
    if (ret) {
       /* TODO: Handle error */
       printf("OFI: SOFI: ERROR: Unable to mark the slab memory MR region!\n");
       return;
    }

    /* ==================== */

    /* [2] MR Helper : For USER DATA */
    ret = ofi_mr_alloc( self->ep, &self->mr_user );
    if (ret) {
       /* TODO: Handle error */
       printf("OFI: SOFI: ERROR: Unable to alloc MR obj for user!\n");
       return;
    }

    /* ==================== */

    /**
     * Allocate a reusable chunk for incoming messages
     */
    ret = nn_chunk_alloc(self->recv_buffer_size, 0, (void**)&self->inmsg_chunk);
    if (ret) {
       /* TODO: Handle error */
       printf("OFI: SOFI: ERROR: Unable to alloc inmsg chunk!\n");
       return;
    }

    /* Increment reference counter by 1 so it's not disposed on msg_term */
    nn_chunk_addref( self->inmsg_chunk, 1 );

    /* [3] MR Helper : For INPTR */
    ret = ofi_mr_alloc( self->ep, &self->mr_inmsg );
    if (ret) {
       /* TODO: Handle error */
       printf("OFI: SOFI: ERROR: Unable to alloc an MR obj for inmsg!\n");
       return;
    }

    /* Mark the memory region */
    ret = ofi_mr_manage( self->ep, self->mr_inmsg, self->inmsg_chunk, 
        self->recv_buffer_size, NN_SOFI_MR_KEY_INMSG, MR_RECV );
    if (ret) {
       /* TODO: Handle error */
       printf("OFI: SOFI: ERROR: Unable to mark the slab memory MR region!\n");
       return;
    }

    /* ==================== */

    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_handler, nn_sofi_shutdown,
        src, self, owner);
    self->state = NN_SOFI_STATE_IDLE;
    self->error = 0;

    /* Initialize timer */
    // nn_timer_init(&self->shutdown_timer, NN_SOFI_SRC_SHUTDOWN_TIMER, &self->fsm);
    // self->shutdown_reason = 0;
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
    _ofi_debug("OFI: SOFI: Terminating\n");

    /* Free memory */
    nn_chunk_free( self->inmsg_chunk );
    nn_free( self->mr_slab_ptr );

    /* Cleanup instantiated resources */
    nn_list_item_term (&self->item);
    // nn_timer_term (&self->shutdown_timer);
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
    _ofi_debug("OFI: SOFI: Stopping\n");

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

        /*  Unmanage memory regions. */
        _ofi_debug("OFI: Freeing memory resources\n");
        ofi_mr_free( sofi->ep, &sofi->mr_slab );
        ofi_mr_free( sofi->ep, &sofi->mr_user );
        ofi_mr_free( sofi->ep, &sofi->mr_inmsg );

        /*  Stop endpoint and wait for worker. */
        _ofi_debug("OFI: Freeing endpoint resources\n");
        ofi_shutdown_ep( sofi->ep );
        nn_thread_term (&sofi->thread);
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
        memcpy( self->ptr_slab_out, ptr, len );
        /* Update pointer */
        *sendptr = self->ptr_slab_out;
        *descptr = FI_MR_DESC_OFFSET( self->mr_slab->mr, self->ptr_slab_out, self->mr_slab_ptr );
    }
    else
    {
        /* Manage this memory region */
        ofi_mr_manage( self->ep, self->mr_user, ptr, len, NN_SOFI_MR_KEY_USER, MR_SEND );
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

    /* Send only if connected */
    if (sofi->state != NN_SOFI_STATE_CONNECTED)
        return -EBADF;

    /*  Start async sending. */
    size_t sz_outhdr = sizeof(sofi->ptr_slab_sysptr->outhdr);
    size_t sz_sphdr = nn_chunkref_size (&msg->sphdr);
    size_t sz_body = nn_chunkref_size (&msg->body);

    /*  Move the message to the local storage. */
    nn_msg_term (&sofi->outmsg);
    nn_msg_mv (&sofi->outmsg, msg);

    /* Manage this memory region */
    ofi_mr_manage( sofi->ep, sofi->mr_user, 
        nn_chunkref_data (&sofi->outmsg.body), sz_body, NN_SOFI_MR_KEY_USER, MR_SEND );

    _ofi_debug("OFI: SOFI: Sending payload (len=%lu)\n", sz_body );
    ret = fi_send( sofi->ep->ep, nn_chunkref_data (&sofi->outmsg.body), sz_body, 
        fi_mr_desc( sofi->mr_user->mr ), sofi->ep->remote_fi_addr, &sofi->ep->tx_ctx);
    if (ret) {

        /* If we are in a bad state, we were remotely disconnected */
        if (ret == -FI_EOPBADSTATE) {
            _ofi_debug("OFI: SOFI: fi_send returned -FI_EOPBADSTATE, considering shutdown.\n");
            return -EBADF;           
        }

        /* Otherwise display error */
        FT_PRINTERR("nn_sofi_send", ret);
        sofi->error = ret;
        nn_fsm_action ( &sofi->fsm, NN_SOFI_ACTION_ERROR );
        return 0;
    }

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
    nn_chunk_addref( sofi->inmsg_chunk, 1 );
    nn_msg_mv (msg, &sofi->inmsg);
    nn_msg_init (&sofi->inmsg, 0);

    /* Tell fsm to prepare send buffers */
    _ofi_debug("OFI: SOFI: Acknowledging rx data!\n");
    nn_fsm_action ( &sofi->fsm, NN_SOFI_ACTION_PRE_RX );

    /* Success */
    return 0;
}


/**
 * The internal poller thread, since OFI does not 
 * have blocking UNIX file descriptors
 */
static void nn_sofi_poller_thread (void *arg)
{
    int ret;
    struct nn_sofi * self = (struct nn_sofi *) arg;
    struct fi_eq_cm_entry eq_entry;
    struct fi_cq_err_entry err_entry;
    struct fi_cq_data_entry cq_entry;
    uint8_t fastpoller = 100;
    uint32_t event;

    /* Keep thread alive while  */
    _ofi_debug("OFI: SOFI: Starting poller thread\n");
    while ( self->state == NN_SOFI_STATE_CONNECTED ) {

        /* ========================================= */
        /* Wait for Rx CQ event */
        /* ========================================= */
        ret = fi_cq_read( self->ep->rx_cq, &cq_entry, 1 );
        if (nn_slow(ret > 0)) {

            /* Initialize a new message on the shared pointer */
            nn_msg_init_chunk (&self->inmsg, self->inmsg_chunk);
            _ofi_debug("OFI: SOFI: Got incoming message of %li bytes\n", cq_entry.len);

            /* Hack to force new message size on the chunkref */
            if (cq_entry.len <= self->recv_buffer_size) {
                nn_sofi_DANGEROUS_hack_chunk_size( self->inmsg_chunk, cq_entry.len );
            } else {
                printf("WARNING: Silent data truncation from %lu to %d (increase your receive buffer size!)\n", cq_entry.len, self->recv_buffer_size );
                nn_sofi_DANGEROUS_hack_chunk_size( self->inmsg_chunk, self->recv_buffer_size );
            }

            /* Handle the fact that the data are received  */
            _ofi_debug("OFI: SOFI: Rx CQ Event\n");
            nn_ctx_enter( self->fsm.ctx );
            nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_RX );
            nn_ctx_leave( self->fsm.ctx );

        } else if (nn_slow(ret != -FI_EAGAIN)) {
            if (ret == -FI_EAVAIL) {

                /* Get error details */
                ret = fi_cq_readerr( self->ep->rx_cq, &err_entry, 0 );
                if (err_entry.err == FI_ECANCELED) {

                    /* The socket operation was cancelled, we were disconnected */
                    _ofi_debug("OFI: SOFI: Rx CQ Error (-FI_ECANCELED) caused disconnection\n");
                    nn_ctx_enter( self->fsm.ctx );
                    nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_DISCONNECT );
                    nn_ctx_leave( self->fsm.ctx );
                    break;

                }

                /* Handle error */
                self->error = -err_entry.err;
                _ofi_debug("OFI: SOFI: Rx CQ Error (%i)\n", -err_entry.err);
                nn_ctx_enter( self->fsm.ctx );
                nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_ERROR );
                nn_ctx_leave( self->fsm.ctx );

            } else {

                /* Unexpected CQ Read error */
                FT_PRINTERR("fi_cq_read<rx_cq>", ret);
                self->error = ret;
                nn_ctx_enter( self->fsm.ctx );
                nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_ERROR );
                nn_ctx_leave( self->fsm.ctx );

            }
        }

        /* ========================================= */
        /* Wait for Tx CQ event */
        /* ========================================= */
        ret = fi_cq_read( self->ep->tx_cq, &cq_entry, 1 );
        if (nn_slow(ret > 0)) {

            /* Handle the fact that the data are sent */
            _ofi_debug("OFI: SOFI: Tx CQ Event\n");
            nn_ctx_enter( self->fsm.ctx );
            nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_TX );
            nn_ctx_leave( self->fsm.ctx );

        } else if (nn_slow(ret != -FI_EAGAIN)) {

            if (ret == -FI_EAVAIL) {

                /* Get error details */
                ret = fi_cq_readerr( self->ep->tx_cq, &err_entry, 0 );
                if (err_entry.err == FI_ECANCELED) {

                    /* The socket operation was cancelled, we were disconnected */
                    _ofi_debug("OFI: SOFI: Tx CQ Error (-FI_ECANCELED) caused disconnection\n");
                    nn_ctx_enter( self->fsm.ctx );
                    nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_DISCONNECT );
                    nn_ctx_leave( self->fsm.ctx );
                    break;

                }

                /* Handle error */
                self->error = -err_entry.err;
                _ofi_debug("OFI: SOFI: Tx CQ Error (%i)\n", -err_entry.err);
                nn_ctx_enter( self->fsm.ctx );
                nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_ERROR );
                nn_ctx_leave( self->fsm.ctx );

            } else {

                /* Unexpected CQ Read error */
                FT_PRINTERR("fi_cq_read<tx_cq>", ret);
                self->error = ret;
                nn_ctx_enter( self->fsm.ctx );
                nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_ERROR );
                nn_ctx_leave( self->fsm.ctx );

            }

        }

        /* ========================================= */
        /* Wait for EQ events */
        /* ========================================= */
        ret = fi_eq_read( self->ep->eq, &event, &eq_entry, sizeof eq_entry, 0);
        if (nn_fast(ret != -FI_EAGAIN)) {

            /* Check for socket disconnection */
            if (event == FI_SHUTDOWN) {
                _ofi_debug("OFI: SOFI: Got shutdown EQ event\n");
                nn_ctx_enter( self->fsm.ctx );
                nn_fsm_action ( &self->fsm, NN_SOFI_ACTION_DISCONNECT );
                nn_ctx_leave( self->fsm.ctx );
                break;
            }

        }

        /* Microsleep for lessen the CPU load */
        if (!fastpoller--) {
            usleep(10);
            fastpoller = 100;
        }

    }
    _ofi_debug("OFI: SOFI: Exited poller thread\n");

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
                nn_timer_start( &sofi->keepalive_timer, NN_SOFI_KEEPALIVE_TIMEOUT );

                /* Send initialization event */
                nn_fsm_action ( &sofi->fsm, NN_SOFI_ACTION_PRE_RX );

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

        /* Local Actions */
        case NN_FSM_ACTION:
            switch (type) {

            /* Post receive buffers to prepare for reception */
            case NN_SOFI_ACTION_PRE_RX:

                /* Post receive buffers */
                _ofi_debug("OFI: SOFI: Posting receive buffers (max_rx=%i)\n", sofi->recv_buffer_size);
                ret = ofi_rx_post( sofi->ep, sofi->inmsg_chunk, sofi->recv_buffer_size, fi_mr_desc( sofi->mr_inmsg->mr ) );

                /* Handle errors */
                if (ret == -FI_REMOTE_DISCONNECT) { /* Remotely disconnected */
                    _ofi_debug("OFI: SOFI: Could not post receive buffers because remote endpoit is in invalid state!\n");
                    nn_fsm_action ( &sofi->fsm, NN_SOFI_ACTION_DISCONNECT );
                } else if (ret) {
                    sofi->error = ret;
                    _ofi_debug("OFI: SOFI: Could not post receive buffers, got error %i!\n", ret);
                    nn_fsm_action ( &sofi->fsm, NN_SOFI_ACTION_ERROR );
                }
                return;

            /* The data on the input buffer are populated */
            case NN_SOFI_ACTION_RX:

                /* Data are received, notify pipebase */
                nn_pipebase_received (&sofi->pipebase);

                /* Restart keepalive timeout */
                nn_timer_stop( &sofi->keepalive_timer);

                return;

            /* The data on the output buffer are sent */
            case NN_SOFI_ACTION_TX:

                /* Data are sent, notify pipebase */
                nn_pipebase_sent (&sofi->pipebase);

                /* Restart keepalive timeout */
                nn_timer_stop( &sofi->keepalive_timer);

                return;

            /* An error occured on the socket */
            case NN_SOFI_ACTION_ERROR:

                _ofi_debug("OFI: SOFI: An error occured (%i)\n", sofi->error);

            /* The socket was disconnected */
            case NN_SOFI_ACTION_DISCONNECT:

                /* We are disconnected, stop timer */
                _ofi_debug("OFI: SOFI: Disconnected\n");
                sofi->state = NN_SOFI_STATE_DISCONNECTING;
                nn_timer_stop(&sofi->keepalive_timer);
                // nn_timer_stop(&sofi->shutdown_timer);

                return;

            default:
                nn_fsm_bad_action (sofi->state, src, type);
            }

        /* Keepalive timer */
        case NN_SOFI_SRC_KEEPALIVE_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* Timeout reached */
                _ofi_debug("OFI: SOFI: Connection timeout due to lack of traffic\n");
                sofi->state = NN_SOFI_STATE_DISCONNECTING;
                nn_timer_stop( &sofi->keepalive_timer);
                // nn_timer_stop(&sofi->shutdown_timer);
                return;

            case NN_TIMER_STOPPED:

                /* Restart timer when stopped */
                nn_timer_start( &sofi->keepalive_timer, NN_SOFI_KEEPALIVE_TIMEOUT );
                return;

            default:
                nn_fsm_bad_action (sofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (sofi->state, src, type);
        }

/******************************************************************************/
/*  DISCONNECTING state.                                                      */
/******************************************************************************/

    case NN_SOFI_STATE_DISCONNECTING:
        switch (src) {

        /* Local Actions */
        case NN_FSM_ACTION:
            switch (type) {

            /* Do not post Rx buffers */
            case NN_SOFI_ACTION_PRE_RX:
                return;

            /* Do notify completion of rx data */
            case NN_SOFI_ACTION_RX:
                nn_pipebase_received (&sofi->pipebase);
                return;

            /* Do notify completing of tx data */
            case NN_SOFI_ACTION_TX:
                nn_pipebase_sent (&sofi->pipebase);
                return;

            /* Ignore errors and disconnect events when disconnecting */
            case NN_SOFI_ACTION_ERROR:
            case NN_SOFI_ACTION_DISCONNECT:
                return;

            default:
                nn_fsm_bad_action (sofi->state, src, type);
            }

        /* Keepalive timer */
        case NN_SOFI_SRC_KEEPALIVE_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:

                /* Just stop timer if it kicks-in */
                nn_timer_stop( &sofi->keepalive_timer);
                return;

            case NN_TIMER_STOPPED:

                /* We are disconnected */
                sofi->state = NN_SOFI_STATE_DISCONNECTED;
                nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_DISCONNECT );
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

        /* Local Actions */
        case NN_FSM_ACTION:
            switch (type) {

            /* Ignore local FSM actions when we are shutting down */
            case NN_SOFI_ACTION_PRE_RX:
            case NN_SOFI_ACTION_RX:
            case NN_SOFI_ACTION_TX:
            case NN_SOFI_ACTION_ERROR:
                return;

            /* Handle the disconnection confirmation */
            case NN_SOFI_ACTION_DISCONNECT:

                /* Notify parent fsm that we are disconnected */
                nn_fsm_raise(&sofi->fsm, &sofi->disconnected, 
                    NN_SOFI_DISCONNECTED);
                return;

            default:
                nn_fsm_bad_action (sofi->state, src, type);
            }

        /* Zombie timer */
        case NN_SOFI_SRC_KEEPALIVE_TIMER:
            switch (type) {
            case NN_TIMER_STOPPED:

                /* Wait until both timers are idle */
                if (!nn_timer_isidle(&sofi->keepalive_timer)) return;
                _ofi_debug("OFI: SOFI: All timers are idle, we are safe to shutdown.\n");

                /* Send disconnection notification */
                nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_DISCONNECT );

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
