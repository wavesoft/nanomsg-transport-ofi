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

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "../../ofi.h"
#include "sofi.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/fast.h"
#include "../../utils/wire.h"
#include "../../core/ep.h"
#include "../../core/sock.h"

/* FSM States */
#define NN_SOFI_STATE_IDLE               1001
#define NN_SOFI_STATE_CONNECTING         1002
#define NN_SOFI_STATE_ACTIVE             1003
#define NN_SOFI_STATE_CLOSING            1004
#define NN_SOFI_STATE_CLOSED             1005

/* FSM OUT State */
#define NN_SOFI_STAGEOUT_STATE_IDLE      0
#define NN_SOFI_STAGEOUT_STATE_STAGED    1

/* FSM OUT State */
#define NN_SOFI_OUT_STATE_IDLE           0
#define NN_SOFI_OUT_STATE_ACTIVE         1

/* FSM IN Flags */
#define NN_SOFI_IN_STATE_IDLE            0
#define NN_SOFI_IN_STATE_ACTIVE          1

/* FSM Handshake State */
#define NN_SOFI_HS_STATE_LOCAL           0
#define NN_SOFI_HS_STATE_FULL            1

/* Socket states */
#define NN_SOFI_SOCKET_STATE_IDLE        0
#define NN_SOFI_SOCKET_STATE_CONNECTING  1
#define NN_SOFI_SOCKET_STATE_CONNECTED   2
#define NN_SOFI_SOCKET_STATE_CLOSING     3
#define NN_SOFI_SOCKET_STATE_DRAINING    4
#define NN_SOFI_SOCKET_STATE_CLOSED      5

/* FSM Sources */
#define NN_SOFI_SRC_ENDPOINT             1101
#define NN_SOFI_SRC_KEEPALIVE_TIMER      1102
#define NN_SOFI_SRC_SHUTDOWN_TIMER       1103

/* Timeout values */
#define NN_SOFI_TIMEOUT_HANDSHAKE        1000
#define NN_SOFI_TIMEOUT_KEEPALIVE_TICK   500
#define NN_SOFI_TIMEOUT_DRAIN            OFI_DRAIN_TIMEOUT
#define NN_SOFI_TIMEOUT_SHUTDOWN         500

/* Ingress Buffer Flags */
#define NN_SOFI_INGRESS_ANCILLARY        1

/* How many ticks to wait before sending
   an keepalive packet to remote end. */
#define NN_SOFI_KEEPALIVE_OUT_TICKS      2

/* How many ticks to wait for any incoming
   message (assumed keepalive) from remote end */
#define NN_SOFI_KEEPALIVE_IN_TICKS       4

/* Memory registration keys */
#define NN_SOFI_MRM_AUX_KEY             0x0000
#define NN_SOFI_MRM_SEND_KEY            0x0001
#define NN_SOFI_MRM_RECV_KEY            0x0800
#define NN_SOFI_MRM_KEY_PAGE_SIZE       0x1000

/* Page size */
#ifdef _SC_PAGESIZE
#define NN_SOFI_PAGE_SIZE 4096
#else
#define NN_SOFI_PAGE_SIZE sysconf(_SC_PAGESIZE)
#endif

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
    /* Set error & stop the FSM */
    self->error = error;
    nn_fsm_stop( &self->fsm );
}

/* ########################################################################## */
/*  Egress Functions                                                          */
/* ########################################################################## */

/**
 * Check if there are no outstanding items on the egress queue
 */
static int nn_sofi_egress_empty( struct nn_sofi * self )
{
    return self->stageout_counter.n == self->egress_max;
}

/**
 * Custom chunk deallocator function that also hints the memory registration 
 * manager for the action.
 */
static void nn_sofi_mr_free( void *p, void *user )
{
    struct nn_sofi_out_ctx * ctx = user;
}

/**
 * Send the contents of the aux buffer
 */
static int nn_sofi_egress_post_aux( struct nn_sofi * self, size_t len )
{
    int ret;
    void * desc[0];
    struct iovec iov [1];
    struct fi_msg msg;

    /* Set desc */
    desc[0] = fi_mr_desc( self->aux_mr );

    /* Prepare msg IOVs */
    iov[0].iov_base = self->aux_buf;
    iov[0].iov_len = len;
    msg.desc = &desc[0];
    msg.msg_iov = iov;
    msg.iov_count = 1;
    msg.context = &self->aux_context;

    /* Send data, without generating a CQ */
    _ofi_debug("OFI[S]: Sending AUX egress (len=%zu)\n", len);
    ret = ofi_sendmsg( self->ep, &msg, 0 );
    if (ret) {
        FT_PRINTERR("ofi_sendmsg", ret);
        return ret;
    }

    /* Success */
    return 0;

}

/**
 * Get a free context that can be used to keep track of an egress packet
 */
static int nn_sofi_egress_get_context( struct nn_sofi * self,
    struct nn_sofi_out_ctx ** ctx )
{

    /* Free items should have mr_handle set to null */
    if (nn_slow( self->egress_ctx_head->mr_handle != NULL )) {
        *ctx = NULL;
        return -EAGAIN;
    }

    /* Get the head */
    *ctx = self->egress_ctx_head;
    _ofi_debug("OFI[S]: get_out_context: Got free ctx=%p\n", *ctx);
    return 0;

}

/**
 * Mark an egress context as busy
 */
static void nn_sofi_egress_mark_busy( struct nn_sofi * self,
    struct nn_sofi_out_ctx * ctx )
{
    nn_assert(nn_fast( ctx == self->egress_ctx_head ));
    nn_assert(nn_fast( ctx->mr_handle != NULL ));

    /* Add item on tail */
    _ofi_debug("OFI[S]: mark_out_busy: Moving ctx=%p to tail\n", ctx);
    ctx->prev = self->egress_ctx_tail;
    self->egress_ctx_tail->next = ctx;
    self->egress_ctx_tail = ctx;

    /* Remove item from head */
    ctx->next->prev = NULL;
    self->egress_ctx_head = ctx->next;
    ctx->next = NULL;
}

/**
 * Mark an egress context as free
 */
