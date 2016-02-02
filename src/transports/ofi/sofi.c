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

// #define NN_FSM_ACTION_GUARD(fsm,action) \
//         nn_ctx_enter( (fsm)->ctx ); \
//         nn_fsm_action ( fsm, action ); \
//         nn_ctx_leave( (fsm)->ctx );

#define NN_FSM_ACTION_FEED(fsm,src,event,ptr) \
        nn_ctx_enter ( (fsm)->ctx); \
        nn_fsm_feed ( fsm, src, event, ptr); \
        nn_ctx_leave ( (fsm)->ctx);


/* Helper macro to enable or disable verbose logs on console */
#ifdef OFI_DEBUG_LOG
    #include <pthread.h>
    /* Enable debug */
    // #define _ofi_debug(...)   printf("[%012i] ", pthread_self()); printf(__VA_ARGS__)
    #define _ofi_debug(...)     printf(__VA_ARGS__)
#else
    /* Disable debug */
    #define _ofi_debug(...)
#endif

/* State machine states */
#define NN_SOFI_STATE_IDLE                  1
#define NN_SOFI_STATE_CONNECTED             2
#define NN_SOFI_STATE_DISCONNECTED          4
#define NN_SOFI_STATE_DISCONNECTING         5

/* Input-Specific Sub-State Machine */
#define NN_SOFI_INSTATE_IDLE                1
#define NN_SOFI_INSTATE_POSTED              2
#define NN_SOFI_INSTATE_PENDING             3
#define NN_SOFI_INSTATE_ERROR               4
#define NN_SOFI_INSTATE_CLOSED              5

#define NN_SOFI_INACTION_ENTER              1
#define NN_SOFI_INACTION_RX_DATA            2
#define NN_SOFI_INACTION_RX_ACK             3
#define NN_SOFI_INACTION_ERROR              4
#define NN_SOFI_INACTION_CLOSE              5

/* Output-Specific Sub-State Machine */
#define NN_SOFI_OUTSTATE_IDLE               1
#define NN_SOFI_OUTSTATE_POSTED             2
#define NN_SOFI_OUTSTATE_PENDING            3
#define NN_SOFI_OUTSTATE_ERROR              4
#define NN_SOFI_OUTSTATE_CLOSED             5

#define NN_SOFI_OUTACTION_ENTER             1
#define NN_SOFI_OUTACTION_TX_DATA           2
#define NN_SOFI_OUTACTION_TX_ACK            3
#define NN_SOFI_OUTACTION_ERROR             4
#define NN_SOFI_OUTACTION_CLOSE             5

/* Private SOFI events */
#define NN_SOFI_ACTION_RX                   2011
#define NN_SOFI_ACTION_TX                   2012
#define NN_SOFI_ACTION_ERROR                2013
#define NN_SOFI_ACTION_DISCONNECT           2014
#define NN_SOFI_ACTION_IN_CLOSED            2020
#define NN_SOFI_ACTION_IN_ERROR             2021
#define NN_SOFI_ACTION_OUT_CLOSED           2030
#define NN_SOFI_ACTION_OUT_ERROR            2031

/* SOFI Asynchronous tasks */
#define NN_SOFI_TASK_RX                     3000
#define NN_SOFI_TASK_TX                     3001
#define NN_SOFI_TASK_ERROR                  3002
#define NN_SOFI_TASK_DISCONNECT             3003

/* Private SOFI sources */
#define NN_SOFI_SRC_SHUTDOWN_TIMER          1100
#define NN_SOFI_SRC_KEEPALIVE_TIMER         1101
#define NN_SOFI_SRC_POLLER_THREAD           1102

/* Configurable times for keepalive */
#define NN_SOFI_KEEPALIVE_TIMEOUT           2000

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

