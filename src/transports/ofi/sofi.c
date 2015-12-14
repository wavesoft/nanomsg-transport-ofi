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

#define NN_SOFI_STATE_IDLE      1
#define NN_SOFI_STATE_CONNECTED 2

#define NN_SOFI_DATA            3000

/*  Possible states of the inbound part of the object. */
#define NN_SOFI_INSTATE_HDR     1
#define NN_SOFI_INSTATE_BODY    2
#define NN_SOFI_INSTATE_HASMSG  3

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

    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_handler, nn_sofi_shutdown,
        src, self, owner);
    self->state = NN_SOFI_STATE_IDLE;

    /* Keep OFI resources */
    self->ofi = ofi;
    self->ep = ep;

    /* Initialize list item */
    nn_list_item_init (&self->item);

    /* Initialize fsm events */ 
    nn_fsm_event_init (&self->connected);
    nn_fsm_event_init (&self->datain);

    /* Initialize buffes */
    self->instate = -1;
    nn_msg_init (&self->inmsg, 0);

    /* Initialize pipe base */
    printf("OFI: SOFI: Replacing pipebase\n");
    nn_pipebase_init (&self->pipebase, &nn_sofi_pipebase_vfptr, epbase);

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
    printf("OFI: SOFI: Initializing MR with tx_size=%i, rx_size=%i\n",
        send_size, recv_size);
    ret = ofi_active_ep_init_mr( self->ofi, self->ep, (unsigned)recv_size, 
        (unsigned)send_size );
    if (ret) {
        /* TODO: Handle error */
        printf("OFI: SOFI: ERROR: Unable to allocate memory region for EP!\n");
        return;
    }

    /* Start FSM */
    nn_fsm_start (&self->fsm);

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
    printf("OFI: SOFI: Shutdown\n");

    /* TODO: Implement */
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

    /*  Serialise the message header. */
    nn_putll (sofi->outhdr, sz_sphdr + sz_body);

    /* TODO: Check maximum length */

    /* Serialize data to the tx buffer */
    memcpy( sofi->ep->tx_buf, &sofi->outhdr, sizeof(sofi->outhdr) );
    memcpy( sofi->ep->tx_buf + sz_outhdr, 
        nn_chunkref_data (&msg->sphdr), sz_sphdr );
    memcpy( sofi->ep->tx_buf + sz_outhdr + sz_sphdr, 
        nn_chunkref_data (&msg->body), sz_body );

    /* Send buffer */
    printf("OFI: SOFI: Send ing data (size=%lu)\n", sz_outhdr+sz_sphdr+sz_body );
    ret = ofi_tx( sofi->ep, sz_outhdr+sz_sphdr+sz_body );
    if (ret) {
        /* TODO: Handle errors */
        printf("OFI: SOFI: Error sending data!\n");
        return -ECONNRESET;
    }

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
        if (ret) {
            printf("OFI: SOFI: Receive Error!\n");
            break;
        }

        /* If exited the connected state, stop thread */
        if (self->state != NN_SOFI_STATE_CONNECTED)
            break;

        /*  Message header was received. Check that message size
            is acceptable by comparing with NN_RCVMAXSIZE;
            if it's too large, drop the connection. */
        size = nn_getll ( self->ep->rx_buf );

        /* Check for invalid sizes */
        if (size > self->ep->rx_size) {
            printf("OFI: SOFI: Discarding incoming packaget due to invalid size"
                    " (len=%lu, max=%lu)\n", size, self->ep->rx_size );
            continue;
        }

        /* Initialize msg with rx chunk */
        nn_msg_term (&self->inmsg);
        nn_msg_init( &self->inmsg, size );

        /* * * * * * * * * * * * * * * * * * * * * * * * * * * */
        /* WARNING : WE ARE BREAKING THE ZERO-COPY PRINCIPLE!  */
        /* * * * * * * * * * * * * * * * * * * * * * * * * * * */

        /* Copy body */
        printf("OFI: SOFI: Received data (len=%lu)\n", size);
        memcpy( nn_chunkref_data (&self->inmsg.body), 
                self->ep->rx_buf + 8, size );

        /* Notify FSM for the fact that we have received data  */
        nn_ctx_enter( self->fsm.ctx );
        self->instate = NN_SOFI_INSTATE_HASMSG;
        nn_fsm_action ( &self->fsm, NN_SOFI_DATA );
        nn_ctx_leave( self->fsm.ctx );

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
    printf("> nn_sofi_handler state=%i, src=%i, type=%i\n", sofi->state, src, 
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

        case NN_FSM_ACTION:
            switch (type) {
            case NN_SOFI_DATA:

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
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (sofi->state, src, type);

    }

}