static void nn_sofi_egress_mark_free( struct nn_sofi * self,
    struct nn_sofi_out_ctx * ctx )
{
    nn_assert(nn_fast( ctx->mr_handle == NULL ));

    if (ctx == self->egress_ctx_tail) {

        /* Tail Item -> Move to Head */

        /* Remove item from tail */
        _ofi_debug("OFI[S]: mark_out_free: Popping ctx=%p to head\n", ctx);
        self->egress_ctx_tail = ctx->prev;
        self->egress_ctx_tail->next = NULL;
        ctx->prev = NULL;

        /* Add item on head */
        ctx->next = self->egress_ctx_head;
        self->egress_ctx_head->prev = ctx;
        self->egress_ctx_head = ctx;

    } else if (ctx == self->egress_ctx_head) {

        /* Head Item -> (Nothing) */

    } else {

        /* Mid Item -> Move to Head */

        /* Remove item from list */
        _ofi_debug("OFI[S]: mark_out_free: Moving ctx=%p to head\n", ctx);
        ctx->next->prev = ctx->prev;
        ctx->prev->next = ctx->next;

        /* Add item on head */
        ctx->next = self->egress_ctx_head;
        self->egress_ctx_head->prev = ctx;
        self->egress_ctx_head = ctx;
        ctx->prev = NULL;

    }

}

/**
 * Free an egress context
 */
static void nn_sofi_egress_free_context( struct nn_sofi * self,
    struct nn_sofi_out_ctx * ctx )
{
    _ofi_debug("OFI[S]: Freeing context=%p\n", ctx);

    /* Reset message */
    nn_msg_term(&ctx->msg);
    nn_msg_init(&ctx->msg, 0);

    /* Free memory region if we have it's handle */
    if (ctx->mr_handle) {
        _ofi_debug("OFI[S]: Freeing mr handle=%p\n", ctx->mr_handle);
        ofi_mr_release( ctx->mr_handle );
        ctx->mr_handle = NULL;
    }

}

/**
 * Post output buffers (AKA "send data"), and return
 * the number of bytes sent or the error occured.
 */
static int nn_sofi_egress_post_buffers( struct nn_sofi * self, 
    struct nn_msg * outmsg )
{   
    int ret;
    uint8_t hdr[4];
    struct fi_msg msg;
    struct iovec iov [3];
    struct nn_sofi_out_ctx * ctx;

    /* Get a free transit context */
    ret = nn_sofi_egress_get_context( self, &ctx );
    if (ret) {
        FT_PRINTERR("nn_sofi_egress_get_context", ret);
        return ret;
    }

    /* Move message in context */
    nn_msg_mv(&ctx->msg, outmsg);

    /* Prepare header.
       NOTE: You will notice that we are assigning data to be sent
             that are allocated on heap. That's ok, because they
             will be copied to a slab buffer by ofi_mr_describe */
    nn_putl( &hdr[0], nn_chunkref_size (&ctx->msg.sphdr) + 
                      nn_chunkref_size (&ctx->msg.body) );
    iov[0].iov_base = &hdr[0];
    iov[0].iov_len = 4;

    /* Get SP Header length */
    iov[1].iov_len = nn_chunkref_size (&ctx->msg.sphdr);

    /* If SP Header is empty, use only 2 iov */
    if (nn_fast( iov[1].iov_len == 0 )) {

        /* Prepare Body IOVs */
        iov[1].iov_base = nn_chunkref_data (&ctx->msg.body);
        iov[1].iov_len = nn_chunkref_size (&ctx->msg.body);

        /* Prepare message */
        msg.msg_iov = iov;
        msg.iov_count = 2;

        /* TODO: Register a custom free function in the
                 body chunk and call ofi_mr_invalidate when
                 the user frees the chunk. */

        _ofi_debug("OFI[S]: Sending HDR[%zu]+BODY[%zu]\n", 
            iov[0].iov_len, iov[1].iov_len);

    } else {

        /* Prepare SP-Header + Body IOVs */
        iov[1].iov_base = nn_chunkref_data (&ctx->msg.sphdr);
        iov[2].iov_base = nn_chunkref_data (&ctx->msg.body);
        iov[2].iov_len = nn_chunkref_size (&ctx->msg.body);

        /* Prepare message */
        msg.msg_iov = iov;
        msg.iov_count = 3;

        /* TODO: Register a custom free function in the
                 body chunk and call ofi_mr_invalidate when
                 the user frees the chunk. SPHeader is 
                 small enough to be copied in an MR slab */

        _ofi_debug("OFI[S]: Sending HDR[%zu]+SPHDR[%zu]+BODY[%zu]\n", 
            iov[0].iov_len, iov[1].iov_len, iov[2].iov_len);

    }

    /* Keep msg context */
    msg.context = &ctx->context;

    /* Populate MR descriptions through MRM */
    ret = ofi_mr_describe( &self->mrm_egress, &msg, &ctx->mr_handle );
    if (ret) {
        FT_PRINTERR("ofi_mr_describe", ret);
        nn_sofi_egress_free_context( self, ctx );
        return ret;
    }

    /* Send Data and generate CQ upon completed transmission */
    ret = ofi_sendmsg( self->ep, &msg, FI_COMPLETION );
    if (ret) {
        FT_PRINTERR("ofi_sendmsg", ret);
        nn_sofi_egress_free_context( self, ctx );
        return ret;
    }

    /* Mark context as busy */
    nn_sofi_egress_mark_busy( self, ctx );

    /* Return */
    return 0;
}

/**
 * Handle an error egress event (mainly memory releasing)
 */
static void nn_sofi_egress_handle_error( struct nn_sofi * self,
    struct fi_cq_err_entry * cq_entry )
{

    /* Release associated resources */
    struct nn_sofi_out_ctx * ctx = nn_cont( cq_entry->op_context, 
        struct nn_sofi_out_ctx, context );
    nn_sofi_egress_free_context( self, ctx );
    nn_sofi_egress_mark_free( self, ctx );

}

/**
 * Acknowledge the fact that the ougoing data are sent
 */