/* Sub-FSMs for Input and Output */
static void nn_sofi_input_action(struct nn_sofi *sofi, int action);
static void nn_sofi_output_action(struct nn_sofi *sofi, int action);

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
    nn_mutex_init (&self->rx_sync);
    nn_mutex_init (&self->tx_sync);

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
    self->instate = NN_SOFI_INSTATE_IDLE;
    self->outstate = NN_SOFI_OUTSTATE_IDLE;
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
    nn_worker_task_init (&self->task_disconnect, NN_SOFI_TASK_DISCONNECT,
        &self->fsm);

    /* Initialize timer */
    nn_timer_init(&self->shutdown_timer, NN_SOFI_SRC_SHUTDOWN_TIMER, &self->fsm);
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
    nn_chunk_free( self->inmsg_chunk );
    nn_free( self->mr_slab_ptr );

    /* Cleanup worker tasks */
    nn_worker_cancel (self->worker, &self->task_disconnect);
    nn_worker_cancel (self->worker, &self->task_error);
    nn_worker_cancel (self->worker, &self->task_tx);
    nn_worker_cancel (self->worker, &self->task_rx);
    nn_worker_task_term (&self->task_disconnect);
    nn_worker_task_term (&self->task_error);
    nn_worker_task_term (&self->task_tx);
    nn_worker_task_term (&self->task_rx);

    /* Cleanup efd */
    nn_mutex_term( &self->rx_sync );
    nn_mutex_term( &self->tx_sync );

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

        /* Disconnect SOFI */
        nn_sofi_disconnect( sofi );

        // /* Abort OFI Operations */
        // sofi->state = NN_SOFI_STATE_DISCONNECTING;

        // /* Stop keepalive timer */
        // nn_timer_stop( &sofi->keepalive_timer);
    }
    if (nn_slow (sofi->state == NN_SOFI_STATE_DISCONNECTING )) {

        /* Wait until everything is closed */
        if (sofi->instate != NN_SOFI_INSTATE_CLOSED) return;
        if (sofi->outstate != NN_SOFI_OUTSTATE_CLOSED) return;
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
 * Disconnect FSM
 *
 * Depending on the currnet state of the FSM route 
 * towrads shutdown.
 */
static void nn_sofi_disconnect ( struct nn_sofi *self )
{

    nn_assert( self->state != NN_SOFI_STATE_IDLE );
    nn_assert( self->state != NN_SOFI_STATE_DISCONNECTED );

    /* Switch to disconnecting */
    self->state = NN_SOFI_STATE_DISCONNECTING;

    /* Stop inner FSMs */
    nn_sofi_output_action( self, NN_SOFI_OUTACTION_CLOSE );
    nn_sofi_input_action( self, NN_SOFI_INACTION_CLOSE );

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
    if (sofi->state != NN_SOFI_STATE_CONNECTED) {
        _ofi_debug("OFI: SOFI: Sending on a non-connected socket\n");
        return -EBADF;
    }
    nn_assert( sofi->outstate == NN_SOFI_OUTSTATE_IDLE );

    /*  Move the message to the local storage. */
    nn_msg_term (&sofi->outmsg);
    nn_msg_mv (&sofi->outmsg, msg);

    /* Inform output FSM that there are data pending */
    nn_sofi_output_action( sofi, NN_SOFI_OUTACTION_TX_DATA );

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

    _ofi_debug("OFI: SOFI: Handling reception of data\n");

    /* Move received message to the user. */
    // nn_mutex_lock (&sofi->rx_sync);
    nn_chunk_addref( sofi->inmsg_chunk, 1 );
    nn_msg_mv (msg, &sofi->inmsg);
    nn_msg_init (&sofi->inmsg, 0);

    /* Continue input FSM */
    nn_sofi_input_action( sofi, NN_SOFI_INACTION_RX_ACK );

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
    uint8_t fastpoller = 200;
    uint32_t event;

    /* Keep thread alive while  */
    _ofi_debug("OFI: SOFI: Starting poller thread\n");
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
                    _ofi_debug("OFI: SOFI: Rx CQ Error (-FI_ECANCELED) caused disconnection\n");
                    nn_worker_execute (self->worker, &self->task_disconnect);
                    // nn_sofi_input_action( self, NN_SOFI_INACTION_CLOSE );

                    // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_DISCONNECT, NULL );
                    break;

                }

                /* Handle error */
                _ofi_debug("OFI: SOFI: Rx CQ Error (%i)\n", -err_entry.err);
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
            _ofi_debug("OFI: SOFI: Tx CQ Event\n");
            nn_worker_execute (self->worker, &self->task_tx);
            // nn_sofi_output_action( self, NN_SOFI_OUTACTION_TX_ACK );
            // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_TX, NULL );

        } else if (nn_slow(ret != -FI_EAGAIN)) {

            if (ret == -FI_EAVAIL) {

                /* Get error details */
                ret = fi_cq_readerr( self->ep->tx_cq, &err_entry, 0 );
                if (err_entry.err == FI_ECANCELED) {

                    /* The socket operation was cancelled, we were disconnected */
                    _ofi_debug("OFI: SOFI: Tx CQ Error (-FI_ECANCELED) caused disconnection\n");
                    nn_worker_execute (self->worker, &self->task_disconnect);
                    // nn_sofi_output_action( self, NN_SOFI_OUTACTION_CLOSE );
                    // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_DISCONNECT, NULL );
                    break;

                }

                /* Handle error */
                _ofi_debug("OFI: SOFI: Tx CQ Error (%i)\n", -err_entry.err);
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
                _ofi_debug("OFI: SOFI: Got shutdown EQ event\n");
                nn_worker_execute (self->worker, &self->task_disconnect);
                // nn_sofi_disconnect( self );
                // nn_sofi_output_action( self, NN_SOFI_OUTACTION_CLOSE );
                // nn_sofi_input_action( self, NN_SOFI_INACTION_CLOSE );
                // NN_FSM_ACTION_FEED ( &self->fsm, NN_SOFI_SRC_POLLER_THREAD, NN_SOFI_ACTION_DISCONNECT, NULL );
                break;
            }

        }

        /* Microsleep for lessen the CPU load */
        if (!fastpoller--) {
            usleep(10);
            fastpoller = 200;
        }

    }
    _ofi_debug("OFI: SOFI: Exited poller thread\n");

}

