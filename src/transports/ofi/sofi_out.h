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

#ifndef NN_SOFI_OUT_INCLUDED
#define NN_SOFI_OUT_INCLUDED

#include "hlapi.h"

#include "../../transport.h"
#include "../../aio/fsm.h"
#include "../../aio/timer.h"

/* FSM Events */
#define NN_SOFI_OUT_EVENT_STARTED       3201
#define NN_SOFI_OUT_EVENT_SENT          3202
#define NN_SOFI_OUT_EVENT_ERROR         3203
#define NN_SOFI_OUT_EVENT_CLOSE         3204

/* Shared, Connected OFI FSM */
struct nn_sofi_out {

    /* The state machine. */
    struct nn_fsm               fsm;
    int                         state;
    int                         error;

    /* References */
    struct nn_pipebase          * pipebase;
    struct ofi_resources        * ofi;
    struct ofi_active_endpoint  * ep;

    /* The FSM events */
    struct nn_fsm_event         error_event;
    struct nn_fsm_event         close_event;

    /* Abort cleanup timeout */
    struct nn_timer             abort_timer;

};

/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/*  Initialize the state machine */
void nn_sofi_out_init ( struct nn_sofi_out *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep,
    const uint8_t ng_direction, struct nn_pipebase * pipebase,
    int src, struct nn_fsm *owner );

/* Check if FSM is idle */
int nn_sofi_out_isidle (struct nn_sofi_out *self);

/*  Start the state machine */
void nn_sofi_out_start (struct nn_sofi_out *self);

/*  Stop the state machine */
void nn_sofi_out_stop (struct nn_sofi_out *self);

/*  Cleanup the state machine */
void nn_sofi_out_term (struct nn_sofi_out *self);

// /* ============================== */
// /*         INPUT EVENTS           */
// /* ============================== */

// /* There are data available for Rx */
// void nn_sofi_out_event__rx_data (struct nn_sofi_out *self, /* Data */);

// /* There was an Rx Error */
// void nn_sofi_out_event__rx_error (struct nn_sofi_out *self, /* Error */);


#endif