static void nn_sofi_egress_handle( struct nn_sofi * self,
    struct fi_cq_entry * cq_entry )
{
    int c;

    /* Reset keepalive timer */
    self->ticks_out = 0;

    /* Release associated resources */
    struct nn_sofi_out_ctx * ctx = nn_cont( cq_entry->op_context, 
        struct nn_sofi_out_ctx, context );
    nn_sofi_egress_free_context( self, ctx );
    nn_sofi_egress_mark_free( self, ctx );

    /* Release back-pressure */
    c = nn_atomic_inc( &self->stageout_counter, 1);
    if (c == 0) {
        /* Operation was previously blocked because of back-pressure,
           call `nn_pipebase_sent` to allow further transmission operations. */
        nn_pipebase_sent( &self->pipebase );
    }

}

/**
 * Flush egress queue, discarding all pending messages
 */
static void nn_sofi_egress_flush( struct nn_sofi * self )
{

    /* Release egress contexts */
    struct nn_sofi_out_ctx * ctx = self->egress_ctx_head;
    while (ctx != NULL) {

        /* Free contexts that have an MR handle */
        if (ctx->mr_handle) {

            /* Increment stageout counters */
            nn_atomic_inc( &self->stageout_counter, 1 );

            /* Free context */
            nn_sofi_egress_free_context( self, ctx );

        } else {
            break;
        }
        ctx = ctx->next;
    }

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
    ret = nn_sofi_egress_post_buffers( self, &self->outmsg );
    if (ret) {
        FT_PRINTERR("nn_sofi_egress_post_buffers", ret);
        return ret;
    }

    /* Release staged data */
    self->stageout_state = NN_SOFI_STAGEOUT_STATE_IDLE;

    /* Apply back-pressure */
    if (nn_atomic_dec( &self->stageout_counter, 1) > 1) {

        /* Data are sent, unlock pipebase for next request */
        nn_pipebase_sent( &self->pipebase );

    } else {
        _ofi_debug("OFI[S]: Back-pressure from egress queue\n");
    }

    /* Success */
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
        return 0;

    /* Move the message to the local storage. */
    nn_msg_mv (&self->outmsg, msg);
    self->stageout_state = NN_SOFI_STAGEOUT_STATE_STAGED;

    /* Check if we can send right away */
    if (nn_fast( self->out_state == NN_SOFI_OUT_STATE_ACTIVE )) {
        ret = nn_sofi_egress_send( self );
        if (ret) {
            FT_PRINTERR("nn_sofi_egress_send", ret);
            nn_sofi_critical_error( self, ret );
            return 0;
        }
    }

    /* Success */
    return 0;
}

/* ########################################################################## */
/*  Ingress Functions                                                         */
/* ########################################################################## */

/**
 * Check if there are no outstanding items on the ingress queue
 */
static int nn_sofi_ingress_empty( struct nn_sofi * self )
{
    return nn_queue_empty( &self->ingress_busy );
}

/**
 * Flush ingress queue, discarding all pending messages
 */
static void nn_sofi_ingress_flush( struct nn_sofi * self )
{
    struct nn_sofi_in_buf * buf;

    /* Drain busy */
    while (!nn_queue_empty( &self->ingress_busy ))
        nn_queue_pop( &self->ingress_busy );

}

/**
 * Post a sofi buffer as ingress
 */
static int nn_sofi_ingress_post_buffer( struct nn_sofi * self, 
    struct nn_sofi_in_buf * buf )
{
    int ret;
    void * desc[0];
    struct iovec iov[1];
    struct fi_msg msg;

    /* Prepare message from active ingress buffer */
    memset( &msg, 0, sizeof(msg) );
    iov[0].iov_base = ((uint8_t*)nn_chunk_deref( buf->chunk )) - sizeof(uint32_t);
    iov[0].iov_len = self->ingress_buf_size + sizeof(uint32_t);
    desc[0] = fi_mr_desc( self->ingress_buf_mr );
    msg.desc = &desc[0];
    msg.msg_iov = &iov[0];
    msg.iov_count = 1;
    msg.context = &buf->context;

    _ofi_debug("OFI[S]: Posting ingress buffer=%p (chunk=%p)\n", buf, iov[0].iov_base);

    /* We have a buffer posted */
    nn_atomic_inc( &self->ingress_posted, 1 );

    /* Post receive buffers */
    ret = ofi_recvmsg( self->ep, &msg, 0 );
    if (ret) {

        /* Return error */
        FT_PRINTERR("ofi_recvmsg", ret);
        nn_atomic_dec( &self->ingress_posted, 1 );
        return ret;

    }

    /* Success */
    return 0;
}

/**
 * Post receive buffers
 *
 * This function should pick one of the available pre-allocated message buffers
 * and post them to libfabric. After that, we are expecting a CQ event to 
 * trigger the `sofi_ingress_handle` in order to receive the incoming data.
 */
static int nn_sofi_ingress_post( struct nn_sofi * self )
{
    int ret;
    struct nn_sofi_in_buf * buf;
    struct nn_queue_item * item;

    /* If we have no free items return -EAGAIN */
    if (nn_slow( nn_queue_empty( &self->ingress_free )) ) {
        _ofi_debug("OFI[S]: No free buffers to post\n");
        return -EAGAIN;
    }

    /* Pop free item */
    item = nn_queue_pop( &self->ingress_free );
    buf = nn_cont( item, struct nn_sofi_in_buf, item );

    /* Post the input buffer */
    ret = nn_sofi_ingress_post_buffer( self, buf );
    if (ret < 0) {
        FT_PRINTERR("nn_sofi_ingress_post_buffer", ret);
        return ret;
    }

    /* Success */
    return 0;
}

/**
 * Post all ingress messages
 */
static int nn_sofi_ingress_post_all( struct nn_sofi * self )
{
    int ret;

    /* Post ingress buffers until we ran ount of items */
    while (1) {

        /* Post item */
        ret = nn_sofi_ingress_post( self );

        /* If unavailable, we are done */
        if (ret == -EAGAIN)
            return 0;

        /* Handle errors */
        if (ret) {
            FT_PRINTERR("nn_sofi_ingress_post", ret);
            _ofi_debug("OFI[S]: Failed post next free buffer!\n");
            return ret;
        }

    }

    /* Never reached */
    return 0;
}

