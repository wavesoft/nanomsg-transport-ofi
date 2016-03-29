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
#include "ofiapi.h"
#include "cofi.h"
#include "sofi.h"

#include <string.h>

#include "../../core/ep.h"
#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"

/* State machine states */
#define NN_COFI_STATE_IDLE              1
#define NN_COFI_STATE_ACTIVE            2
#define NN_COFI_STATE_STOPPING          3

/* State machine sources */
#define NN_COFI_SRC_SOFI                NN_OFI_SRC_SOFI
#define NN_COFI_SRC_ENDPOINT            5100

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
    struct ofi_fabric *fabric;
    struct ofi_domain *domain;
    struct nn_ofiw *worker;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct nn_epbase epbase;

    /*  State machine that handles the active part of the connection
        lifetime. */
    struct nn_sofi sofi;

};

/* ########################################################################## */
/*  Utility Functions                                                         */
/* ########################################################################## */

/**
 * Create an active endpoint and place a connection request
 */
static int nn_cofi_start_connecting( struct nn_cofi * self )
{
    int ret;

    /* Initialize a new SOFI */
    nn_sofi_init( &self->sofi, self->domain, 0,
        &self->epbase, NN_COFI_SRC_SOFI, &self->fsm );

    /* Start SOFI */
    ret = nn_sofi_start_connect( &self->sofi );
    if (ret) {
        FT_PRINTERR("nn_sofi_start_connect", ret);
        return ret;
    }

    /* Success */
    return 0;

}

/**
 * Reap the SOFI object specified
 */
static void nn_cofi_reap_sofi( struct nn_cofi * self )
{
    /* Terminate SOFI */
    nn_sofi_term( &self->sofi );
}

/**
 * An unrecoverable error has occured
 */
static void nn_cofi_critical_error( struct nn_cofi * self, int error )
{
    _ofi_debug("OFI[C]: Unrecoverable error #%i: %s\n", error,
        fi_strerror((int) -error));

    nn_epbase_set_error( &self->epbase, error );
    nn_fsm_stop( &self->fsm );
}

/* ########################################################################## */
/*  Implementation  Functions                                                 */
/* ########################################################################## */

/**
 * Create a connected OFI Socket
 */
int nn_cofi_create (void *hint, struct nn_epbase **epbase, 
    struct ofi_fabric * fabric)
{
    int ret;
    struct nn_cofi *self;

    _ofi_debug("OFI[C]: Creating connected OFI socket\n");

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_cofi), "cofi");
    alloc_assert (self);
    memset( self, 0, sizeof (struct nn_cofi));

    /*  Initalise the endpoint. */
    nn_epbase_init (&self->epbase, &nn_cofi_epbase_vfptr, hint);

    /* Open a domain on this address */
    self->fabric = fabric;
    ret = ofi_domain_open( fabric, NULL, &self->domain );
    if (ret) {
        nn_epbase_term (&self->epbase);
        nn_free(self);
        return ret;
    }

    /* Create an ofi worker for this fabric */
    self->worker = ofi_fabric_getworker( self->fabric, &self->fsm );

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

    /* Stop SOFI (sofi also closes endpoint) */
    nn_sofi_term(&cofi->sofi);

    /* Clean-up OFI resources */
    nn_ofiw_term( cofi->worker );
    ofi_domain_close( cofi->domain );

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
static void nn_cofi_handler (struct nn_fsm *fsm, int src, int type,
    void *srcptr)
{
    int ret;
    struct nn_cofi *self;

    /* Continue with the next OFI Event */
    self = nn_cont (fsm, struct nn_cofi, fsm);
    _ofi_debug("OFI[C]: nn_cofi_handler state=%i, src=%i, type=%i\n", 
        self->state, src, type);

    /* Handle new state */
    switch (self->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_COFI_STATE_IDLE:
        switch (src) {

        /* ========================= */
        /*  FSM Action               */
        /* ========================= */
        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:

                /* Create a new SOFI and start connection */
                ret = nn_cofi_start_connecting( self );
                if (ret) {
                    nn_cofi_critical_error( self, ret );
                } else {                
                    self->state = NN_COFI_STATE_ACTIVE;
                }

                return;
            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  NN_COFI_STATE_ACTIVE state.                                               */
/*  Our SOFI is active, which means that a connection is established          */
/******************************************************************************/
    case NN_COFI_STATE_ACTIVE:
        switch (src) {

        /* ========================= */
        /*  Streaming OFI FSM        */
        /* ========================= */
        case NN_COFI_SRC_SOFI:
            switch (type) {

            /* The SOFI is stopped only when the connection is interrupted */
            case NN_SOFI_STOPPED:

                /* Disconnected from remote endpoint */
                _ofi_debug("OFI[C]: SOFI Stopped error=%i. Restarting\n", 
                    self->sofi.error);

                /* TODO: Use back-off timer */

                /* Start another SOFI */
                nn_cofi_reap_sofi( self );
                ret = nn_cofi_start_connecting( self );
                if (ret) {
                    nn_cofi_critical_error( self, ret );
                }

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

