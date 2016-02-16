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

/* State machine states */
#define NN_COFI_STATE_IDLE              1
#define NN_COFI_STATE_CONNECTED         2
#define NN_COFI_STATE_STOPPING          3

/* State machine sources */
#define NN_COFI_SRC_SOFI                NN_OFI_SRC_SOFI

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
    struct nn_fsm               fsm;
    int                         state;

    /* The high-level api structures */
    struct ofi_resources        ofi;
    struct ofi_active_endpoint  ep;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct nn_epbase            epbase;

    /*  State machine that handles the active part of the connection
        lifetime. */
    struct nn_sofi              sofi;

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

    _ofi_debug("OFI[C]: Creating connected OFI socket\n");

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
    _ofi_debug("OFI[C]: Createing socket for (domain=%s, service=%s)\n", domain, 
        service );

    /* Initialize ofi */
    ret = ofi_alloc( &self->ofi, FI_EP_MSG );
    if (ret) {
        _ofi_debug("OFI[C]: Failed to ofi_alloc!\n");
        nn_epbase_term (&self->epbase);
        nn_free(self);
        return ret;
    }

    /* Start server */
    ret = ofi_init_client( &self->ofi, &self->ep, 
        FI_SOCKADDR, domain, service );
    if (ret) {
        _ofi_debug("OFI[C]: Failed to ofi_init_client!\n");
        nn_epbase_term (&self->epbase);
        nn_free(self);
        return ret;
    }

    /*  Initialise the root FSM. */
    nn_fsm_init_root(&self->fsm, 
        nn_cofi_handler, 
        nn_cofi_shutdown,
        nn_epbase_getctx( &self->epbase ));

    /* Start FSM. */
    self->state = NN_COFI_STATE_IDLE;
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
    _ofi_debug("OFI[C]: Stopping COFI\n");

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
    _ofi_debug("OFI[C]: Destroying COFI\n");

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
    _ofi_debug("OFI[C]: Shutting down COFI\n");

    /* Get reference to the cofi structure */
    struct nn_cofi *cofi;
    cofi = nn_cont(self, struct nn_cofi, fsm);

    /* Switch to shutdown if this was an fsm action */
    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {

        /* Enter stopping state */
        cofi->state = NN_COFI_STATE_STOPPING;

        /* Stop SOFI if not already stopped */
        if (!nn_sofi_isidle (&cofi->sofi)) {
            _ofi_debug("OFI[C]: SOFI is not idle, shutting down\n");
            nn_sofi_stop (&cofi->sofi);
            return;
        }
    }

    /* If we are in shutting down state, stop everyhing else */
    if (cofi->state == NN_COFI_STATE_STOPPING) {

        /* We are stopped */
        _ofi_debug("OFI[C]: Stopping epbase\n");
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
    _ofi_debug("OFI[C]: nn_cofi_handler state=%i, src=%i, type=%i\n", 
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
                _ofi_debug("OFI[C]: Creating new SOFI\n");
                cofi->state = NN_COFI_STATE_CONNECTED;
                nn_sofi_init (&cofi->sofi, &cofi->ofi, &cofi->ep, NN_SOFI_NG_SEND, 
                    &cofi->epbase, NN_COFI_SRC_SOFI, &cofi->fsm);

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
            case NN_SOFI_STOPPED:

                /* Disconnected from remote endpoint */
                _ofi_debug("OFI[C]: Connection dropped\n");
                nn_fsm_stop (&cofi->fsm);

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