/**
 * Streaming OFI [INPUT] FSM Handler
 */
static void nn_sofi_input_action(struct nn_sofi *sofi, int action)
{
    int ret;

    switch (sofi->instate) {
/******************************************************************************/
/*  INPUT - IDLE state.                                                       */
/******************************************************************************/
        case NN_SOFI_INSTATE_IDLE:
        switch (sofi->state) {

            /* FSM Branch on [CONNECTED] state */
            case NN_SOFI_STATE_CONNECTED:
            switch (action) {
                case NN_SOFI_INACTION_ENTER:

                    /* Post receive buffers */
                    _ofi_debug("OFI: SOFI: Posting receive buffers (max_rx=%i)\n", sofi->recv_buffer_size);
                    ret = ofi_rx_post( sofi->ep, sofi->inmsg_chunk, sofi->recv_buffer_size, fi_mr_desc( sofi->mr_inmsg->mr ) );

                    /* Handle errors */
                    if (ret == -FI_REMOTE_DISCONNECT) { /* Remotely disconnected */

                        /* Update state and release mutex */
                        _ofi_debug("OFI: SOFI: Could not post receive buffers because remote endpoit is in invalid state!\n");

                        /* Switch directly to closed */
                        sofi->instate = NN_SOFI_INSTATE_CLOSED;
                        /* Notify core fsm for the fact that IN-FSM is closed */
                        nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_IN_CLOSED );
                        return;

                    } else if (ret) {

                        /* Update state and release mutex */
                        _ofi_debug("OFI: SOFI: Could not post receive buffers, got error %i!\n", ret);

                        /* Trigger error */
                        sofi->error = ret;
                        sofi->instate = NN_SOFI_INSTATE_ERROR;
                        nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_IN_ERROR );
                        return;

                    }

                    /* Switch to posted state */
                    sofi->instate = NN_SOFI_INSTATE_POSTED;

                    return;


                default:
                    nn_fsm_bad_action( sofi->instate, sofi->state, action );
                }

            /* FSM Branch on [DISCONNECTING] state */
            case NN_SOFI_STATE_DISCONNECTING:
            switch (action) {
                case NN_SOFI_INACTION_ENTER:
                case NN_SOFI_INACTION_CLOSE:

                    /* Switch directly to closed */
                    sofi->instate = NN_SOFI_INSTATE_CLOSED;
                    /* Notify core fsm for the fact that IN-FSM is closed */
                    nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_IN_CLOSED );
                    return;

                default:
                    nn_fsm_bad_action( sofi->instate, sofi->state, action );
                }

            default:
                nn_fsm_bad_source( sofi->instate, sofi->state, action );

        }


