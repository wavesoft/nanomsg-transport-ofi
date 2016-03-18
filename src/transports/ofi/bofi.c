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
#include "bofi.h"
#include "sofi.h"
#include "hlapi.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/efd.h"
#include "../../aio/ctx.h"
#include "../../aio/worker.h"

/* BOFI States */
#define NN_BOFI_STATE_IDLE              1
#define NN_BOFI_STATE_ACCEPTING         2
#define NN_BOFI_STATE_STOPPING          3
#define NN_BOFI_STATE_LISTENING         4

/* BOFI Actions */
#define NN_BOFI_CONNECTION_ACCEPTED     1

/* BOFI Child FSM Sources */
#define NN_BOFI_SRC_SOFI                NN_OFI_SRC_SOFI
#define NN_BOFI_SRC_WORKER_ACCEPT       4101
#define NN_BOFI_SRC_WORKER_ERROR        4102

#define NN_BOFI_SRC_SOCKET_EQ           4103

struct nn_bofi {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /* The high-level api structures */
    struct ofi_resources *      ofi;
    struct ofi_passive_endpoint pep;
    struct nn_ofiw_pool         ofiw_pool;
    struct nn_ofiw              *ofiw;

    /* The Connected OFIs */
    struct nn_sofi *            sofi;
    struct nn_list              sofis;

    /* For receiving events from thread */
    struct nn_worker            * worker;
    struct ofi_active_endpoint  * accept_ep;
    struct nn_worker_task       task_accept;
    struct nn_worker_task       task_error;

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

/* Thread functions */
static void nn_bofi_accept_thread (void *arg);

/* Helper functions */
static int nn_bofi_start_accepting ( struct nn_bofi* self );

/**
 * Create a bound (server) OFI Socket
 */
int nn_bofi_create (void *hint, struct nn_epbase **epbase, struct ofi_resources * ofi)
{
    int ret;
    struct nn_bofi *self;
    const char * domain;
    const char * service;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_bofi), "bofi");
    alloc_assert (self);

    /*  Initalise the endpoint. */
    nn_epbase_init (&self->epbase, &nn_bofi_epbase_vfptr, hint);
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
    _ofi_debug("OFI[B]: Creating bound OFI socket (domain=%s, service=%s)\n", domain, 
        service );

    /* Keep resources */
    self->ofi = ofi;
    if (ofi->err) {
        _ofi_debug("OFI[B]: OFI was not properly initialized!\n");
        nn_epbase_term (&self->epbase);
        nn_free(self);
        return ofi->err;
    }

    /* Start server */
    ret = ofi_init_server( self->ofi, FI_SOCKADDR, domain, 
        service );
    if (ret) {
        nn_epbase_term (&self->epbase);
        nn_free(self);
        return ret;
    }

    /* Create an OFI Worker Pool */
    ret = nn_ofiw_pool_init( &self->ofiw_pool, self->ofi->fabric );
    if (ret) {
        nn_epbase_term (&self->epbase);
        nn_free(self);
        return ret;
    }

    /*  Initialise the root FSM. */
    nn_fsm_init_root(&self->fsm, 
        nn_bofi_handler, 
        nn_bofi_shutdown,
        nn_epbase_getctx( &self->epbase ));
    self->state = NN_BOFI_STATE_IDLE;

    /* Create an ofi worker */
    self->ofiw = nn_ofiw_pool_getworker( &self->ofiw_pool, &self->fsm );

    /* Initializ e the list of Connected OFI Connections */
    self->sofi =NULL;
    nn_list_init (&self->sofis);

    /*  Choose a worker thread to handle this socket. */
    self->accept_ep = NULL;
    self->worker = nn_fsm_choose_worker (&self->fsm);

    /* Initialize worker task to receive accept requests */
    nn_worker_task_init (&self->task_accept, NN_BOFI_SRC_WORKER_ACCEPT,
        &self->fsm);
    nn_worker_task_init (&self->task_error, NN_BOFI_SRC_WORKER_ERROR,
        &self->fsm);

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
    _ofi_debug("OFI[B]: Stopping OFI\n");

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

    /* Free open connection handlers */
    struct nn_list_item *it;
    struct nn_sofi *sofi;
    for (it = nn_list_begin (&bofi->sofis);
          it != nn_list_end (&bofi->sofis);
          it = nn_list_next (&bofi->sofis, it)) {
        sofi = nn_cont (it, struct nn_sofi, item);

        /* Stop SOFI */
        nn_sofi_stop(sofi);

        /* Cleanup */
        nn_sofi_term(sofi);
        nn_free(sofi);
    }

    /* Free structures */
    nn_ofiw_term( bofi->ofiw );
    nn_ofiw_pool_term( &bofi->ofiw_pool );
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
    _ofi_debug("OFI[B]: Shutdown\n");

    /* Get pointer to bofi structure */
    struct nn_bofi *bofi;
    bofi = nn_cont (self, struct nn_bofi, fsm);

    /* Switch to shutdown if this was an fsm action */
    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {

        /* Switch to shutting down state */
        bofi->state = NN_BOFI_STATE_STOPPING;

        /*  Post shutdown event, wait for thread to exit and then close PEP. */
        ofi_shutdown_pep( &bofi->pep );
        ofi_free_pep( &bofi->pep );

        /* We are stopped */
        nn_fsm_stopped_noevent(&bofi->fsm);
        nn_epbase_stopped (&bofi->epbase);
        return;

    }

    /* Invalid fsm action */
    nn_fsm_bad_state (bofi->state, src, type);

}

