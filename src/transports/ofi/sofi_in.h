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
#include "utils/mrm.h"

#include "../../transport.h"
#include "../../aio/fsm.h"
#include "../../aio/timer.h"

/* FSM Events */
#define NN_SOFI_IN_EVENT_STARTED        2201
#define NN_SOFI_IN_EVENT_RECEIVED       2202
#define NN_SOFI_IN_EVENT_ERROR          2203
#define NN_SOFI_IN_EVENT_CLOSE          2204

/* MR Keys */
#define NN_SOFI_IN_MR_SMALL             0xF200
#define NN_SOFI_IN_MR_INPUT_BASE        0xF201

/**
 * The [INPUT SOFI] FSM is used like this:
 *
 * - Initialize it using `nn_sofi_in_init`
 *
 * - Start it using `nn_sofi_in_start`
 *   + The FSM Will raise `NN_SOFI_IN_EVENT_STARTED` when ready
 *
 * - Wait for CQ Events and call:
 *   + `nn_sofi_in_rx_event` when an Rx CQ event is completed
 *   + `nn_sofi_in_rx_error_event` when an Rx CQ error event occurs
 *
 * - The FSM will process the buffers, populate the `inmsg` property and
 *   + Will raise `NN_SOFI_IN_EVENT_RECEIVED` when a message is ready
 *
 * - When the buffer is processed you *MUST* call:
 *   + `nn_sofi_in_rx_event_ack` 
 *
 * - You can force a shutdown by calling `nn_sofi_in_stop` function.
 *   The FSM will take care of cleanly shutting down everything and will
 *   raise either `NN_SOFI_IN_EVENT_ERROR` or `NN_SOFI_IN_EVENT_CLOSE`
 *
 * - If at any time the connection is cleanly closed, the FSM will raise
 *   the event `NN_SOFI_IN_EVENT_CLOSE` 
 *
 * - If at any time an error occurs, the FSM will raise the event
 *   `NN_SOFI_IN_EVENT_ERROR`, setting the `error` property accordingly.
 *
 */

/* Incoming message chunk */
struct nn_sofi_in_chunk {

    /* The flags of this chunk */
    uint32_t flags;

    /* The nanomsg chunk for the body */
    void * chunk;

    /* The libfabric memory region */
    struct fid_mr * mr;

    /* This chunk can also be part of queue */
    struct nn_queue_item item;

    /* This chunk can also be used as a context to libfabric */
    struct fi_context context;

};

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

    /* Incoming : Events through worker tasks */
    struct nn_worker            * worker;
    struct nn_worker_task       task_rx;
    struct nn_worker_task       task_rx_error;
    struct nn_worker_task       task_rx_ack;

    /* Abort cleanup timeout */
    struct nn_timer             timer_abort;

    /* Buffers */
    struct ofi_mr               mr_small;
    struct nn_sofi_in_chunk     * mr_chunks;

    /* Ingress queue and pending item */
    struct nn_sofi_in_chunk *   chunk_ingress;
    struct nn_queue             queue_ingress;

    /* Buffer sizes  */
    int                         queue_size;
    size_t                      msg_size;

    /* Context for synchronous Rx operations */
    struct fi_context           context;

};

/* ============================== */
/*    CONSTRUCTOR / DESTRUCTOR    */
/* ============================== */

/*  Initialize the state machine */
void nn_sofi_in_init ( struct nn_sofi_in *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep,
    const uint8_t ng_direction, int queue_size, size_t msg_size,
    struct nn_pipebase * pipebase, int src, struct nn_fsm *owner );

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
void nn_sofi_in_rx_event( struct nn_sofi_in *self, 
    struct fi_cq_data_entry * cq_entry );

/* Trigger an rx erro event */
void nn_sofi_in_rx_error_event( struct nn_sofi_in *self, 
    struct fi_cq_err_entry * cq_err );

/* Acknowledge an rx event */
int nn_sofi_in_rx_event_ack( struct nn_sofi_in *self, struct nn_msg *msg );

/* Synchronous (blocking) rx request */
size_t nn_sofi_in_rx( struct nn_sofi_in *self, void * ptr, size_t max_sz, 
    int timeout );

#endif
