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

#include "sofi_in.h"

/* FSM States */
#define NN_SOFI_IN_STATE_IDLE           2001
#define NN_SOFI_IN_STATE_POSTED         2002
#define NN_SOFI_IN_STATE_PROCESSING     2003
#define NN_SOFI_IN_STATE_ERROR          2004
#define NN_SOFI_IN_STATE_CLOSED         2005
#define NN_SOFI_IN_STATE_ABORTING       2006
#define NN_SOFI_IN_STATE_ABORT_CLEANUP  2007
#define NN_SOFI_IN_STATE_ABORT_TIMEOUT  2008

/* FSM Sources */
#define NN_SOFI_IN_SRC_TIMER            2101

/* Forward Declarations */
static void nn_sofi_in_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_in_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);

/* ============================== */
/*       HELPER FUNCTIONS         */
/* ============================== */

/* Post buffers */
static int nn_sofi_in_post_buffers(struct nn_sofi_in *self)
{

}

/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/*  Initialize the state machine */
void nn_sofi_in_init (struct nn_sofi_in *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep,
    const uint8_t ng_direction, int src, struct nn_fsm *owner)
{

    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_in_handler, nn_sofi_in_shutdown,
        src, self, owner);

    /* Reset properties */
    self->state = NN_SOFI_IN_STATE_IDLE;
    self->error = 0;

}

/* Check if FSM is idle */
int nn_sofi_in_isidle (struct nn_sofi_in *self)
{

}

/*  Stop the state machine */
void nn_sofi_in_stop (struct nn_sofi_in *self)
{

}

/*  Cleanup the state machine */
void nn_sofi_in_term (struct nn_sofi_in *self)
{

}

/* ============================== */
/*         INPUT EVENTS           */
/* ============================== */

/* Data needs to be sent */
void nn_sofi_in_event__tx_data (struct nn_sofi_in *self, /* Data */)
{

}

/* Acknowledge transmission of data */
void nn_sofi_in_event__tx_ack (struct nn_sofi_in *self)
{

}

/* ============================== */
/*          FSM HANDLERS          */
/* ============================== */

/**
 * SHUTDOWN State Handler
 */
static void nn_sofi_in_shutdown (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{

}

/**
 * ACTIVE State Handler
 */
static void nn_sofi_in_handler (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{

    /* Get pointer to sofi structure */
    struct nn_sofi_in *self;
    self = nn_cont (fsm, struct nn_sofi_in, fsm);

    /* Handle state transitions */
    switch (self->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_SOFI_IN_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:

                /* Post buffers */
                self->state = NN_SOFI_IN_STATE_POSTED;
                nn_sofi_in_post_buffers( self );
                return;

            default:
                nn_fsm_bad_action (self->state, src, type);
            }

        default:
            nn_fsm_bad_source (self->state, src, type);
        }

/******************************************************************************/
/*  POSTED state.                                                             */
/******************************************************************************/
    case NN_SOFI_IN_STATE_POSTED:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:

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


