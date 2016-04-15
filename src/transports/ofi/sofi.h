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

#include "ofi.h"
#include "ofiapi.h"

#include "../../transport.h"
#include "../../aio/fsm.h"
#include "../../aio/timer.h"
#include "../../aio/worker.h"
#include "../../utils/thread.h"
#include "../../utils/efd.h"
#include "../../utils/mutex.h"

/* Outgoing FSM events towards FSM IN/OUT */
#define NN_SOFI_EVENT_RX_EVENT          1201
#define NN_SOFI_EVENT_RX_ERROR          1202
#define NN_SOFI_EVENT_TX_EVENT          1203
#define NN_SOFI_EVENT_TX_ERROR          1204

/* Outgoing FSM events towards parent */
#define NN_SOFI_STOPPED                 1210

/* Negotiation direction */
#define NN_SOFI_NG_NONE                 0
#define NN_SOFI_NG_SEND                 1
#define NN_SOFI_NG_RECV                 2

/* The payload of the keepalive message, trying to be
   quite diverse in order not to be matched by any other protocol header */
#define NN_SOFI_KEEPALIVE_PACKET_LEN    24
#define NN_SOFI_KEEPALIVE_PACKET        "\xff\xff\xff\xff\xff\xff\xff\xff" \
                                        "\xce\x9a\x11\x7e\xce\x9a\x11\x7e" \
                                        "\xff\xff\xff\xff\xff\xff\xff\xff"

/* The size of the local buffer for ancillary data I/O */
#define NN_SOFI_ANCILLARY_SIZE          4096

/* Handshake information */ 
struct nn_sofi_handshake
{

    /* Protocol version */
    uint32_t                    version;

};

/* Structure that keeps track of ingress messages */
struct nn_sofi_in_buf {

    /* Body chunk */
    void * chunk;

    /* Underlying message */
    struct nn_msg msg;

    /* Memory registration hint */
    struct fid_mr *mr;

    /* MR Descriptor */
    void * mr_desc[1];

    /* This structure acts as a libfabric context */
    struct fi_context context;

    /* Linked list pointers */
    struct nn_sofi_in_buf * next;
    struct nn_sofi_in_buf * prev;

};

/* Structure that keeps track of egress messages */
struct nn_sofi_out_ctx {

    /* The message in transit */
    struct nn_msg msg;

    /* Pointer to SOFI */
    struct nn_sofi * sofi;

    /* The MRM handle */
    void * mr_handle;

    /* This structure acts as a libfabric context */
    struct fi_context context;  

    /* Linked list pointers */
    struct nn_sofi_out_ctx * next;
    struct nn_sofi_out_ctx * prev;

};

/* Shared, Connected OFI FSM */
struct nn_sofi {

    /*  The state machine. */
    struct nn_fsm               fsm;
    struct nn_epbase            *epbase;
    int                         state;
    int                         socket_state;
    int                         error;
    int                         offset;

    /* This member can be used by owner to keep individual sofis in a list. */
    struct nn_list_item         item;

    /*  Pipe connecting this inproc connection to the nanomsg core. */
    struct nn_pipebase          pipebase;

    /* The high-level api structures */
    struct ofi_domain           *domain;
    struct ofi_mr_manager       mrm_egress;
    struct ofi_active_endpoint  *ep;
    struct nn_ofiw              *worker;

    /* Shutdown timer */
    struct nn_timer             timer_shutdown;

    /* Keepalive mechanism */
    struct nn_timer             timer_keepalive;
    uint8_t                     ticks_in;
    uint8_t                     ticks_out;

    /* Ancillary buffer */
    uint8_t                     aux_buf[NN_SOFI_ANCILLARY_SIZE];
    struct fi_context           aux_context;
    struct fid_mr               *aux_mr;

    /* Egress properties */
    struct nn_msg               outmsg;
    struct nn_atomic            stageout_counter;
    uint8_t                     stageout_state;
    uint8_t                     out_state;
    int                         egress_max;
    struct nn_sofi_out_ctx      *egress_contexts;
    struct nn_sofi_out_ctx      *egress_ctx_head;
    struct nn_sofi_out_ctx      *egress_ctx_tail;

    /* Ingress properties */
    struct nn_sofi_in_buf       *ingress_buffers;
    struct nn_sofi_in_buf       *ingress_buf_free;
    struct nn_sofi_in_buf       *ingress_buf_busy;
    struct nn_sofi_in_buf       *ingress_buf_pop_head;
    struct nn_sofi_in_buf       *ingress_buf_pop_tail;
    int                         ingress_max;
    size_t                      ingress_buf_size;
    uint8_t                     ingress_flags;

    /* Local and remote handshake information */
    struct nn_sofi_handshake    hs_local;
    struct nn_sofi_handshake    hs_remote;
#ifndef OFI_DISABLE_HANDSHAKE
    int                         hs_state;
#endif

};

/*  Initialize the state machine */
int nn_sofi_init (struct nn_sofi *self, struct ofi_domain *domain, int offset,
    struct nn_epbase *epbase, int src, struct nn_fsm *owner);

/* Start the state machine either in receiving or sending side */
int nn_sofi_start_accept( struct nn_sofi *self, struct fi_eq_cm_entry * conreq );
int nn_sofi_start_connect( struct nn_sofi *self );

/* Check if FSM is idle */
int nn_sofi_isidle (struct nn_sofi *self);

/*  Stop the state machine */
void nn_sofi_stop (struct nn_sofi *sofi);

/*  Cleanup the state machine */
void nn_sofi_term (struct nn_sofi *sofi);

#endif
