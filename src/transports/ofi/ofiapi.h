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

#ifndef NN_OFIAPI_INCLUDED
#define NN_OFIAPI_INCLUDED

#include "oficommon.h"
#include "ofiw.h"

#include "../../utils/list.h"
#include "../../utils/atomic.h"

/**
 * This class collects all the interface code between libfabric and nanomsg
 * in order to make it easy to re-use parts of this code into another
 * application or library. 
 *
 * In principle, the core nanomsg code should call out here and never call
 * the fi_* functions directly.
 */

/* ########################################################################## */
/*  Configurable Macros                                                       */
/* ########################################################################## */

/* LibFabric Protocol to use */
#define FT_FIVERSION    FI_VERSION(1,1)

/* Maximum size of domain specifications */
#define MAX_DOMAIN_LEN  255
#define MAX_SERVICE_LEN 16

/* ########################################################################## */
/*  Global Macros                                                             */
/* ########################################################################## */

/* Additional masks applied on the src attributes when using the workers
   in order to differentiate CQs and EQs */

#define OFI_SRC_EQ      0x10000000
#define OFI_SRC_CQ_RX   0x40000000
#define OFI_SRC_CQ_TX   0x20000000

/* ########################################################################## */
/*  Global Structures                                                         */
/* ########################################################################## */

/**
 * Global Resources
 */
struct ofi_resources
{

    /* Fabric core structures */
    struct fi_info *hints;

    /* Number of active fabrics */
    struct nn_list fabrics;

    /* Last error */
    int err;

};

/**
 * Per-Fabric Resources 
 */
struct ofi_fabric {

    /* The resources that created this fabric */
    struct ofi_resources *parent;

    /* The fabric this domain is hosted upon */
    struct fid_fabric *fabric;
    struct fi_info *fi;

    /* The worker pool linked to this fabric */
    struct nn_ofiw_pool pool;

    /* Reference counter */
    struct nn_atomic ref;

    /* List item */
    struct nn_list_item item;

    /* Number of active domains */
    struct nn_list domains;

};

/**
 * Per-Domain Resources
 */
struct ofi_domain {

    /* Fabric info used to identify the domain */
    struct fi_info *fi;

    /* The domain created */
    struct fid_domain *domain;

    /* The parent fabric */
    struct ofi_fabric *parent;

    /* List item */
    struct nn_list_item item;

};

/* ########################################################################## */
/*  Global Functions                                                          */
/* ########################################################################## */

/**
 * Allocate hints and prepare core structures
 */
int ofi_init( struct ofi_resources * R, enum fi_ep_type ep_type );

/**
 * Free hints and core structures
 */
int ofi_term( struct ofi_resources * R );

/* ########################################################################## */
/*  Fabric Functions                                                          */
/* ########################################################################## */

/**
 * Flags for oppening a new fabric
 */
enum ofi_fabric_addr_flags {
    /* We are creating a local fabric (ex. bind) */
    OFI_ADDR_LOCAL,
    /* We are creating a remote fabric (ex. connect) */
    OFI_ADDR_REMOTE,
};

/**
 * Open or re-use a fabric & domain for the specified address
 */
int ofi_fabric_open( struct ofi_resources * R, const char * address,
    enum ofi_fabric_addr_flags flags, struct ofi_fabric ** F );

/**
 * Close a fabric previously allocated with ofi_fabric_open
 */
int ofi_fabric_close( struct ofi_fabric * F );

/**
 * Get a worker from this fabric
 */
struct nn_ofiw * ofi_fabric_getworker( struct ofi_fabric * F,
    struct nn_fsm * owner );

/* ########################################################################## */
/*  Domain Functions                                                          */
/* ########################################################################## */

/**
 * Open or re-use the correct OFI domain for the fi_info specified. 
 * If `fi` is set to NULL, the fi_info used to resolve the fabric will be used.
 */
int ofi_domain_open( struct ofi_fabric * F, struct fi_info *fi,
    struct ofi_domain ** domain );

/**
 * Close a domain previously created with `ofi_domain_open`.
 */
int ofi_domain_close( struct ofi_domain * domain );

/* ########################################################################## */
/*  Passive Endpoint Functions                                                */
/* ########################################################################## */

/**
 * A passive endpoint
 */