/**
 * This function is called when it's a good idea to check
 * if there are any ingress buffer posted, and if not to post one.
 */
static void nn_sofi_ingress_post_eager( struct nn_sofi * self )
{
    /* If we haven't posted anything, post something now */
    if (!nn_queue_empty( &self->ingress_free )) {
        _ofi_debug("OFI[S]: Posting another ingress buffer\n");
        nn_sofi_ingress_post( self );
    }
}

/**
 * This function is called when it's a good idea to check
 * if there are any busy buffers in queue, and if not to notify nanomsg for one.
 */
static void nn_sofi_ingress_busy_eager( struct nn_sofi * self )
{
    /* If there are ingress items, call pipebase_received */
    if ( !nn_queue_empty( &self->ingress_busy ) && 
         (self->ingress_state = NN_SOFI_IN_STATE_IDLE) ) {
        _ofi_debug("OFI[S]: There are pending busy buffers, notifying pipe\n");
        self->ingress_state = NN_SOFI_IN_STATE_ACTIVE;
        nn_pipebase_received( &self->pipebase );
    }
}

/**
 * Handle an ingress error event
 */
static void nn_sofi_ingress_handle_error( struct nn_sofi * self, 
    struct fi_cq_err_entry * cq_entry )
{
    /* Nothing really to do here */
}

/**
 * Process input data
 *
 * Upon completion, this function should return 0 if there are input free
 * buffers available for re-posting or -EAGAIN otherwise. In case an error
 * occurs, this function will return the appropriate error code.
 */
static void nn_sofi_ingress_handle( struct nn_sofi * self, 
    struct fi_cq_entry * cq_entry )
{
    struct nn_sofi_in_buf * buf;
    uint32_t len;

    /* Reset keepalive timer */
    self->ticks_in = 0;

    /* Get the pointer to the ingress buffer */
    buf = nn_cont( cq_entry->op_context, struct nn_sofi_in_buf, context );

    /* Get message length */
    len = nn_getl( (uint8_t*) (((uint32_t*) buf->chunk) - 1) );

    /* We have a limit of <4GB on the message, so we reserved the max for 
       keepalive packets */
    if (len == 0xFFFFFFFF) {

        /* Mark buffer as free */
        _ofi_debug("OFI[S]: Received KEEPALIVE\n");
        nn_queue_push( &self->ingress_free, &buf->item );

        /* Eager post */
        nn_sofi_ingress_post_eager(self);

        /* No need to continue */
        return;

    }

    /* Recover damaged chunk tag */
    nn_putl( (uint8_t*) (((uint32_t*) buf->chunk) - 1), 0xdeadcafe );

    /* Prepare message from the active staged buffer */
    _ofi_debug("OFI[S]: Received BODY[%u]\n", len);
    nn_chunk_reset( buf->chunk, len );
    nn_msg_init_chunk( &buf->msg, buf->chunk );

    /* Stage for pickup  */
    nn_queue_push( &self->ingress_busy, &buf->item );
    nn_sofi_ingress_busy_eager( self );

}

/**
 * Pop a message from the ingress queue
 */
static int nn_sofi_ingress_fetch( struct nn_sofi * self,
    struct nn_msg * msg )
{
    int ret;
    struct nn_queue_item * item;
    struct nn_sofi_in_buf * buf;

    /* This should not be called on empty queue */
    nn_assert(nn_slow( !nn_queue_empty(&self->ingress_busy) ));

    /* Pop busy item */
    item = nn_queue_pop( &self->ingress_busy );
    buf = nn_cont( item, struct nn_sofi_in_buf, item );

    /* Move message to output */
    _ofi_debug("OFI[S]: Passing to nanomsg ingress buffer=%p\n", buf);
    nn_msg_mv( msg, &buf->msg );

    /* We are eager to received another one if available */
    self->ingress_state = NN_SOFI_IN_STATE_IDLE;
    nn_sofi_ingress_busy_eager( self );

    /* Return success */
    return 0;
}

/**
 * Chunk termination function that is called when the user frees
 * a chunk pointer.
 */
static void nn_sofi_ingress_freefn( void * chunk, void * user )
{
    struct nn_sofi *self = (struct nn_sofi *) user;
    struct nn_sofi_in_buf *buf = (struct nn_sofi_in_buf *)
        ( (uint8_t*)chunk + nn_chunk_hdrsize() - NN_SOFI_PAGE_SIZE - sizeof(uint32_t) );

    _ofi_debug("OFI[S]: User released buf=%p\n", buf);

    /* Re-initialize terminated chunk */
    nn_chunk_init( chunk, self->ingress_buf_size + nn_chunk_hdrsize(),
                   nn_sofi_ingress_freefn, self, &buf->chunk );

    /* Mark buffer as free */
    nn_queue_push( &self->ingress_free, &buf->item );
    nn_sofi_ingress_post_eager( self );

}

/* ########################################################################## */
/*  Implementation  Functions                                                 */
/* ########################################################################## */

/*  Initialize the state machine */
int nn_sofi_init ( struct nn_sofi *self, struct ofi_domain *domain, int offset,
    struct nn_epbase *epbase, int src, struct nn_fsm *owner )
{
    const uint64_t mr_flags = FI_RECV | FI_READ | FI_REMOTE_WRITE;
    uint64_t mr_page_offset = NN_SOFI_MRM_KEY_PAGE_SIZE * offset;
    int ret, i;

    /* Initialize properties */
    self->domain = domain;
    self->ep = NULL;
    self->offset = offset;
    self->epbase = epbase;
    self->stageout_state = NN_SOFI_STAGEOUT_STATE_IDLE;
    self->out_state = NN_SOFI_OUT_STATE_IDLE;

    /* Initialize local handshake */
    self->hs_local.version = 1;

#ifndef OFI_DISABLE_HANDSHAKE
    self->hs_state = NN_SOFI_HS_STATE_LOCAL;
#endif

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
    self->socket_state = NN_SOFI_STATE_IDLE;
    self->error = 0;

