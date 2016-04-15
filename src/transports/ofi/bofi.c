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
#include "ofiw.h"
#include "ofiapi.h"
#include "bofi.h"
#include "sofi.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/efd.h"
#include "../../aio/ctx.h"
#include "../../aio/worker.h"

/* BOFI States */
#define NN_BOFI_STATE_IDLE              1
#define NN_BOFI_STATE_ACTIVE            2
#define NN_BOFI_STATE_STOPPING          3
#define NN_BOFI_STATE_ACCEPTING         4

/* BOFI Actions */
#define NN_BOFI_CONNECTION_ACCEPTED     1

/* BOFI Child FSM Sources */
#define NN_BOFI_SRC_SOFI                NN_OFI_SRC_SOFI
#define NN_BOFI_SRC_ENDPOINT            4100

struct nn_bofi {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /* The high-level OFI API structures */
    struct ofi_fabric *fabric;
    struct ofi_domain *domain;
    struct ofi_passive_endpoint *pep;

    /* Helper to find offsets */
    uint8_t *sofi_offsets;
    int sofi_offsets_size;

    /* The Connected OFIs */
    struct nn_list sofis;

    /* The worker that receives EQ events */
    struct nn_ofiw *worker;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct nn_epbase epbase;

};

/* nn_epbase virtual interface implementation. */
static void nn_bofi_stop (struct nn_epbase *self);
static void nn_bofi_destroy (struct nn_epbase *self);
const struct nn_epbase_vfptr nn_bofi_epbase_vfptr = {
    nn_bofi_stop,
    nn_bofi_destroy
};

/* State machine functions. */
static void nn_bofi_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_bofi_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);

/* ########################################################################## */
/*  Utility Functions                                                         */
/* ########################################################################## */

/**
 * An unrecoverable error has occured
 */
static void nn_bofi_critical_error( struct nn_bofi * self, int error )
{
    _ofi_debug("OFI[B]: Unrecoverable error #%i: %s\n", error,
        fi_strerror((int) -error));

    nn_epbase_set_error( &self->epbase, error );
    nn_fsm_stop( &self->fsm );
}

/**
 * Find and return a free SOFI offset
 */
static int nn_bofi_new_sofi_offset( struct nn_bofi * self )
{
    int i;
    int base;

    /* Find a free sot */
    for (i=0; i<self->sofi_offsets_size; ++i) {

        /* If found a free, mark and return */
        if (self->sofi_offsets[i] == 0) {
            self->sofi_offsets[i] = 1;
            return i;
        }

    }

    /* Not found in the range, so allocate a few more */
    base = self->sofi_offsets_size;
    self->sofi_offsets_size += 64;
    self->sofi_offsets = nn_realloc( self->sofi_offsets, 
        sizeof(uint8_t) * self->sofi_offsets_size );
    memset( &self->sofi_offsets[base], 0, sizeof(uint8_t)*64 );

    /* Return new ID */
    return base;
}

static void nn_bofi_free_sofi_offset( struct nn_bofi * self, int offset )
{
    int i;

    /* Free slot */
    nn_assert( offset < self->sofi_offsets_size );
    self->sofi_offsets[offset] = 0;

}

/**
 * Create a new passive endpoint and start listening for incoming connections
 * on the active fabric.
 */
static int nn_bofi_start_accepting ( struct nn_bofi* self )
{
    int ret;

    /* Disable worker */
    nn_ofiw_stop( self->worker );

    /* Open passive endpoint bound to our worker */
    ret = ofi_passive_endpoint_open( self->fabric, self->worker, 
        NN_BOFI_SRC_ENDPOINT, NULL, &self->pep );
    if (ret) {
        FT_PRINTERR("ofi_passive_endpoint_open", ret);        
        return ret;
    }

    /* Listen for incoming PEP EQ event */
    ret = ofi_cm_listen( self->pep );
    if (ret) {
        FT_PRINTERR("ofi_cm_listen", ret);        
        return ret;
    }

    /* Enable worker */
    nn_ofiw_start( self->worker );

    /* We are listening */
    self->state = NN_BOFI_STATE_ACTIVE;
    _ofi_debug("OFI[B]: We are listening for connections\n");

    /* Success */
    return 0;

}

/**
 * Release the resources associated with the specified SOFI
 */