/******************************************************************************/
/*  INPUT - POSTED state.                                                     */
/******************************************************************************/
        case NN_SOFI_INSTATE_POSTED:
        switch (sofi->state) {

            /* Both [CONNECTED] and [DISCONNECTED] branches are the same */
            case NN_SOFI_STATE_CONNECTED:
            case NN_SOFI_STATE_DISCONNECTING:
            switch (action) {
                case NN_SOFI_INACTION_RX_DATA:

                    /* Handle data and switch to pending */
                    sofi->instate = NN_SOFI_INSTATE_PENDING;
                    nn_pipebase_received (&sofi->pipebase);
                    return;

                case NN_SOFI_INACTION_ERROR:

                    /* If an error occured */
                    sofi->instate = NN_SOFI_INSTATE_ERROR;
                    /* Notify core fsm for the fact that IN-FSM has error */
                    nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_IN_ERROR );
                    return;

                case NN_SOFI_INACTION_CLOSE:

                    /* Switch directly to closed */
                    sofi->instate = NN_SOFI_INSTATE_CLOSED;
                    /* Notify core fsm for the fact that IN-FSM is closed */
                    nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_IN_CLOSED );
                    return;

                default:
                    nn_fsm_bad_action( sofi->instate, sofi->state, action );
                }

            default:
                nn_fsm_bad_source( sofi->instate, sofi->state, action );

        }

/******************************************************************************/
/*  INPUT - PENDING state.                                                    */
/******************************************************************************/
        case NN_SOFI_INSTATE_PENDING:
        switch (sofi->state) {

            /* FSM Branch on [CONNECTED] state */
            case NN_SOFI_STATE_CONNECTED:
            switch (action) {
                case NN_SOFI_INACTION_RX_ACK:

                    /* After acknowledgement of input action, switch back to input */
                    sofi->instate = NN_SOFI_STATE_IDLE;
                    nn_sofi_input_action( sofi, NN_SOFI_INACTION_ENTER );
                    return;

                case NN_SOFI_INACTION_ERROR:

                    /* If an error occured */
                    sofi->instate = NN_SOFI_INSTATE_ERROR;
                    /* Notify core fsm for the fact that IN-FSM has error */
                    nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_IN_ERROR );
                    return;

                case NN_SOFI_INACTION_RX_DATA:

                    /* TODO: Be smart on duble-buffering here perhaps?? */
                    printf("OFI: WARNING: Rx Buffer Overrun!\n");
                    return;

                default:
                    nn_fsm_bad_action( sofi->instate, sofi->state, action );
                }

            /* FSM Branch on [DISCONNECTING] state */
            case NN_SOFI_STATE_DISCONNECTING:
            switch (action) {
                case NN_SOFI_INACTION_RX_ACK:
                case NN_SOFI_INACTION_ERROR:

                    /* When disconnecting, both these events are an opportuninty to close */
                    sofi->instate = NN_SOFI_INSTATE_CLOSED;
                    /* Notify core fsm for the fact that IN-FSM is closed */
                    nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_IN_CLOSED );

                    return;

                case NN_SOFI_INACTION_CLOSE:

                    /* Ignore closed event, since the socket will be closed
                       one way or another when Rx is acknowledged (assuming that
                       the CLOSE event is triggered when core is in DISCONNECTING state) */

                    return;

                default:
                    nn_fsm_bad_action( sofi->instate, sofi->state, action );
                }

            default:
                nn_fsm_bad_source( sofi->instate, sofi->state, action );

        }


/******************************************************************************/
/*  INPUT - ERROR state.                                                      */
/******************************************************************************/
        case NN_SOFI_INSTATE_ERROR:
        switch (action) {
            case NN_SOFI_INACTION_CLOSE:

                /* When disconnecting, both these events are an opportuninty to close */
                sofi->instate = NN_SOFI_INSTATE_CLOSED;
                /* Notify core fsm for the fact that IN-FSM is closed */
                nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_IN_CLOSED );
                return;

            default:
                nn_fsm_bad_action( sofi->instate, sofi->state, action );
        }

/******************************************************************************/
/*  INPUT - CLOSED state.                                                     */
/******************************************************************************/
        case NN_SOFI_INSTATE_CLOSED:
        switch (action) {
            case NN_SOFI_INACTION_CLOSE:
                /* Ignore other close events (we are expecting 1 extra) */
                return;

            default:
                nn_fsm_bad_action( sofi->instate, sofi->state, action );
        }

    }
}


/**
 * Streaming OFI [OUTPUT] FSM Handler
 */