struct ofi_passive_endpoint {

    /* The passive endpoint */
    struct fid_pep      *ep;

    /* The event queue on this passive endpoint */
    struct fid_eq       *eq;

    /* The worker the EQ/CQ is bound to, used when closing */
    struct nn_ofiw      *worker;

    /* The fabric this endpoint belongs to */
    struct ofi_fabric   *fabric;

};

/**
 * Open a passive endpoint and receive EQ events through the specified worker
 * The even types emmited from this source are the following:
 *
 * - FI_CONNREQ     : There was a connection request on this endpoint
 * - FI_SHUTDOWN    : The endpoint was shutdown for an unknown reason
 *
 */
int ofi_passive_endpoint_open( struct ofi_fabric * fabric, struct nn_ofiw * wrk,
    int src, void * context, struct ofi_passive_endpoint ** pep );

/**
 * Close a previously open endpoint
 */
int ofi_passive_endpoint_close( struct ofi_passive_endpoint * pep );

/* ########################################################################## */
/*  Active Endpoint Functions                                                 */
/* ########################################################################## */

/**
 * An active endpoint
 */
struct ofi_active_endpoint {

    /* End endpoint */
    struct fid_ep       *ep;

    /* The event queue */
    struct fid_eq       *eq;

    /* The completion queues */
    struct fid_cq       *cq_tx, *cq_rx;

    /* The worker the EQ/CQ is bound to, used when closing */
    struct nn_ofiw      *worker;

    /* The domain this endpoint belongs to */
    struct ofi_domain   *domain;

};

/**
 * Open an active endpoint and receive EQ/CQ events through the specified worker
 */
int ofi_active_endpoint_open( struct ofi_domain* domain, struct nn_ofiw* wrk,
    int src, void *context, struct fi_info *fi, struct ofi_active_endpoint** a);

/**
 * Close a previously open active endpoint
 */
int ofi_active_endpoint_close( struct ofi_active_endpoint * aep );

/* ########################################################################## */
/*  Connection Management Operations                                          */
/* ########################################################################## */

/**
 * Bring a passive endpoint in listening state. When a connection request is
 * sent, the the FI_CONNREQ event will be triggered by the associated worker.
 *
 * Note tha the source will be masked with OFI_SRC_EQ
 */
int ofi_cm_listen( struct ofi_passive_endpoint * ep );

/**
 * Accept an incoming request
 *
 * The active endpoint must be created using the `fi_info` that came as
 * part of the FI_CONNREQ EQ event. 
 *
 * You can optionally specify an payload that will be sent as part of the
 * handshake sequence to the other end. This data will be available as
 * part of the FI_CONNECTED EQ event.
 *
 * This will trigger a FI_CONNECTED event to the endpoint's EQ when the
 * connection is established.
 */
int ofi_cm_accept( struct ofi_active_endpoint * ep, const void *data, 
    size_t datalen );

/**
 * You can optionally specify an payload that will be sent as part of the
 * handshake sequence to the other end. This data will be available as
 * part of the FI_CONNECTED EQ event.
 */
int ofi_cm_connect( struct ofi_active_endpoint * ep, void *addr, 
    const void *data, size_t datalen );

/* ########################################################################## */
/*  Memory Management Operations                                              */
/* ########################################################################## */

/**
 * A chunk in the multi-buffered blocks
 */
struct ofi_io_buf_chunk {

    /* One or more buffers under management */
    struct iovec * iov;

    /* Memory region handler */
    struct fid_mr * mr;

};

/**
 * Transmission or reception double-buffered buffers
 */
struct ofi_io_buf {

    /* The two buffer chunks */
    struct ofi_io_buf_chunk[2];

    /* Which buffer is active */
    uint8_t active;

};

/**
 * Initialize the OFI Tx/Rx Buffer structures
 */
int ofi_io_buf_init( struct ofi_io_buf * buf );

/**
 * Terminate the OFI Tx/Rx Buffer structures
 */
int ofi_io_buf_term( struct ofi_io_buf * buf );

/* ########################################################################## */
/*  Data Transmission Operations                                              */
/* ########################################################################## */

/**
 * Perform send operation
 */
int ofi_send( struct ofi_active_endpoint * ep, struct ofi_io_buf * buf,
    struct iovec * iov );

#endif