static void nn_bofi_reap_sofi(struct nn_bofi *self, struct nn_sofi *sofi)
{
    /* The SOFI fsm was stopped */
    _ofi_debug("OFI[B]: Reaping stopped SOFI\n");

    /* Release the offset */
    nn_bofi_free_sofi_offset( self, sofi->offset );

    /* Remove item from list */
    nn_list_erase (&self->sofis, &sofi->item);

    /* Cleanup */
    nn_sofi_term(sofi);
    nn_free(sofi);
}

/* ########################################################################## */
/*  Implementation  Functions                                                 */
/* ########################################################################## */

/**
 * Create a bound (server) OFI Socket
 */
int nn_bofi_create (void *hint, struct nn_epbase **epbase, 
    struct ofi_fabric * fabric)
{
    int ret;
    struct nn_bofi *self;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_bofi), "bofi");
    alloc_assert (self);

    /*  Initalise the endpoint. */
    nn_epbase_init (&self->epbase, &nn_bofi_epbase_vfptr, hint);

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

    /* Initialize properties */
    self->pep = NULL;
    nn_list_init (&self->sofis);

    /* Allocate a couple of offset slots */
    self->sofi_offsets = nn_alloc( sizeof(uint8_t) * 64, "offsets" );
    self->sofi_offsets_size = 64;
    memset( self->sofi_offsets, 0, sizeof(uint8_t)*64 );

    /*  Initialise the root FSM. */
    nn_fsm_init_root(&self->fsm, 
        nn_bofi_handler, 
        nn_bofi_shutdown,
        nn_epbase_getctx( &self->epbase ));
    self->state = NN_BOFI_STATE_IDLE;

    /* Start FSM. */
    nn_fsm_start( &self->fsm );

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;
    return 0;
}

/**
 * Stop the Bound OFI FSM
 */
static void nn_bofi_stop (struct nn_epbase *self)
{
    _ofi_debug("OFI[B]: Stopping BOFI\n");

    /* Get reference to the bofi structure */
    struct nn_bofi *bofi;
    bofi = nn_cont(self, struct nn_bofi, epbase);

    /* Stop the FSM */
    nn_fsm_stop (&bofi->fsm);
}

/**
 * Destroy the OFI FSM
 */
static void nn_bofi_destroy (struct nn_epbase *self)
{
    _ofi_debug("OFI[B]: Destroying OFI\n");

    /* Get reference to the bofi structure */
    struct nn_bofi *bofi;
    bofi = nn_cont(self, struct nn_bofi, epbase);

    /* Free structures */
    ofi_domain_close( bofi->domain );
    nn_ofiw_term( bofi->worker );
    nn_list_term ( &bofi->sofis );
    nn_free( bofi );
}

/**
 * Shutdown OFI FSM Handler
 *
 * Depending on the state the FSM is currently in, this 
 * function should perform the appropriate clean-up operations.
 */
static void nn_bofi_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_list_item *it;
    struct nn_sofi *sofi;
    struct nn_bofi *bofi;

    _ofi_debug("OFI[B]: Shutdown\n");

    /* Get pointer to bofi structure */
    bofi = nn_cont (self, struct nn_bofi, fsm);

    /* Switch to shutdown if this was an fsm action */
    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {

        /* Switch to shutting down state */
        bofi->state = NN_BOFI_STATE_STOPPING;

        /* Close the passive endpoint. */
        _ofi_debug("OFI[B]: Stopping passive endpoint\n");
        ofi_passive_endpoint_close( bofi->pep );

        /* Send the stop event to all SOFIs */
        for (it = nn_list_begin (&bofi->sofis);
              it != nn_list_end (&bofi->sofis);
              it = nn_list_next (&bofi->sofis, it)) {

            /* Stop the specified sofi */
            sofi = nn_cont (it, struct nn_sofi, item);
            nn_sofi_stop( sofi );

        }
    } 
    else if (nn_slow(src == NN_BOFI_SRC_SOFI && type == NN_SOFI_STOPPED))
    {
        /* Reap SOFIs as they close */
        sofi = (struct nn_sofi *) srcptr;
        nn_bofi_reap_sofi( bofi, sofi );

    } else {
        /* Invalid fsm state */
        nn_fsm_bad_state (bofi->state, src, type);
    }

    /* Wait until all SOFIs are closed */
    if (nn_list_empty(&bofi->sofis)) {
        nn_fsm_stopped_noevent(&bofi->fsm);
        nn_epbase_stopped (&bofi->epbase);
    }

}