/**
 * Bound OFI FSM Handler
 */
static void nn_bofi_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct fi_eq_cm_entry * cq_entry;
    struct ofi_active_endpoint * ep;
    struct nn_bofi *bofi;
    struct nn_sofi *sofi;
    int ret;

    /* Continue with the next OFI Event */
    bofi = nn_cont (self, struct nn_bofi, fsm);
    _ofi_debug("OFI[B]: nn_bofi_handler state=%i, src=%i, type=%i\n", 
        bofi->state, src, type);

    /* Events from children SOFIs are handled separately */
    if (src == NN_BOFI_SRC_SOFI) {
        switch (type) {

            case NN_SOFI_STOPPED:

                /* The SOFI fsm was stopped */
                _ofi_debug("OFI[B]: Marking SOFI as inactive\n");

                /* Remove item from list */
                nn_list_erase (&bofi->sofis, &sofi->item);

                /* Cleanup */
                nn_sofi_term(sofi);
                nn_free(sofi);
                return;

        }
        return;
    }

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
                nn_bofi_start_accepting( bofi );
                bofi->state = NN_BOFI_STATE_ACCEPTING;
                return;

            default:
                nn_fsm_bad_action (bofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (bofi->state, src, type);
        }

/******************************************************************************/
/*  NN_BOFI_STATE_ACCEPTING state.                                            */
/*  the socket is listening for incoming connections                          */
/******************************************************************************/
    case NN_BOFI_STATE_ACCEPTING:
        switch (src) {

        case NN_BOFI_SRC_SOCKET_EQ:
            switch (type) {

            /* Incoming connection request */
            case FI_CONNREQ:
                _ofi_debug("OFI[B]: Got connection request\n");
    
                /* Get incoming CQ request */                
                cq_entry = (struct fi_eq_cm_entry *) srcptr;

                /* Stop monitoring passive ednpoint's EQ */
                nn_ofiw_remove( bofi->ofiw, bofi->pep.eq );

                /* Allocate new active enpoint */
                ep = nn_alloc( sizeof (struct ofi_active_endpoint), 
                    "ofi-active-endpoint" );
                alloc_assert(ep);

                /* Accept incoming request to the new active endpoint */
                ret = ofi_server_accept( bofi->ofi, cq_entry, ep );
                if (ret) {
                    FT_PRINTERR("ofi_server_accept", ret);

                    /* Reject connection */
                    fi_reject(bofi->pep.pep, cq_entry->info->handle, NULL, 0);

                    /* Free */
                    ofi_free_pep( &bofi->pep );
                    nn_free(ep);
                    return;
                }

                /* Close passive endpoint */
                ofi_free_pep( &bofi->pep );

                /* Create new SOFI */
                _ofi_debug("OFI[B]: Allocating new SOFI\n");
                bofi->sofi = nn_alloc (sizeof (struct nn_sofi), "sofi");
                alloc_assert (bofi->sofi);
                nn_sofi_init (bofi->sofi, bofi->ofi, ep, NN_SOFI_NG_RECV,
                    &bofi->epbase, NN_BOFI_SRC_SOFI, &bofi->fsm);

                /*  Move the newly created connection to the list of existing
                    connections. */
                nn_list_insert (&bofi->sofis, &bofi->sofi->item,
                    nn_list_end (&bofi->sofis));
                bofi->sofi = NULL;

                return;

            /* An incoming connection completed */
            case FI_CONNECTED:
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

/**
 * Bound OFI FSM Handler
 */
static int nn_bofi_start_accepting ( struct nn_bofi* self )
{
    int ret;

    /* Start listening for connections */
    ret = ofi_server_listen( self->ofi, &self->pep );
    if (ret) {
        FT_PRINTERR("ofi_server_listen", ret);        
        return ret;
    }

    /* Listen for incoming PEP EQ event */
    ret = nn_ofiw_add_eq( self->ofiw, self->pep.eq, NN_BOFI_SRC_SOCKET_EQ );
    if (ret) {
        FT_PRINTERR("nn_ofiw_add_eq", ret);        
        return ret;
    }

    /* We are listening */
    self->state = NN_BOFI_STATE_LISTENING;

    /* Success */
    return 0;

}