    /* ----------------------------------- */
    /*  NanoMsg Component Initialization   */
    /* ----------------------------------- */

    /* Initialize timers */
    nn_timer_init(&self->timer_keepalive, NN_SOFI_SRC_KEEPALIVE_TIMER,
        &self->fsm);
    nn_timer_init(&self->timer_shutdown, NN_SOFI_SRC_SHUTDOWN_TIMER,
        &self->fsm);

    /* Reset properties */
    self->ticks_in = 0;
    self->ticks_out = 0;

    /* Outgoing message */
    nn_msg_init (&self->outmsg, 0);

    /* Initialize queues */
    nn_queue_init( &self->ingress_free );
    nn_queue_init( &self->ingress_busy );

    /* ----------------------------------- */
    /*  OFI Sub-Component Initialization   */
    /* ----------------------------------- */

    /* Get an OFI worker */
    self->worker = ofi_fabric_getworker( domain->parent, &self->fsm );

    /* Get options */
    int rx_queue, tx_queue, rx_msg_size, slab_size;
    size_t opt_sz = sizeof(int);
    nn_epbase_getopt (epbase, NN_OFI, NN_OFI_TX_QUEUE_SIZE, &tx_queue, &opt_sz);
    nn_epbase_getopt (epbase, NN_OFI, NN_OFI_RX_QUEUE_SIZE, &rx_queue, &opt_sz);
    nn_epbase_getopt (epbase, NN_OFI, NN_OFI_SLAB_SIZE, &slab_size, &opt_sz);
    nn_epbase_getopt (epbase, NN_SOL_SOCKET, NN_RCVBUF, &rx_msg_size, &opt_sz);

    /* Put default values if set to AUTO */
    if (tx_queue == 0) tx_queue = domain->fi->tx_attr->size;
    if (rx_queue == 0) rx_queue = domain->fi->rx_attr->size;

    /* Wrap overflown values */ 
    if (tx_queue > domain->fi->tx_attr->size) tx_queue = domain->fi->tx_attr->size;
    if (rx_queue > domain->fi->rx_attr->size) rx_queue = domain->fi->rx_attr->size;

    /* Debug print current values */
    _ofi_debug("OFI[S]: Options: Tx-Queue-Size: %i"
                    ", Rx-Queue-Size: %i, Offset: %i\n", 
                    rx_queue, tx_queue, offset);
    _ofi_debug("OFI[S]:          Max-Recv-Size: %i b, Slab-Size: %i b\n",
                    rx_msg_size, slab_size);

    /* ####[ ANCILLARY ]#### */

    /* Register ancillary data */
    ret = fi_mr_reg(self->domain->domain, self->aux_buf, NN_SOFI_ANCILLARY_SIZE, 
        FI_RECV| FI_READ| FI_REMOTE_WRITE| FI_SEND| FI_WRITE| FI_REMOTE_READ, 
        0, mr_page_offset+NN_SOFI_MRM_AUX_KEY, 0, &self->aux_mr, NULL);
    if (ret) {
        FT_PRINTERR("fi_mr_reg", ret);
    }

    /* Currently AUX buffer is only used for keepalive message,
       so for optimisation reasons, write it's contents once now */
    memcpy( self->aux_buf, NN_SOFI_KEEPALIVE_PACKET, 
        NN_SOFI_KEEPALIVE_PACKET_LEN );

    /* ####[ EGRESS ]#### */

    /* Initialize egress MR Manager with 32 banks */
    struct ofi_mr_bank_attr mrattr_tx = {
         /* Worst case Tx scenario : Body bank + SP Header bank */
        .bank_count = tx_queue * 2,
        .domain = self->domain,
        .direction = OFI_MR_DIR_SEND,
        .slab_count = tx_queue,
        .slab_size = slab_size,
        .base_key = mr_page_offset+NN_SOFI_MRM_SEND_KEY,
    };
    ofi_mr_init( &self->mrm_egress, &mrattr_tx );

    /* Initialize throttle counters */
    nn_atomic_init( &self->stageout_counter, tx_queue );
    self->egress_max = tx_queue;

    /* A list of egress chunks */
    self->egress_contexts = nn_alloc( sizeof(struct nn_sofi_out_ctx ) * tx_queue,
        "egress sofi buffers");
    nn_assert( self->egress_contexts );
    memset( self->egress_contexts, 0, sizeof(struct nn_sofi_out_ctx) * tx_queue);

    /* Initilaize buffers */
    for (i=0; i<rx_queue; ++i) {

        /* Initialize properties */
        self->egress_contexts[i].sofi = self;
        nn_msg_init( &self->egress_contexts[i].msg, 0);

        /* Setup linked list */
        if (i > 0) {
            self->egress_contexts[i-1].next = &self->egress_contexts[i];
        }
        if (i < rx_queue-1) {
            self->egress_contexts[i+1].prev = &self->egress_contexts[i];
        }

    }

    /* Setup ring */
    self->egress_ctx_head = &self->egress_contexts[0];
    self->egress_ctx_tail = &self->egress_contexts[rx_queue-1];

    /* ####[ INGRESS ]#### */

    /* Initialize atomic counters */
    nn_atomic_init( &self->ingress_posted, 0 );
    self->ingress_state = NN_SOFI_IN_STATE_IDLE;

    /* Wrap buffer size to page-size multiplicants */
    self->ingress_buf_size = (1 + ((rx_msg_size - 1) / NN_SOFI_PAGE_SIZE)) 
                                * NN_SOFI_PAGE_SIZE;

    /* Make sure it fits the prefix header */
    if ((self->ingress_buf_size - self->rx_msg_size) < sizeof(uint32_t) ) {
        self->ingress_buf_size += NN_SOFI_PAGE_SIZE;
    }
    _ofi_debug("OFI[S]:          Effective-Recv-Size: %i b\n", 
        self->ingress_buf_size);

    /* Claculate the size of the buffer */
    size_t ibufsz = self->ingress_buf_size + NN_SOFI_PAGE_SIZE;