/**
 * Bound OFI FSM Handler
 */
static void nn_bofi_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct fi_eq_cm_entry * eq_cm;
    struct ofi_active_endpoint * ep;
    struct nn_bofi *bofi;
    struct nn_sofi *sofi;
    int ret;

    /* Continue with the next OFI Event */
    bofi = nn_cont (self, struct nn_bofi, fsm);
    _ofi_debug("OFI[B]: nn_bofi_handler state=%i, src=%i, type=%i\n", 
        bofi->state, src, type);

    /* Handle new state */
    switch (bofi->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_BOFI_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:

                /* Start accepting incoming connections */
                ret = nn_bofi_start_accepting( bofi );
                if (ret) {
                    FT_PRINTERR("nn_bofi_start_accepting", ret);
                    nn_bofi_critical_error( bofi, ret );
                }
                return;

            default:
                nn_fsm_bad_action (bofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (bofi->state, src, type);
        }

/******************************************************************************/
/*  NN_BOFI_STATE_ACTIVE state.                                            */
/*  the socket is listening for incoming connections                          */
/******************************************************************************/
    case NN_BOFI_STATE_ACTIVE:
        switch (src) {

        /* Reap dead SOFIs */
        case NN_BOFI_SRC_SOFI:
            switch (type) {

            /* A SOFI has stopped */
            case NN_SOFI_STOPPED:

                /* Reap the dead SOFI */                
                sofi = (struct nn_sofi *) srcptr;
                nn_bofi_reap_sofi( bofi, sofi );
                return;

            default:
                nn_fsm_bad_action (bofi->state, src, type);

            }

        /* Wait for socket EQ Event */
        case NN_BOFI_SRC_ENDPOINT | OFI_SRC_EQ:
            switch (type) {

            /* Incoming connection request */
            case FI_CONNREQ:
                _ofi_debug("OFI[B]: Got connection request\n");
    
                /* Get incoming EQ request */                
                eq_cm = (struct fi_eq_cm_entry *) srcptr;

                /* Create a new SOFI to handle the incoming stream */
                _ofi_debug("OFI[B]: Allocating new SOFI\n");
                sofi = nn_alloc (sizeof (struct nn_sofi), "sofi");
                alloc_assert (sofi);

                /* Initialize SOFI */
                _ofi_debug("OFI[B]: Initializing SOFI\n");
                ret = nn_sofi_init (sofi, bofi->domain, 
                    nn_bofi_new_sofi_offset(bofi), &bofi->epbase, 
                    NN_BOFI_SRC_SOFI, &bofi->fsm);
                if (ret) {
                    nn_bofi_reap_sofi( bofi, sofi );
                    if (ret) {
                        FT_PRINTERR("nn_sofi_init", ret);
                        nn_bofi_critical_error( bofi, ret );
                    }
                    return;
                }

                /* Tell SOFI to accept the endpoint connection */
                _ofi_debug("OFI[B]: Starting SOFI in accept state\n");
                ret = nn_sofi_start_accept( sofi, eq_cm );
                if (ret) {
                    FT_PRINTERR("nn_sofi_start_accept", ret);

                    /* Reject the connection */
                    ofi_cm_reject( bofi->pep, eq_cm->info );
                    ofi_passive_endpoint_close( bofi->pep );

                    /* Release SOFI resources */
                    nn_sofi_term( sofi );
                    nn_free( sofi );

                    /* Start waiting for a new incoming connection. */
                    ret = nn_bofi_start_accepting( bofi );
                    if (ret) {
                        FT_PRINTERR("nn_bofi_start_accepting", ret);
                        nn_bofi_critical_error( bofi, ret );
                    }
                    return;

                }

                /* Stop passive endpoint */
                ofi_passive_endpoint_close( bofi->pep );

                /* Move the newly created connection to the list of existing
                    connections. */
                nn_list_insert (&bofi->sofis, &sofi->item,
                    nn_list_end (&bofi->sofis));

                /* Start waiting for a new incoming connection. */
                ret = nn_bofi_start_accepting( bofi );
                if (ret) {
                    FT_PRINTERR("nn_bofi_start_accepting", ret);
                    nn_bofi_critical_error( bofi, ret );
                }
                return;

            default:
                nn_fsm_bad_action (bofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (bofi->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (bofi->state, src, type);

    }

}
