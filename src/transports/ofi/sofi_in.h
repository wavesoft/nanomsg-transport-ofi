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

#ifndef NN_SOFI_IN_INCLUDED
#define NN_SOFI_IN_INCLUDED

#include "hlapi.h"
#include "ofi.h"

#include "../../transport.h"
#include "../../aio/fsm.h"
#include "../../aio/timer.h"

/* FSM Events */
#define NN_SOFI_IN_EVENT_STARTED        2201
#define NN_SOFI_IN_EVENT_RECEIVED       2202
#define NN_SOFI_IN_EVENT_ERROR          2203
#define NN_SOFI_IN_EVENT_CLOSE          2204

/* Shared, Connected OFI FSM */
struct nn_sofi_in {

    /* The state machine. */
    struct nn_fsm               fsm;
    int                         state;
    int                         error;

    /* References */
    struct nn_pipebase          * pipebase;
    struct ofi_resources        * ofi;
    struct ofi_active_endpoint  * ep;

    /* Outgoing : Events */
    struct nn_fsm_event         event_started;
    struct nn_fsm_event         event_received;
    struct nn_fsm_event         event_error;
    struct nn_fsm_event         event_close;

    /* Incoming : Events through worker tasks */
    struct nn_worker            * worker;
    struct nn_worker_task       task_rx;
    struct nn_worker_task       task_rx_error;
    struct nn_worker_task       task_rx_ack;

    /* Abort cleanup timeout */
    struct nn_timer             timer_abort;

};

/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/*  Initialize the state machine */
void nn_sofi_in_init ( struct nn_sofi_in *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep,
    const uint8_t ng_direction, struct nn_pipebase * pipebase,
    int src, struct nn_fsm *owner );

/* Check if FSM is idle */
int nn_sofi_in_isidle (struct nn_sofi_in *self);

/*  Start the state machine */
void nn_sofi_in_start (struct nn_sofi_in *self);

/*  Stop the state machine */
void nn_sofi_in_stop (struct nn_sofi_in *self);

/*  Cleanup the state machine */
void nn_sofi_in_term (struct nn_sofi_in *self);

/* ============================== */
/*        EXTERNAL EVENTS         */
/* ============================== */

/* Trigger an rx event */
void nn_sofi_in_rx_event( struct nn_sofi_in *self );

/* Trigger an rx erro event */
void nn_sofi_in_rx_error_event( struct nn_sofi_in *self, int err_number );

/** Acknowledge an rx event */
void nn_sofi_in_rx_error_ack( struct nn_sofi_in *self );

#endif
