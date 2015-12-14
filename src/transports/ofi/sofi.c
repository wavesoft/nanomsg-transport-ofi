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


#define NN_SOFI_STATE_IDLE      1
#define NN_SOFI_STATE_CONNECTED 2

#define NN_SOFI_DATA            3000

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
    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_handler, nn_sofi_shutdown,
        src, self, owner);
    self->state = NN_SOFI_STATE_IDLE;

    /* Keep OFI resources */
    self->ofi = ofi;
    self->ep = ep;

    /* Initialize fsm events */ 
    nn_fsm_event_init (&self->connected);
    nn_fsm_event_init (&self->datain);

    /* Initialize pipe base */
    printf("OFI: SOFI: Replacing pipebase\n");
    nn_pipebase_init (&self->pipebase, &nn_sofi_pipebase_vfptr, epbase);

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

    // Nothing now
}


static int nn_sofi_send (struct nn_pipebase *self, struct nn_msg *msg)
{
    printf("OFI: SOFI: Send\n");
    struct nn_sofi *sofi;
    sofi = nn_cont (self, struct nn_sofi, pipebase);

    return -ECONNRESET;
}

static int nn_sofi_recv (struct nn_pipebase *self, struct nn_msg *msg)
{
    printf("OFI: SOFI: Receive\n");
    int rc;
    struct nn_sofi *sofi;
    sofi = nn_cont (self, struct nn_sofi, pipebase);
    return -ECONNRESET;
}

/**
 * The internal poller thread, since OFI does not have blocking UNIX file descriptors
 */
static void nn_sofi_poller_thread (void *arg)
{
    ssize_t ret;
    struct nn_sofi * self = (struct nn_sofi *) arg;

    /* Infinite loop */
    while (1) {

        /* Receive data from OFI */
        ret = ofi_rx( self->ep, MAX_MSG_SIZE );
        if (ret) {
            printf("OFI: SOFI: Receive Error!\n");
            break;
        }

        printf("OFI: SOFI: Received data: '%s'\n", self->ep->rx_buf);
        nn_assert_state (self, NN_SOFI_STATE_CONNECTED);
        nn_ctx_enter( self->fsm.ctx );
        nn_fsm_raise ( &self->fsm, &self->datain, NN_SOFI_DATA );
        nn_ctx_leave( self->fsm.ctx );
        // nn_fsm_action( &self->fsm, NN_SOFI_DATA );

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
    printf("> nn_sofi_handler state=%i, src=%i, type=%i\n", sofi->state, src, type);

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
                sofi->state = NN_SOFI_STATE_CONNECTED;

                /* Start poller thread */
                nn_thread_init (&sofi->thread, nn_sofi_poller_thread, sofi);

                /* We are started */
                printf("OFI: SOFI: Started\n");
                // nn_fsm_raise (self, &sofi->connected, NN_SOFI_CONNECTED);

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

                /* We received some incoming data */
                printf("OFI: SOFI: Got incoming data\n");

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