    /* Allocate ingress buffers */
#if _POSIX_C_SOURCE >= 200112L
    self->ingress_buffers = NULL;
    ret = posix_memalign( (void**)&self->ingress_buffers, sysconf(_SC_PAGESIZE), 
        ibufsz * rx_queue );
    _ofi_debug("OFI[S]: Allocated page-aligned egress aux sz=%zu, at=%p\n",
        ibufsz * rx_queue, self->ingress_buffers);
#else
    self->ingress_buffers = nn_alloc( ibufsz * rx_queue, "ingress sofi buffer");
    _ofi_debug("OFI[S]: Allocated system-aligned egress aux sz=%zu, at=%p\n",
        ibufsz * rx_queue, self->ingress_buffers);
#endif

    /* Ensure correct allocation and initialize */
    nn_assert( self->ingress_buffers );
    memset( self->ingress_buffers, 0, sizeof(struct nn_sofi_in_buf) * rx_queue);

    /* Register block of memory */
    ret = fi_mr_reg(self->domain->domain, self->ingress_buffers, 
        ibufsz * rx_queue, mr_flags, 0, mr_page_offset+NN_SOFI_MRM_RECV_KEY, 0, 
        &self->ingress_buf_mr, NULL);
    if (ret) {
        FT_PRINTERR("fi_mr_reg", ret);
    }

    /* Initilaize buffers */
    struct nn_sofi_in_buf *ibuf = self->ingress_buffers;
    for (i=0; i<rx_queue; ++i) {

        /* Initialize chunk in such a way that it's page-aligned to
           the tag. (The tag will be replaced when receiving data) */
        ret = nn_chunk_init( (uint8_t*)ibuf + NN_SOFI_PAGE_SIZE - 
                                nn_chunk_hdrsize() + sizeof(uint32_t),
                            self->ingress_buf_size + nn_chunk_hdrsize(),
                            nn_sofi_ingress_freefn, self,
                            &ibuf->chunk );
        if (ret) {
            FT_PRINTERR("nn_chunk_alloc", ret);
            return ret;
        }

        /* Init inbuf properties */
        nn_msg_init( &ibuf->msg, 0 );
        nn_queue_item_init( &ibuf->item );

        /* Keep free items on queue */
        nn_queue_push( &self->ingress_free, &ibuf->item );

        /* Forward */
        ibuf = (struct nn_sofi_in_buf *)((uint8_t*)ibuf + ibufsz);

    }

    /* Populate ingress properties */
    self->ingress_max = rx_queue;

    /* Success */
    return 0;

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

    /* Receive remote handshake information */
#ifndef OFI_DISABLE_HANDSHAKE
    memcpy( &self->hs_remote, conreq->data, sizeof(self->hs_remote) );
    _ofi_debug("OFI[S]: Handshake with remote version=%i\n",
        self->hs_remote.version);
    self->hs_state = NN_SOFI_HS_STATE_FULL;
#endif

