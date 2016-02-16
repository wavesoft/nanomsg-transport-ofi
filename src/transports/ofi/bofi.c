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
#include "bofi.h"
#include "sofi.h"
#include "hlapi.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/efd.h"
#include "../../aio/ctx.h"

/* BOFI States */
#define NN_BOFI_STATE_IDLE              1
#define NN_BOFI_STATE_ACCEPTING         2
#define NN_BOFI_STATE_STOPPING          3

/* BOFI Actions */
#define NN_BOFI_CONNECTION_ACCEPTED     1

/* BOFI Child FSM Sources */
#define NN_BOFI_SRC_SOFI                NN_OFI_SRC_SOFI

struct nn_bofi {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /* The high-level api structures */
    struct ofi_resources        ofi;
    struct ofi_passive_endpoint pep;

    /* The Connected OFIs */
    struct nn_sofi *            sofi;
    struct nn_list              sofis;

    /* The accepting thread and sync mutex */
    struct nn_thread            thread;
    struct nn_efd               sync;

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

/**
 * Create a bound (server) OFI Socket
 */
int nn_bofi_create (void *hint, struct nn_epbase **epbase)
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

    /* Initialize ofi */
    ret = ofi_alloc( &self->ofi, FI_EP_MSG );
    if (ret) {
        nn_epbase_term (&self->epbase);
        nn_free(self);
        return ret;
    }

    /* Start server */
    ret = ofi_init_server( &self->ofi, &self->pep, FI_SOCKADDR, domain, 
        service );
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

    /* Initialize the list of Connected OFI Connections */
    self->sofi = NULL;
    nn_list_init (&self->sofis);

    /* Prepare thread resources */
    nn_efd_init( &self->sync );

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
    ofi_free( &bofi->ofi );
    nn_efd_term( &bofi->sync );
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

        /* Stop OFI operations */
        _ofi_debug("OFI[B]: Freeing passive endpoint resources\n");
        ofi_shutdown_pep( &bofi->pep );
        ofi_free_pep( &bofi->pep );

        /*  Wait till worker thread terminates. */
        nn_thread_term (&bofi->thread);

        /* We are stopped */
        nn_fsm_stopped_noevent(&bofi->fsm);
        return;

    }

    /* Invalid fsm action */
    nn_fsm_bad_state (bofi->state, src, type);

}

/**
 * The internal thread that takes care of the blocking accept() operations
 */
static void nn_bofi_accept_thread (void *arg)
{
    ssize_t ret;
    struct ofi_active_endpoint * ep;
    struct nn_bofi * self = (struct nn_bofi *) arg;

    /* Infinite loop */
    while (1) {

        /* Allocate new endpoint */
        ep = nn_alloc( sizeof (struct ofi_active_endpoint), 
            "ofi-active-endpoint" );
        alloc_assert (ep);

        /* Listen for incoming connections */
        _ofi_debug("OFI[B]: bofi_accept_thread: Waiting for incoming connections\n");
        ret = ofi_server_accept( &self->ofi, &self->pep, ep );
        if (ret == FI_SHUTDOWN) {
            _ofi_debug("OFI[B]: bofi_accept_thread: Stopping because of FI_SHUTDOWN\n");

            /* Free resources */
            ofi_free_ep(ep);
            nn_free(ep);
            break;

        } else if (ret < 0) {
            printf("OFI: ERROR: Cannot accept incoming connection!\n");

            /* Free resources */
            nn_free(ep);

            /* TODO: Forward event? */

            break;
        }

        /* Check if we are being stopped */
        if (self->state != NN_BOFI_STATE_ACCEPTING) {
            _ofi_debug("OFI[B]: bofi_accept_thread: Stopping because switched to state %i\n", self->state);

            /* Free resources */
            ofi_free_ep(ep);
            nn_free(ep);
            break;
        }

        /* Create new connected OFI */
        _ofi_debug("OFI[B]: bofi_accept_thread: Allocating new SOFI\n");
        self->sofi = nn_alloc (sizeof (struct nn_sofi), "sofi");
        alloc_assert (self->sofi);
        nn_sofi_init (self->sofi, &self->ofi, ep, NN_SOFI_NG_RECV,
            &self->epbase, NN_BOFI_SRC_SOFI, &self->fsm);

        /* Notify FSM that a connection was accepted */
        _ofi_debug("OFI[B]: bofi_accept_thread: Notifying FSM for the result\n");
        nn_ctx_enter( self->fsm.ctx );
        nn_fsm_action( &self->fsm, NN_BOFI_CONNECTION_ACCEPTED );
        nn_ctx_leave( self->fsm.ctx );

        /* Wait for event to be handled */
        _ofi_debug("OFI[B]: bofi_accept_thread: Waiting for ack from FSM\n");
        nn_efd_wait( &self->sync, -1 );
        nn_efd_unsignal( &self->sync );

    }

}
/**
 * Bound OFI FSM Handler
 */
