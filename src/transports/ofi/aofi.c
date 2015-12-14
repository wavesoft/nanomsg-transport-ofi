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
#include "aofi.h"

#include "../../utils/cont.h"
#include "../../utils/err.h"
#include "../../utils/alloc.h"

#define NN_AOFI_STATE_IDLE       1
#define NN_AOFI_STATE_ACCEPTING  2
#define NN_AOFI_STATE_ACCEPTED   3

/*  Private functions. */
static void nn_aofi_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_aofi_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr);

// static void nn_sofi_accept_thread (void *arg);

/**
 * Initialize AOFI FSM
 */
void nn_aofi_init (struct nn_aofi *self, 
    struct ofi_resources *ofi,struct ofi_passive_endpoint *pep, 
    struct nn_epbase *epbase, int src, struct nn_fsm *owner)
{
    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_aofi_handler, nn_aofi_shutdown,
        src, self, owner);
    self->state = NN_AOFI_STATE_IDLE;

    /* Keep references */
    self->ofi = ofi;
    self->pep = pep;
    self->epbase = epbase;

    /* Initialize fsm events */ 
    nn_fsm_event_init (&self->accepted);

    /* Initialize list item */
    nn_list_item_init (&self->item);
}

/**
 * Start AOFI FSM
 */
void nn_aofi_start (struct nn_aofi *self )
{
    nn_assert_state (self, NN_AOFI_STATE_IDLE);

    /* Start SOFI */
    printf("OFI: AOFI: Starting FSM\n");
    nn_fsm_start(&self->fsm);
    printf("OFI: AOFI: FSM Started!\n");

}

// /**
//  * The internal thread that keeps blocking until we have an accept operation
//  */
// static void nn_sofi_poller_thread (void *arg)
// {
//     ssize_t ret;
//     struct nn_sofi * self = (struct nn_sofi *) arg;

//     /* Infinite loop */
//     while (1) {

//         /* Listen for incoming connections */
//         ret = ofi_server_accept( aofi->ofi, aofi->pep, &aofi->ep );
//         if (ret < 0) {
//             printf("OFI: AOFI: ERROR: Cannot accept!\n");
//             /* TODO: What do I do? */
//             return;
//         }

//         printf("OFI: SOFI: Received data: '%s'\n", self->ep->rx_buf);
//         nn_ctx_enter( self->fsm.ctx );
//         nn_fsm_raise ( &self->fsm, &self->datain, NN_SOFI_DATA );
//         nn_ctx_leave( self->fsm.ctx );
//         // nn_fsm_action( &self->fsm, NN_SOFI_DATA );

//     }
// }

static void nn_aofi_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    printf("OFI: AOFI: Shutdown\n");

    /* Get reference to the aofi structure */
    struct nn_aofi *aofi;
    aofi = nn_cont(self, struct nn_aofi, fsm);

    /* Free structures */
    nn_list_item_term (&aofi->item);
    nn_free (aofi);

}

static void nn_aofi_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    int ret;
    struct nn_aofi *aofi;
    aofi = nn_cont (self, struct nn_aofi, fsm);
    printf("> nn_aofi_handler state=%i, src=%i, type=%i\n", aofi->state, src, type);

    switch (aofi->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_AOFI_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
            

                /* Start SOFI to handle the connection from now on */
                nn_sofi_init(&aofi->sofi, aofi->ofi, &aofi->ep, aofi->epbase, 
                    NN_BTCP_SRC_SOFI, &aofi->fsm );

                printf("OFI: AOFI: Accepted\n");
                nn_fsm_raise (&aofi->fsm, &aofi->accepted, NN_AOFI_ACCEPTED);

                return;
            default:
                nn_fsm_bad_action (aofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (aofi->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (aofi->state, src, type);

    }

}
