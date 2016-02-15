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

#include "sofi_out.h"

/* FSM States */
#define NN_SOFI_OUT_STATE_IDLE          3001

/* FSM Sources */
#define NN_SOFI_OUT_SRC_TIMER           3101

/* Forward Declarations */
static void nn_sofi_out_handler (struct nn_fsm *self, int src, int type, 
    void *srcptr);
static void nn_sofi_out_shutdown (struct nn_fsm *self, int src, int type, 
    void *srcptr);

/* ============================== */
/*       HELPER FUNCTIONS         */
/* ============================== */


/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/*  Initialize the state machine */
void nn_sofi_out_init (struct nn_sofi_out *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep,
    const uint8_t ng_direction, struct np_pipebase * pipebase,
    int src, struct nn_fsm *owner)
{


    /* Initialize FSM */
    nn_fsm_init (&self->fsm, nn_sofi_out_handler, nn_sofi_out_shutdown,
        src, self, owner);

    /* Reset properties */
    self->state = NN_SOFI_OUT_STATE_IDLE;
    self->error = 0;

}

/* Check if FSM is idle */
int nn_sofi_out_isidle (struct nn_sofi_out *self)
{

}

/*  Stop the state machine */
void nn_sofi_out_stop (struct nn_sofi_out *self)
{

}

/*  Cleanup the state machine */
void nn_sofi_out_term (struct nn_sofi_out *self)
{

}

/* ============================== */
/*          FSM HANDLERS          */
/* ============================== */

/**
 * SHUTDOWN State Handler
 */
static void nn_sofi_out_shutdown (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{
    /* Get pointer to sofi structure */
    struct nn_sofi_out *self;
    self = nn_cont (fsm, struct nn_sofi_out, fsm);

}

/**
 * ACTIVE State Handler
 */
static void nn_sofi_out_handler (struct nn_fsm *fsm, int src, int type, 
    void *srcptr)
{

    /* Get pointer to sofi structure */
    struct nn_sofi_out *self;
    self = nn_cont (fsm, struct nn_sofi_out, fsm);

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


