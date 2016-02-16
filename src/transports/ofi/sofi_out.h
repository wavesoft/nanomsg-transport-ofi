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

/**
 * The [OUTPUT SOFI] FSM is used like this:
 *
 * - Initialize it using `nn_sofi_out_init`
 *
 * - Start it using `nn_sofi_out_start`
 *   + The FSM Will raise `NN_SOFI_OUT_EVENT_STARTED` when ready
 *
 * - Wait for CQ Events and call:
 *   + `nn_sofi_out_rx_event` when a Tx CQ event is completed
 *   + `nn_sofi_out_rx_error_event` when a Tx CQ error event occurs
 *
 * - When you want to send some data you should populate the `outmsg`
 *   property and then call `nn_sofi_out_tx_event_send`.
 *   + The FSM will rause `NN_SOFI_OUT_EVENT_SENT` when you can reuse your buffer 
 *
 * - You can force a shutdown by calling `nn_sofi_out_stop` function.
 *   The FSM will take care of cleanly shutting down everything and will
 *   raise either `NN_SOFI_OUT_EVENT_ERROR` or `NN_SOFI_OUT_EVENT_CLOSE`
 *
 * - If at any time the connection is cleanly closed, the FSM will raise
 *   the event `NN_SOFI_OUT_EVENT_CLOSE`.
 *
 * - If at any time an error occurs, the FSM will raise the event
 *   `NN_SOFI_OUT_EVENT_ERROR`, setting the `error` property accordingly.
 *
 */

/* Shared, Connected OFI FSM */
struct nn_sofi_out {

    /* The state machine. */
    struct nn_fsm               fsm;
    int                         state;
    int                         error;

    /* The outgoing message */
    struct nn_msg               outmsg;

    /* References */
    struct nn_pipebase          * pipebase;
    struct ofi_resources        * ofi;
    struct ofi_active_endpoint  * ep;

    /* Outgoing : Events */
    struct nn_fsm_event         event_started;
    struct nn_fsm_event         event_sent;

    /* Incoming : Events through worker tasks */
    struct nn_worker            * worker;
    struct nn_worker_task       task_tx;
    struct nn_worker_task       task_tx_error;
    struct nn_worker_task       task_tx_send;

    /* Abort cleanup timeout */
    struct nn_timer             timer_abort;

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

/* ============================== */
/*        EXTERNAL EVENTS         */
/* ============================== */

/* Trigger the transmission of a packet */
void nn_sofi_out_tx_event_send( struct nn_sofi_out *self );

/* Trigger an rx event */
void nn_sofi_out_tx_event( struct nn_sofi_out *self );

/* Trigger an rx erro event */
void nn_sofi_out_tx_error_event( struct nn_sofi_out *self, int err_number );

/** Synchronous (blocking) tx request */
size_t nn_sofi_out_tx( struct nn_sofi_out *self, void * ptr, size_t max_sz, int timeout );

#endif
