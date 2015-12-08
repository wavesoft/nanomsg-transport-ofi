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

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"

#define NN_BOFI_STATE_IDLE      1
#define NN_BOFI_STATE_CLOSING   2

struct nn_bofi {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

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
    const char * addr;
    const char * local_domain;
    const char * local_service;
    const char * remote_domain;
    const char * remote_service;
    struct nn_bofi *self;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_bofi), "ctcp");
    alloc_assert (self);

    /*  Initalise the endpoint. */
    nn_epbase_init (&self->epbase, &nn_bofi_epbase_vfptr, hint);
    local_domain = nn_epbase_getaddr (&self->epbase);

    /* Separate local/remote endpoints */
    remote_domain = strrchr( local_domain, '@' );
    if (remote_domain == NULL) {

        /* No remote configuration */
        remote_service = NULL;

        /* Get local service */
        local_service = strrchr (local_domain, ':');
        if (local_service == NULL) {
            nn_epbase_term (&self->epbase);
            return -EINVAL;
        }
        *(char*)(local_service) = '\0'; /* << TODO: That's a HACK! */
        local_service++;

    } else {

        /* Separate remote domain */
        *(char*)(remote_domain) = '\0'; /* << TODO: That's a HACK! */
        remote_domain++;

        /* Get local service */
        local_service = strrchr (local_domain, ':');
        if (local_service == NULL) {
            nn_epbase_term (&self->epbase);
            return -EINVAL;
        }
        *(char*)(local_service) = '\0'; /* << TODO: That's a HACK! */
        local_service++;

        /* Get remote service */
        remote_service = strrchr (remote_domain, ':');
        if (remote_service == NULL) {
            nn_epbase_term (&self->epbase);
            return -EINVAL;
        }
        *(char*)(remote_service) = '\0'; /* << TODO: That's a HACK! */
        remote_service++;

    }

    /* Debug */
    printf("OFI: Creating bound OFI socket "
        "(domain=%s, service=%s, remote=%s, remote-service=%s)\n", 
        local_domain, local_service, remote_domain, remote_service );

    /*  Initialise the FSM. */
    nn_fsm_init_root(&self->fsm, 
        nn_bofi_handler, 
        nn_bofi_shutdown,
        nn_epbase_getctx( &self->epbase ));
    self->state = NN_BOFI_STATE_IDLE;

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

    /* Free structure */
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
                nn_bofi_start_listening (bofi);
                return;
            default:
                nn_fsm_bad_action (bofi->state, src, type);
            }

        default:
            nn_fsm_bad_source (bofi->state, src, type);
        }

/******************************************************************************/
/*  CLOSING_USOCK state.                                                     */
/*  usock object was asked to stop but it haven't stopped yet.                */
/******************************************************************************/
    case NN_BOFI_STATE_CLOSING:
        nn_fsm_bad_source (bofi->state, src, type);

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

static void nn_bofi_start_listening (struct nn_bofi *self)
{

    printf("OFI: Listening\n");

    // Just stop for now
    nn_fsm_stop(&self->fsm);
    self->state = NN_BOFI_STATE_CLOSING;

}

static void nn_bofi_start_accepting (struct nn_bofi *self)
{

}