static void nn_sofi_output_action(struct nn_sofi *sofi, int action)
{
    int ret;
    size_t sz_outhdr, sz_sphdr, sz_body;

    switch (sofi->outstate) {
/******************************************************************************/
/*  OUTPUT - IDLE state.                                                      */
/******************************************************************************/
        case NN_SOFI_OUTSTATE_IDLE:
        switch (sofi->state) {

            /* FSM Branch on [CONNECTED] state */
            case NN_SOFI_STATE_CONNECTED:
            switch (action) {
                case NN_SOFI_OUTACTION_ENTER:

                    /* We stay idle */
                    return;

                case NN_SOFI_OUTACTION_TX_DATA:

                    /*  Start async sending. */
                    sz_outhdr = sizeof(sofi->ptr_slab_sysptr->outhdr);
                    sz_sphdr = nn_chunkref_size (&sofi->outmsg.sphdr);
                    sz_body = nn_chunkref_size (&sofi->outmsg.body);

                    /* Manage this memory region */
                    ofi_mr_manage( sofi->ep, sofi->mr_user, 
                        nn_chunkref_data (&sofi->outmsg.body), sz_body, NN_SOFI_MR_KEY_USER, MR_SEND );

                    _ofi_debug("OFI: SOFI: Sending payload (len=%lu)\n", sz_body );
                    ret = fi_send( sofi->ep->ep, nn_chunkref_data (&sofi->outmsg.body), sz_body, 
                        fi_mr_desc( sofi->mr_user->mr ), sofi->ep->remote_fi_addr, &sofi->ep->tx_ctx);
                    if (ret) {

                        /* Otherwise display error */
                        FT_PRINTERR("nn_sofi_send", ret);

                        /* If we are in a bad state, we were remotely disconnected */
                        if (ret == -FI_EOPBADSTATE) {
                            _ofi_debug("OFI: SOFI: fi_send returned -FI_EOPBADSTATE, considering shutdown.\n");

                            /* Swtich to closed state */
                            sofi->outstate = NN_SOFI_OUTSTATE_CLOSED;
                            /* Notify core fsm for the fact that IN-FSM has error */
                            nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_OUT_CLOSED );

                        } else {

                            /* Swtich to error state */
                            sofi->outstate = NN_SOFI_OUTSTATE_ERROR;
                            /* Notify core fsm for the fact that IN-FSM has error */
                            nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_OUT_ERROR );

                        }

                    } else {

                        /* We have posted the output buffers */
                        sofi->outstate = NN_SOFI_OUTSTATE_POSTED;

                    }

                    return;

                default:
                    nn_fsm_bad_action( sofi->outstate, sofi->state, action );
                }

            /* FSM Branch on [DISCONNECTING] state */
            case NN_SOFI_STATE_DISCONNECTING:
            switch (action) {
                case NN_SOFI_OUTACTION_ENTER:
                case NN_SOFI_OUTACTION_TX_DATA:
                case NN_SOFI_OUTACTION_CLOSE:

                    /* Switch directly to closed */
                    sofi->outstate = NN_SOFI_OUTSTATE_CLOSED;
                    /* Notify core fsm for the fact that IN-FSM is closed */
                    nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_OUT_CLOSED );
                    return;

                default:
                    nn_fsm_bad_action( sofi->outstate, sofi->state, action );
                }

            default:
                nn_fsm_bad_source( sofi->outstate, sofi->state, action );

        }


/******************************************************************************/
/*  OUTPUT - POSTED state.                                                    */
/******************************************************************************/
        case NN_SOFI_OUTSTATE_POSTED:
        switch (sofi->state) {

            /* FSM Branch on [CONNECTED] state */
            case NN_SOFI_STATE_CONNECTED:
            switch (action) {
                case NN_SOFI_OUTACTION_TX_ACK:

                    /* Notify pipebase for the fact that the data ere sent */
                    sofi->outstate = NN_SOFI_OUTSTATE_IDLE;
                    nn_pipebase_sent(&sofi->pipebase);
                    return;

                default:
                    nn_fsm_bad_action( sofi->outstate, sofi->state, action );
                }

            /* FSM Branch on [DISCONNECTING] state */
            case NN_SOFI_STATE_DISCONNECTING:
            switch (action) {
                case NN_SOFI_OUTACTION_TX_ACK:
                case NN_SOFI_OUTACTION_CLOSE:

                    /* Don't notify anything, go straight to shutdown */
                    sofi->outstate = NN_SOFI_OUTSTATE_CLOSED;
                    /* Notify core fsm for the fact that IN-FSM is closed */
                    nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_OUT_CLOSED );
                    return;

                default:
                    nn_fsm_bad_action( sofi->outstate, sofi->state, action );
                }

            default:
                nn_fsm_bad_source( sofi->outstate, sofi->state, action );

        }

