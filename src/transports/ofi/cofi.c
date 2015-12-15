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

/* Helper macro to enable or disable verbose logs on console */
#ifdef OFI_DEBUG_LOG
    /* Enable debug */
    #define _ofi_debug(...)   printf(__VA_ARGS__)
#else
    /* Disable debug */
    #define _ofi_debug(...)
#endif

/* State machine states */
#define NN_COFI_STATE_IDLE              1
#define NN_COFI_STATE_CONNECTED         2
#define NN_COFI_STATE_STOPPING          3

/* State machine sources */
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

    _ofi_debug("OFI: Creating connected OFI socket\n");

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_cofi), "cofi");
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
    _ofi_debug("OFI: Creating client OFI socket to (domain=%s, service=%s)\n", domain, 
        service );

    /* Initialize ofi */
    ret = ofi_alloc( &self->ofi, FI_EP_MSG );
    if (ret) {
        _ofi_debug("OFI: Failed to ofi_alloc!\n");
        nn_epbase_term (&self->epbase);
        nn_free(self);
        return ret;
    }

    /* Start server */
    ret = ofi_init_client( &self->ofi, &self->ep, 
        FI_SOCKADDR, domain, service );
    if (ret) {
        _ofi_debug("OFI: Failed to ofi_init_client!\n");
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
    _ofi_debug("OFI: Stopping OFI\n");

    struct nn_cofi *cofi;
    cofi = nn_cont(self, struct nn_cofi, epbase);

    /* Stop FSM */
    nn_fsm_stop (&cofi->fsm);
}

/**
 * Destroy the OFI FSM
 */
static void nn_cofi_destroy (struct nn_epbase *self)
{
    _ofi_debug("OFI: Destroying OFI\n");

    /* Get reference to the cofi structure */
    struct nn_cofi *cofi;
    cofi = nn_cont(self, struct nn_cofi, epbase);

    /* Stop OFI (sofi also closes endpoint) */
    nn_sofi_term(&cofi->sofi);
    ofi_free( &cofi->ofi );

    /* Cleanup other resources */
    nn_fsm_term (&cofi->fsm);
    nn_epbase_term (&cofi->epbase);
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
    _ofi_debug("OFI: Shutting down OFI\n");

    /* Get reference to the cofi structure */
    struct nn_cofi *cofi;
    cofi = nn_cont(self, struct nn_cofi, fsm);

    /* Switch to shutdown if this was an fsm action */
    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {
        if (!nn_sofi_isidle (&cofi->sofi)) {
            nn_sofi_stop (&cofi->sofi);
        }
        cofi->state = NN_COFI_STATE_STOPPING;
    }

    /* If we are in shutting down state, stop everyhing else */
    if (cofi->state == NN_COFI_STATE_STOPPING) {

        // /* Wait for STCP to be idle */
        // if (!nn_sofi_isidle (&cofi->sofi)) {
        //     return;
        // }

        /* We are stopped */
        nn_fsm_stopped_noevent (&cofi->fsm);
        nn_epbase_stopped (&cofi->epbase);
        return;
    }

    /* Otherwise the fsm is in invalid state */
    nn_fsm_bad_state (cofi->state, src, type);

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
    _ofi_debug("OFI: nn_cofi_handler state=%i, src=%i, type=%i\n", 
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
                _ofi_debug("OFI: COFI: Creating new SOFI\n");
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
/*  NN_COFI_STATE_CONNECTED state.                                            */
/*  We are connected to the remote endpoint                                   */
/******************************************************************************/
    case NN_COFI_STATE_CONNECTED:
        switch (src) {

        case NN_COFI_SRC_SOFI:
            switch (type) {
            case NN_SOFI_DISCONNECTED:

                /* Disconnected from remote endpoint */
                printf("OFI: ERROR: Disconnected from remote endpoint (not implemented)\n");

                /* TODO: Implement this */

                return;

            default:
                nn_fsm_bad_action (cofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (cofi->state, src, type);
        }

/******************************************************************************/
/*  STOPPING state.                                                           */
/*  This state is initiated by nn_cofi_shutdown                               */
/******************************************************************************/
    case NN_COFI_STATE_STOPPING:
        switch (src) {

        case NN_COFI_SRC_SOFI:
            switch (type) {

            case NN_SOFI_STOPPED:
                /* Successfully stopped - We don't care */
                return;

            case NN_SOFI_DISCONNECTED:
                /* We are disconnected, but we are stopping - We don't care */
                _ofi_debug("Stopping Again!\n");
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