static void nn_bofi_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_bofi *bofi;
    struct nn_sofi *sofi;

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

                /* Start accept thread */
                nn_thread_init (&bofi->thread, nn_bofi_accept_thread, bofi);
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
/*  the accepting thread is listening for incoming connections                */
/******************************************************************************/
    case NN_BOFI_STATE_ACCEPTING:
        switch (src) {

        /* Local thread actions */
        case NN_FSM_ACTION:
            switch (type) {
            case NN_BOFI_CONNECTION_ACCEPTED:

                /*  Move the newly created connection to the list of existing
                    connections. */
                nn_list_insert (&bofi->sofis, &bofi->sofi->item,
                    nn_list_end (&bofi->sofis));
                bofi->sofi = NULL;

                /* Acknowledge event and resume operation */
                nn_efd_signal( &bofi->sync );
                self->state = NN_BOFI_STATE_ACCEPTING;

                return;
            default:
                nn_fsm_bad_action (bofi->state, src, type);
            }

        /* SOFI FSM actions */
        case NN_BOFI_SRC_SOFI:

            /* Get reference to sofi */
            sofi = (struct nn_sofi *) srcptr;

            switch (type) {
            case NN_SOFI_STOPPED:
                /* The SOFI fsm was stopped */
                _ofi_debug("OFI[B]: Cleaning-up SOFI\n");

                /* Remove item from list */
                nn_list_erase (&bofi->sofis, &sofi->item);

                /* Cleanup */
                nn_sofi_term(sofi);
                nn_free(sofi);

                return;

            case NN_SOFI_INTERRUPTED:
                /* A remote enpodint was disconnected */
                _ofi_debug("OFI[B]: Connection interrupted, stopping SOFI\n");

                /* Stop sofi */
                if (!nn_sofi_isidle (sofi)) {
                    nn_sofi_stop (sofi);
                }

                return;
            default:
                nn_fsm_bad_action (bofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (bofi->state, src, type);
        }

/******************************************************************************/
/*  NN_BOFI_STATE_STOPPING state.                                             */
/*  the fsm is being stopped, ignore new events and prepare for cleanup       */
/******************************************************************************/
    case NN_BOFI_STATE_STOPPING:
        switch (src) {

        /* Local thread actions */
        case NN_FSM_ACTION:
            switch (type) {
            case NN_BOFI_CONNECTION_ACCEPTED:

                /*  Move the newly created connection to the list of existing
                    connections. */
                nn_list_insert (&bofi->sofis, &bofi->sofi->item,
                    nn_list_end (&bofi->sofis));
                bofi->sofi = NULL;

                /* Acknowledge event and resume operation */
                nn_efd_signal( &bofi->sync );
                self->state = NN_BOFI_STATE_ACCEPTING;

                return;
            default:
                nn_fsm_bad_action (bofi->state, src, type);
            }

        /* SOFI FSM actions */
        case NN_BOFI_SRC_SOFI:

            /* Get reference to sofi */
            sofi = (struct nn_sofi *) srcptr;

            switch (type) {
            case NN_SOFI_STOPPED:
                /* The SOFI fsm was stopped */
                _ofi_debug("OFI[B]: Cleaning-up SOFI\n");

                /* Remove item from list */
                nn_list_erase (&bofi->sofis, &sofi->item);

                /* Cleanup */
                nn_sofi_term(sofi);
                nn_free(sofi);

                return;

            case NN_SOFI_INTERRUPTED:
                /* A remote enpodint was disconnected */
                _ofi_debug("OFI[B]: Connection interrupted, stopping SOFI\n");

                /* Stop sofi */
                if (!nn_sofi_isidle (sofi)) {
                    nn_sofi_stop (sofi);
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
