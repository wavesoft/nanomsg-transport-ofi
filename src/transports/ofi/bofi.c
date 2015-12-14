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
#include "aofi.h"
#include "hlapi.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"

#define NN_BOFI_STATE_IDLE      1
#define NN_BOFI_STATE_ACTIVE    2
#define NN_BOFI_STATE_PENDING   3

#define NN_BOFI_SRC_SOFI        1
#define NN_BTCP_SRC_AOFI        2

struct nn_bofi {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /* The high-level api structures */
    struct ofi_resources        ofi;
    struct ofi_passive_endpoint pep;

    /* The Accepting OFI */
    struct nn_aofi *            aofi;
    struct nn_list              aofis;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct nn_epbase epbase;

};

/*  nn_epbase virtual interface implementation. */
static void nn_bofi_stop (struct nn_epbase *self);
static void nn_bofi_destroy (struct nn_epbase *self);
const struct nn_epbase_vfptr nn_bofi_epbase_vfptr = {
    nn_bofi_stop,
    nn_bofi_destroy
};

/*  State machine functions. */
static void nn_bofi_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_bofi_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_bofi_start_listening (struct nn_bofi *self);
static void nn_bofi_start_accepting (struct nn_bofi *self);

/**
 * Create a bound (server) OFI Socket
 */
int nn_bofi_create (void *hint, struct nn_epbase **epbase)
{
    int ret;
    struct nn_bofi *self;
    const char * addr;
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
    printf("OFI: Creating bound OFI socket (domain=%s, service=%s)\n", domain, service );

    /* Initialize ofi */
    ret = ofi_alloc( &self->ofi, FI_EP_MSG );
    if (ret < 0) return ret;

    /* Start server */
    ret = ofi_init_server( &self->ofi, &self->pep, FI_SOCKADDR, domain, service );
    if (ret < 0) return ret;

    /*  Initialise the root FSM. */
    nn_fsm_init_root(&self->fsm, 
        nn_bofi_handler, 
        nn_bofi_shutdown,
        nn_epbase_getctx( &self->epbase ));
    self->state = NN_BOFI_STATE_IDLE;

    /* Initialize the list of Active OFI Connections */
    self->aofi = NULL;
    nn_list_init (&self->aofis);

    /*  Start FSM. */
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
    printf("OFI: Stopping OFI\n");

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
    printf("OFI: Destroying OFI\n");

    /* Get reference to the bofi structure */
    struct nn_bofi *bofi;
    bofi = nn_cont(self, struct nn_bofi, epbase);

    /* Free structures */
    nn_list_term (&bofi->aofis);
    nn_free (bofi);
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
    printf("OFI: Shutting down OFI\n");

    // Nothing now
}

/**
 * Bound OFI FSM Handler
 */
static void nn_bofi_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_bofi *bofi;

    /* Continue with the next OFI Event */
    bofi = nn_cont (self, struct nn_bofi, fsm);
    printf("> nn_bofi_handler state=%i, src=%i, type=%i\n", bofi->state, src, type);

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
                nn_bofi_start_accepting (bofi);
                return;
            default:
                nn_fsm_bad_action (bofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (bofi->state, src, type);
        }

/******************************************************************************/
/*  NN_BOFI_STATE_ACTIVE state.                                               */
/*  the state machine is ready to handle incoming connections                 */
/******************************************************************************/
    case NN_BOFI_STATE_ACTIVE:
        switch (src) {

        case NN_BTCP_SRC_AOFI:
            switch (type) {
            case NN_AOFI_ACCEPTED:

                /*  Move the newly created connection to the list of existing
                    connections. */
                nn_list_insert (&bofi->aofis, &bofi->aofi->item,
                    nn_list_end (&bofi->aofis));
                bofi->aofi = NULL;

                /* Wait until SOFI is connected before accepting another connection */
                // self->state = NN_BOFI_STATE_PENDING;

                /*  Start waiting for a new incoming connection. */
                nn_bofi_start_accepting (bofi);

                return;
            default:
                nn_fsm_bad_action (bofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (bofi->state, src, type);
        }

/******************************************************************************/
/*  NN_BOFI_STATE_PENDING state.                                              */
/*  waiting confirmation from SOFI before waiting for another connection      */
/******************************************************************************/
    case NN_BOFI_STATE_PENDING:
        switch (src) {

        case NN_BTCP_SRC_SOFI:
            switch (type) {
            case NN_SOFI_CONNECTED:

                /*  Start waiting for a new incoming connection. */
                nn_bofi_start_accepting (bofi);
                self->state = NN_BOFI_STATE_ACTIVE;

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

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

/**
 * Create a new accepting FSM
 */
static void nn_bofi_start_accepting (struct nn_bofi *self)
{
    /*  Allocate new aofi state machine. */
    printf("OFI: Allocating new AOFI\n");
    self->aofi = nn_alloc (sizeof (struct nn_aofi), "aofi");
    alloc_assert (self->aofi);
    nn_aofi_init (self->aofi, &self->ofi, &self->pep, &self->epbase, NN_BTCP_SRC_AOFI, &self->fsm);

    /*  Start waiting for a new incoming connection. */
    self->state = NN_BOFI_STATE_ACTIVE;
    printf("OFI: Starting new AOFI\n");
    nn_aofi_start (self->aofi);

}
