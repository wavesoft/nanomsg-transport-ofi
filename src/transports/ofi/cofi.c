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
#include "cofi.h"
#include "sofi.h"
#include "hlapi.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"

#define NN_COFI_STATE_IDLE              1
#define NN_COFI_STATE_CONNECTED         2

#define NN_COFI_SRC_SOFI                1

/* nn_epbase virtual interface implementation. */
static void nn_cofi_stop (struct nn_epbase *self);
static void nn_cofi_destroy (struct nn_epbase *self);
const struct nn_epbase_vfptr nn_cofi_epbase_vfptr = {
    nn_cofi_stop,
    nn_cofi_destroy
};

/* State machine functions. */
static void nn_cofi_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_cofi_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);

struct nn_cofi {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /* The high-level api structures */
    struct ofi_resources        ofi;
    struct ofi_active_endpoint  ep;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct nn_epbase epbase;

    /*  State machine that handles the active part of the connection
        lifetime. */
    struct nn_sofi sofi;

};

/**
 * Create a connected OFI Socket
 */
int nn_cofi_create (void *hint, struct nn_epbase **epbase)
{
    int ret;
    struct nn_cofi *self;
    const char * domain;
    const char * service;

    printf("OFI: Creating connected OFI socket\n");

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_cofi), "ctcp");
    alloc_assert (self);

    /*  Initalise the endpoint. */
    nn_epbase_init (&self->epbase, &nn_cofi_epbase_vfptr, hint);
    domain = nn_epbase_getaddr (&self->epbase);

    /* Get local service */
    service = strrchr (domain, ':');
    if (service == NULL) {
        nn_epbase_term (&self->epbase);
        return -EINVAL;
    }
    *(char*)(service) = '\0'; /* << TODO: That's a HACK! */
    service++;

    /* Debug */
    printf("OFI: Creating client OFI socket to (domain=%s, service=%s)\n", domain, 
        service );

    /* Initialize ofi */
    ret = ofi_alloc( &self->ofi, FI_EP_MSG );
    if (ret) {
        nn_epbase_term (&self->epbase);
        nn_free(self);
        return ret;
    }

    /* Start server */
    ret = ofi_init_client( &self->ofi, &self->ep, 
        FI_SOCKADDR, domain, service );
    if (ret) {
        nn_epbase_term (&self->epbase);
        nn_free(self);
        return ret;
    }

    /*  Initialise the root FSM. */
    nn_fsm_init_root(&self->fsm, 
        nn_cofi_handler, 
        nn_cofi_shutdown,
        nn_epbase_getctx( &self->epbase ));
    self->state = NN_COFI_STATE_IDLE;

    /* Start FSM. */
    nn_fsm_start( &self->fsm );

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    /* Success */
    return 0;
}

/**
 * Stop the OFI endpint
 */
static void nn_cofi_stop (struct nn_epbase *self)
{
    printf("OFI: Stopping OFI\n");

    /* TODO: Implement */
}

/**
 * Destroy the OFI FSM
 */
static void nn_cofi_destroy (struct nn_epbase *self)
{
    printf("OFI: Destroying OFI\n");

    /* Get reference to the cofi structure */
    struct nn_cofi *cofi;
    cofi = nn_cont(self, struct nn_cofi, epbase);

    /* TODO: Implement */

    /* Free structures */
    nn_free (cofi);
}

/**
 * Shutdown OFI FSM Handler
 *
 * Depending on the state the FSM is currently in, this 
 * function should perform the appropriate clean-up operations.
 */
static void nn_cofi_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    printf("OFI: Shutting down OFI\n");

    /* TODO: Implement */

}

/**
 * Bound OFI FSM Handler
 */
static void nn_cofi_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_cofi *cofi;

    /* Continue with the next OFI Event */
    cofi = nn_cont (self, struct nn_cofi, fsm);
    printf("> nn_cofi_handler state=%i, src=%i, type=%i\n", 
        cofi->state, src, type);

    /* Handle new state */
    switch (cofi->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_COFI_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:

                /* Create new connected OFI */
                printf("OFI: COFI: Creating new SOFI\n");
                cofi->state = NN_COFI_STATE_CONNECTED;
                nn_sofi_init (&cofi->sofi, &cofi->ofi, &cofi->ep, &cofi->epbase, 
                    NN_COFI_SRC_SOFI, &cofi->fsm);

                return;
            default:
                nn_fsm_bad_action (cofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (cofi->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (cofi->state, src, type);

    }

}
