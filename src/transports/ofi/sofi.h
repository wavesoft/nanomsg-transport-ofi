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

#ifndef NN_SOFI_INCLUDED
#define NN_SOFI_INCLUDED

#include "hlapi.h"
#include "sofi_in.h"
#include "sofi_out.h"

#include "../../transport.h"
#include "../../aio/fsm.h"
#include "../../aio/timer.h"
#include "../../aio/worker.h"
#include "../../utils/thread.h"
// #include "../../utils/mutex.h"

/* Outgoing FSM events towards FSM IN/OUT */
#define NN_SOFI_EVENT_RX_EVENT          1201
#define NN_SOFI_EVENT_RX_ERROR          1202
#define NN_SOFI_EVENT_TX_EVENT          1203
#define NN_SOFI_EVENT_TX_ERROR          1204

/* Outgoing FSM events towards parent */
#define NN_SOFI_STOPPED                 1210
#define NN_SOFI_DISCONNECTED            1211

/* Negotiation direction */
#define NN_SOFI_NG_NONE                 0
#define NN_SOFI_NG_SEND                 1
#define NN_SOFI_NG_RECV                 2

/* Shared, Connected OFI FSM */
struct nn_sofi {

    /*  The state machine. */
    struct nn_fsm               fsm;
    int                         state;
    int                         error;
    uint8_t                     ng_direction;

    /* This member can be used by owner to keep individual sofis in a list. */
    struct nn_list_item         item;

    /* Asynchronous communication with the worker thread */
    struct nn_worker            * worker;
    struct nn_worker_task       task_disconnect;

    /*  Pipe connecting this inproc connection to the nanomsg core. */
    struct nn_pipebase          pipebase;

    /* The high-level api structures */
    struct ofi_resources        * ofi;
    struct ofi_active_endpoint  * ep;

    /* Sub-components */
    struct nn_sofi_in           sofi_in;
    struct nn_sofi_out          sofi_out;

    /* Handshake timeout timer */
    struct nn_timer             timer_handshake;
    struct nn_timer             timer_keepalive;

};

/*  Initialize the state machine */
void nn_sofi_init (struct nn_sofi *self, 
    struct ofi_resources *ofi, struct ofi_active_endpoint *ep, const uint8_t ng_direction,
    struct nn_epbase *epbase, int src, struct nn_fsm *owner);

/* Check if FSM is idle */
int nn_sofi_isidle (struct nn_sofi *self);

/*  Stop the state machine */
void nn_sofi_stop (struct nn_sofi *sofi);

/*  Cleanup the state machine */
void nn_sofi_term (struct nn_sofi *sofi);

#endif