    /* Accept incoming connection and send our side of the handshake */
    self->socket_state = NN_SOFI_STATE_CONNECTING;
#ifdef OFI_DISABLE_HANDSHAKE
    ret = ofi_cm_accept( self->ep, NULL, 0 );
#else
    ret = ofi_cm_accept( self->ep, &self->hs_local, sizeof(self->hs_local) );
#endif
    if (ret) {
        FT_PRINTERR("ofi_cm_accept", ret);
        self->socket_state = NN_SOFI_SOCKET_STATE_CLOSED;
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
    self->socket_state = NN_SOFI_STATE_CONNECTING;
#ifdef OFI_DISABLE_HANDSHAKE
    ret = ofi_cm_connect( self->ep, NULL, NULL, 0 ); 
#else
    ret = ofi_cm_connect( self->ep, NULL, &self->hs_local, 
        sizeof(self->hs_local) ); 
#endif
    if (ret) {
        FT_PRINTERR("ofi_cm_connect", ret);
        self->socket_state = NN_SOFI_SOCKET_STATE_CLOSED;
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
    struct nn_sofi_out_ctx *ctx;
    struct nn_list_item *it;
    int i, ret;
    _ofi_debug("OFI[S]: Cleaning-up SOFI\n");

    /* ----------------------------------- */
    /*  OFI Sub-Component Termination      */
    /* ----------------------------------- */

    /* Terminate worker */
    nn_ofiw_term( self->worker );

    /* Terminate MR manager */
    ofi_mr_term( &self->mrm_egress );

    /* Free ingress buffers */
    nn_free( self->ingress_buffers );

    /* Unregister ingress MR */
    ret = fi_close(&self->ingress_buf_mr->fid);
    if (ret) {
        FT_PRINTERR("fi_mr_reg", ret);
    }

    /* Unregister ancillary MR */
    ret = fi_close(&self->aux_mr->fid);
    if (ret) {
        FT_PRINTERR("fi_mr_reg", ret);
    }

    /* ----------------------------------- */
    /*  NanoMsg Component Termination      */
    /* ----------------------------------- */

    /* Stop timers */
    nn_timer_term (&self->timer_keepalive);
    nn_timer_term (&self->timer_shutdown);

    /* Stop throttles */
    nn_atomic_term( &self->stageout_counter );
    nn_atomic_term( &self->ingress_posted );

    /* Release all transit contexts in book-keeping */
    ctx = self->egress_ctx_head;
    while (ctx != NULL) {
        nn_msg_term( &ctx->msg);
        ctx = ctx->next;
    }
    nn_free( self->egress_contexts );

    /* Terminate queues */
    nn_queue_term( &self->ingress_free );
    nn_queue_term( &self->ingress_busy );

    /* ----------------------------------- */
    /*  NanoMSG Core Termination           */
    /* ----------------------------------- */

    /* Terminate list item */
    nn_list_item_term (&self->item);

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

    /* Fetch a message from the ingress queue */
    _ofi_debug("OFI[S]: NanoMsg RECV event\n");
    return nn_sofi_ingress_fetch( self, msg );
}

/**
 * SHUTDOWN State Handler
 */
static void nn_sofi_shutdown (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{
    struct nn_sofi *self;
    struct fi_cq_entry * cq_entry;
    struct fi_cq_err_entry * cq_error;

    /* Get pointer to sofi structure */
    self = nn_cont (fsm, struct nn_sofi, fsm);

    /* If this is part of the FSM action, start shutdown */
    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {
        _ofi_debug("OFI[S]: We are now closing\n");

        /* Enter closing state */
        self->state = NN_SOFI_STATE_CLOSING;

        /* Stop keepalive timer */
        nn_timer_stop( &self->timer_keepalive );

        /* Flush ingress queue */
        _ofi_debug("OFI[S]: Discarding pending ingress buffers\n");
        nn_sofi_ingress_flush( self );

        /* If we are not connected, or if we are closing due to an 
           error, discard all staged egress messages */
        if ((self->state == NN_SOFI_STATE_CONNECTING) || (self->error != 0)) {
            _ofi_debug("OFI[S]: Discarding pending egress buffers\n");
            nn_sofi_egress_flush( self );
        }

    } else if (nn_slow(src == (NN_SOFI_SRC_ENDPOINT | OFI_SRC_CQ_TX) )) {

        /* Handle Tx Events. Other Tx events won't be accepted because our
           state is now in NN_SOFI_STATE_CLOSING */

        /* Wait for successful egress completion only on clean shutdown */
        _ofi_debug("OFI[S]: Draining egress CQ event\n");
        if (type == NN_OFIW_COMPLETED) {
            /* Handle completed event */
            cq_entry = (struct fi_cq_entry *) srcptr;
            nn_sofi_egress_handle( self, cq_entry );
        } else if (type == NN_OFIW_ERROR) {
            /* Handle error event */
            cq_error = (struct fi_cq_err_entry *) srcptr;
            nn_sofi_egress_handle_error( self, cq_error );
        } else {
            nn_fsm_bad_action (self->state, src, type);
        }

    } else if (nn_slow(src == (NN_SOFI_SRC_ENDPOINT | OFI_SRC_CQ_RX) )) {

        /* Handle Rx Events, but don't post new input buffers */
        _ofi_debug("OFI[S]: Discarding ingress CQ event\n");
        if ((type != NN_OFIW_COMPLETED) && (type != NN_OFIW_ERROR)) {
            nn_fsm_bad_action (self->state, src, type);
        }

    } else if (nn_slow(src == (NN_SOFI_SRC_ENDPOINT | OFI_SRC_EQ) )) {

        /* Handle endpoint events */
        _ofi_debug("OFI[S]: Draining endpoint EQ event\n");
        if (type == FI_SHUTDOWN) {

            /* We can now safely close the socket */
            _ofi_debug("OFI[S]: Socket is closed\n");
            self->socket_state = NN_SOFI_SOCKET_STATE_CLOSED;
            nn_timer_stop( &self->timer_shutdown );

        } else {
            /* We also accept error */
            if (type > 0) nn_fsm_bad_action (self->state, src, type);
        }

    } else if (nn_slow(src == NN_SOFI_SRC_KEEPALIVE_TIMER)) {

        /* Wait for timer to stop */
        if (nn_slow( type != NN_TIMER_STOPPED ))
            nn_fsm_bad_action (self->state, src, type);

        _ofi_debug("OFI[S]: Keepalive timer stopped\n");

    } else if (nn_slow(src == NN_SOFI_SRC_SHUTDOWN_TIMER)) {

        /* Wait for timer to stop */
        if (nn_fast( type == NN_TIMER_TIMEOUT )) {
            if (self->socket_state == NN_SOFI_SOCKET_STATE_CLOSING) {
                /* Stop timer (handled at stop event) */
                nn_timer_stop( &self->timer_shutdown );
#if OFI_DRAIN_TIMEOUT > 0

            } else if (self->socket_state == NN_SOFI_SOCKET_STATE_DRAINING) {

                /* Stop timer (it will be restarted at stop event) */
                nn_timer_stop( &self->timer_shutdown );
#endif
            }
            return;

        } else if (nn_fast( type == NN_TIMER_STOPPED )) {

#if OFI_DRAIN_TIMEOUT > 0
            if (nn_fast( self->socket_state == NN_SOFI_SOCKET_STATE_DRAINING)) {
                /* Shutdown connection & close endpoint */
                _ofi_debug("OFI[S]: Stopping endpoint\n");
                self->socket_state = NN_SOFI_SOCKET_STATE_CLOSING;
                ofi_cm_shutdown( self->ep );

                /* Restart timer to track shutdown timeouts */
                nn_timer_start( &self->timer_shutdown, 
                    NN_SOFI_TIMEOUT_SHUTDOWN );
                return;

            } else
#endif
            if (nn_slow( self->socket_state == NN_SOFI_SOCKET_STATE_CLOSING)) {
                /* Timed out waiting for shutdown */
                _ofi_debug("OFI[S]: Timed out waiting for shutdown EQ event\n");
                self->socket_state = NN_SOFI_SOCKET_STATE_CLOSED;

            }else if(nn_slow(self->socket_state!=NN_SOFI_SOCKET_STATE_CLOSED)) {
                nn_fsm_bad_state (self->state, src, type);
            }

        } else {
            nn_fsm_bad_action (self->state, src, type);
        }

    } else {
        nn_fsm_bad_source (self->state, src, type);
    }


    /* Wait for all outstanding transmissions or receptions to complete
       and for all resources to be stopped */
    if (!nn_sofi_egress_empty( self ) || 
        !nn_sofi_ingress_empty( self ) ||
        !nn_timer_isidle( &self->timer_keepalive ) ||
        !nn_timer_isidle( &self->timer_shutdown) ) {
        _ofi_debug("OFI[S]: Delaying close: egress=%i, ingress=%i, isidle=%i"
            ",%i, egress_left=%i\n",
            nn_sofi_egress_empty( self ), nn_sofi_ingress_empty( self ),
            nn_timer_isidle( &self->timer_keepalive ),
            nn_timer_isidle( &self->timer_shutdown ),
            self->egress_max - self->stageout_counter.n);
        return;
    }

    if (self->socket_state == NN_SOFI_SOCKET_STATE_CONNECTED) {

#if OFI_DRAIN_TIMEOUT > 0
        /* Start draining socket */
        _ofi_debug("OFI[S]: Draining pending endpoint events (%i us)\n", 
            NN_SOFI_TIMEOUT_DRAIN);
        self->socket_state = NN_SOFI_SOCKET_STATE_DRAINING;
        nn_timer_start( &self->timer_shutdown, 
            NN_SOFI_TIMEOUT_DRAIN );
#else
        /* Shutdown connection & close endpoint */
        _ofi_debug("OFI[S]: Stopping endpoint\n");
        self->socket_state = NN_SOFI_SOCKET_STATE_CLOSING;
        ofi_cm_shutdown( self->ep );

        /* Restart timer to track shutdown timeouts */
        nn_timer_start( &self->timer_shutdown, 
            NN_SOFI_TIMEOUT_SHUTDOWN );
#endif
        return;

    } else if (self->socket_state == NN_SOFI_STATE_CONNECTING) {

        /* The socket never managed to connect */
        _ofi_debug("OFI[S]: Socket never connected\n");
        self->socket_state = NN_SOFI_SOCKET_STATE_CLOSED;

    } else if (self->socket_state != NN_SOFI_SOCKET_STATE_CLOSED) {

        _ofi_debug("OFI[S]: Unexpected socket_state=%i\n",
            self->socket_state);
        nn_fsm_bad_source (self->state, src, type);

    }

    /* Stop worker */
    nn_ofiw_stop( self->worker );

    /* Close endpoint */
    _ofi_debug("OFI[S]: Closing endpoint\n");
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
    struct fi_eq_cm_entry * cq_cm_entry;
    struct fi_cq_entry * cq_entry;
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
                _ofi_debug("OFI[S]: FSM started, waiting for connection\n");
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

                /* Receive remote handshake information */
                cq_cm_entry = (struct fi_eq_cm_entry *) srcptr;
#ifndef OFI_DISABLE_HANDSHAKE
                if ( self->hs_state == NN_SOFI_HS_STATE_LOCAL ) {
                    memcpy( &self->hs_remote, cq_cm_entry->data, 
                        sizeof(self->hs_remote) );
                    _ofi_debug("OFI[S]: Handshake with remote version=%i\n",
                        self->hs_remote.version);
                }
#endif

                /* The connection is established, start pipe */
                _ofi_debug("OFI[S]: Endpoint connected, starting pipebase\n");
                self->state = NN_SOFI_STATE_ACTIVE;
                self->socket_state = NN_SOFI_SOCKET_STATE_CONNECTED;
                self->out_state = NN_SOFI_OUT_STATE_ACTIVE;
                nn_pipebase_start( &self->pipebase );

                /* Start keepalive timer */
                _ofi_debug("OFI[S]: Starting keepalive timer\n");
                nn_timer_start( &self->timer_keepalive, 
                    NN_SOFI_TIMEOUT_KEEPALIVE_TICK );

                /* Post first ingress buffer */
                ret = nn_sofi_ingress_post_all( self );
                if (ret) {
                    FT_PRINTERR("nn_sofi_ingress_post_all", ret);
                    nn_sofi_critical_error( self, ret );
                    return;
                }

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
            
                /* EQ Error */
                if (type < 0) {

                    /* Unrecoverable comm error */
                    nn_sofi_critical_error( self, type );
                    return;

                }

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

                _ofi_debug("OFI[S]: Remote endpoint disconnected!\n");
                self->socket_state = NN_SOFI_SOCKET_STATE_CLOSED;

                /* The connection is dropped from the remote end.
                   This is an unrecoverable error and we should terminate */
                nn_sofi_critical_error( self, -EINTR );
                return;

            default:

                /* EQ Error */
                if (type < 0) {

                    /* Unrecoverable comm error */
                    nn_sofi_critical_error( self, type );
                    return;

                }

                nn_fsm_bad_action (self->state, src, type);
            }

        /* ========================= */
        /*  Endpoint RX CQ Event     */
        /* ========================= */
        case NN_SOFI_SRC_ENDPOINT | OFI_SRC_CQ_RX:
            switch (type) {
            case NN_OFIW_COMPLETED:

                /* Get CQ Event */
                cq_entry = (struct fi_cq_entry *) srcptr;

                /* Process incoming data */
                nn_sofi_ingress_handle( self, cq_entry );
                return;

            case NN_OFIW_ERROR:

                /* Get CQ Error */
                cq_error = (struct fi_cq_err_entry *) srcptr;

                /* Unrecoverable error while receiving data */
                nn_sofi_ingress_handle_error( self, cq_error );
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
                cq_entry = (struct fi_cq_entry *) srcptr;

                /* Data from the output buffer are sent */
                nn_sofi_egress_handle( self, cq_entry );

                return;

            case NN_OFIW_ERROR:

                /* Get CQ Error */
                cq_error = (struct fi_cq_err_entry *) srcptr;

                /* Unrecoverable error while sending data */
                nn_sofi_egress_handle_error( self, cq_error );
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

                /* Check if connection timed out */
                if (++self->ticks_in > NN_SOFI_KEEPALIVE_IN_TICKS) {
                    _ofi_debug("OFI[S]: Keepalive expired, dropping"
                        " connection\n");
                    
                    /* Reset and stop timer */
                    self->ticks_in = 0;
                    nn_timer_stop( &self->timer_keepalive );

                    /* Drop connection through a critical error */
                    nn_sofi_critical_error( self, -ETIMEDOUT );
                    return;
                }

                /* Check if we should send a keepalive packet */
                if (++self->ticks_out > NN_SOFI_KEEPALIVE_OUT_TICKS) {
                    self->ticks_out = 0;

                    /* Send aux packet */
                    _ofi_debug("OFI[S]: Sending KEEPALIVE\n");
                    ret = nn_sofi_egress_post_aux( self, 
                        NN_SOFI_KEEPALIVE_PACKET_LEN );
                    if (ret) {
                        FT_PRINTERR("nn_sofi_egress_post_aux", ret);
                        nn_sofi_critical_error( self, ret );
                        return;
                    }

                }

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