/******************************************************************************/
/*  OUTPUT - ERROR state.                                                     */
/******************************************************************************/
        case NN_SOFI_OUTSTATE_ERROR:
        switch (action) {
            case NN_SOFI_OUTACTION_CLOSE:

                /* Go straight to shutdown */
                sofi->outstate = NN_SOFI_OUTSTATE_CLOSED;
                /* Notify core fsm for the fact that IN-FSM is closed */
                nn_fsm_action( &sofi->fsm, NN_SOFI_ACTION_OUT_CLOSED );

                return;

            default:
                nn_fsm_bad_action( sofi->outstate, sofi->state, action );
        }

/******************************************************************************/
/*  OUTPUT - CLOSED state.                                                    */
/******************************************************************************/
        case NN_SOFI_OUTSTATE_CLOSED:
        switch (action) {
            case NN_SOFI_OUTACTION_CLOSE:
                /* Ignore other close events (we are expecting 1 extra) */
                return;

            default:
                nn_fsm_bad_action( sofi->outstate, sofi->state, action );
        }

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


    /* Forward worker events no matter the FSM state */
    if (type == NN_WORKER_TASK_EXECUTE) {
        switch (src) {

            case NN_SOFI_TASK_RX:
                _ofi_debug("OFI: SOFI: Acknowledging rx data event\n");
                nn_sofi_input_action( sofi, NN_SOFI_INACTION_RX_DATA );
                return;

            case NN_SOFI_TASK_TX:
                _ofi_debug("OFI: SOFI: Acknowledging tx data event\n");
                nn_sofi_output_action( sofi, NN_SOFI_OUTACTION_TX_ACK );
                return;

            case NN_SOFI_TASK_ERROR:
                _ofi_debug("OFI: SOFI: Tx/Rx error event\n");
                nn_sofi_disconnect( sofi );
                return;

            case NN_SOFI_TASK_DISCONNECT:
                _ofi_debug("OFI: SOFI: Tx/Rx disconnection event\n");
                nn_sofi_disconnect( sofi );
                return;

        }
    }

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
                // nn_timer_start( &sofi->keepalive_timer, NN_SOFI_KEEPALIVE_TIMEOUT );

                /* Initialize In/Out Sub-FSMs */
                sofi->instate = NN_SOFI_INSTATE_IDLE;
                sofi->outstate = NN_SOFI_OUTSTATE_IDLE;
                nn_sofi_input_action( sofi, NN_SOFI_INACTION_ENTER );
                nn_sofi_output_action( sofi, NN_SOFI_OUTACTION_ENTER );
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

        /* Sub-FSM Actions */
        case NN_FSM_ACTION:
            switch (type) {
            case NN_SOFI_ACTION_IN_ERROR:
            case NN_SOFI_ACTION_IN_CLOSED:
            case NN_SOFI_ACTION_OUT_ERROR:
            case NN_SOFI_ACTION_OUT_CLOSED:

                /* Switch to disconnecting */
                sofi->state = NN_SOFI_STATE_DISCONNECTING;

                /* Close sockets */
                nn_sofi_output_action( sofi, NN_SOFI_OUTACTION_CLOSE );
                nn_sofi_input_action( sofi, NN_SOFI_INACTION_CLOSE );

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

        /* Sub-FSM Actions */
        case NN_FSM_ACTION:
            switch (type) {
            case NN_SOFI_ACTION_IN_CLOSED:
            case NN_SOFI_ACTION_OUT_CLOSED:

                /* Check if we have closed everything */
                if (sofi->instate != NN_SOFI_INSTATE_CLOSED) return;
                if (sofi->outstate != NN_SOFI_OUTSTATE_CLOSED) return;
                _ofi_debug("OFI: Both input and output channels are closed!\n");

                /* Switch to disconnected */
                sofi->state = NN_SOFI_STATE_DISCONNECTED;

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
/*  DISCONNECTED state.                                                       */
/******************************************************************************/

    case NN_SOFI_STATE_DISCONNECTED:
        switch (src) {

        /* Keepalive timer */
        case NN_SOFI_SRC_KEEPALIVE_TIMER:
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
