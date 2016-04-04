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

#ifndef nn_ofiw_INCLUDED
#define nn_ofiw_INCLUDED

#include "ofi.h"
#include "oficommon.h"

#include "../../utils/list.h"
#include "../../utils/thread.h"
#include "../../utils/mutex.h"
#include "../../utils/efd.h"

/**
 * This file exposes the OFI Worker Class, which is simmilar to NanoMsg's native
 * worker classes, but it operates over libfabric waitable objects, such as Event
 * and Completion Queues.
 * 
 * It starts only one thread upon calling `nn_ofiw_pool_init` and it allocates 
 * one or more poller workers when calling `nn_ofiw_pool_getworker`. Each worker 
 * can support an arbitrary number of CQs and EQs, that should be created using 
 * the wrapper functions: `nn_ofiw_open_eq` and `nn_ofiw_open_cq`.
 *
 * When the latter functions are used, the CQ/EQ will be created with a waitset
 * configured, that is used to receive events in the most optimal way.
 *
 */

/**
 * Mask overlaied over the item ID 
 */
#define NN_OFIW_COMPLETED   1
#define NN_OFIW_ERROR       2

/**
 * Enums of different polling items
 */
enum nn_ofiw_item_type {
    NN_OFIW_ITEM_EQ,
    NN_OFIW_ITEM_CQ,
};

/**
 * Pool of OFI Workers
 */
struct nn_ofiw_pool {

    /* === NanoMsg Specific === */

    /* Pool poller thread */
    struct nn_thread        thread;

    /* List of workers */
    struct nn_list          workers;

    /* === libfabric Specific === */

    /* Pollset for waiting for events across various CQ/EQ */
    struct fid_poll         *pollset;

    /* The fabric associated with this pool */
    struct fid_fabric       *fabric;

    /* The EQ that passes signals to main thread */
    struct fid_eq           *eq;

    /* Context used by various operations */
    struct fi_context       context;

#ifdef OFI_USE_WAITSET

    /* Global waitset */
    struct fid_wait         *waitset;

#endif

    /* === Thread lock helpers === */

    /* Lock state flag */
    uint8_t                 lock_state;

    /* Lock mutex */
    struct nn_mutex         lock_mutex;

    /* Efd to place lock request */
    struct nn_efd           efd_lock_req;

    /* Efd to place wait lock termination */
    struct nn_efd           efd_lock_ack;

    /* === Local variables === */

    /* Flag that allows termination of the thread */
    uint8_t                 active;

};

/**
 * An item in the ofi worker waitset
 */
struct nn_ofiw_item {

    /* Myself as an item in the worker */
    struct nn_list_item     item;

    /* Polling function to use */
    uint8_t                 fd_type;

    /* Pointer to the FD to test */
    void *                  fd;

    /* Event source */
    int                     src;

    /* Memory for temporary storing the CQ details
       until they are handled by the FSM */
    union { 
        struct fi_eq_entry      eq_entry;
        struct fi_eq_cm_entry   eq_cm_entry;
        struct fi_eq_err_entry  eq_err_entry;
        struct fi_cq_data_entry cq_entry;
        struct fi_cq_err_entry  cq_err_entry;
    } data;

};

/**
 * An OFI worker that listens for events
 */
struct nn_ofiw {

    /* === NanoMsg Specific === */

    /* Myself as an item in the worker pool */
    struct nn_list_item     item;
    
    /* List of items to monitor */
    struct nn_list          items;

    /* The owner FSM */
    struct nn_fsm           *owner;

    /* === Local variables === */

    /* The parent pool */
    struct nn_ofiw_pool     *parent;

};

/* ####################################### */

/* Initialize an OFI worker pool */
int nn_ofiw_pool_init( struct nn_ofiw_pool * pool, struct fid_fabric *fabric );

/* Terminate an OFI worker pool */
int nn_ofiw_pool_term( struct nn_ofiw_pool * pool );

/* ####################################### */

/* Get an OFI worker from the specified worker pool */
struct nn_ofiw * nn_ofiw_pool_getworker( struct nn_ofiw_pool * pool,
    struct nn_fsm * owner );

/* Close a worker */
void nn_ofiw_term( struct nn_ofiw * self );

/* ####################################### */

/* Monitor the specified OFI Completion Queue, and trigger the specified type
   event to the handling FSM */
int nn_ofiw_add_cq( struct nn_ofiw * worker, struct fid_cq * cq, int src );

/* Monitor the specified OFI Completion Queue, and trigger the specified type
   event to the handling FSM */
int nn_ofiw_add_eq( struct nn_ofiw * worker, struct fid_eq * eq, int src );

/* Wrapping function to fi_eq_open that properly registers the resulting EQ
   in the worker. This function might update EQ attributes accordingly in order
   to configure waitsets. */
int nn_ofiw_open_eq( struct nn_ofiw * self, int src, void *context, 
    struct fi_eq_attr *attr, struct fid_eq **eq );

/* Wrapping function to fi_cq_open that properly registers the resulting CQ
   in the worker. This function might update CQ attributes accordingly in order
   to configure waitsets. */
int nn_ofiw_open_cq( struct nn_ofiw * self, int src, struct fid_domain *domain,
    void *context, struct fi_cq_attr *attr, struct fid_cq **cq );

/* Remove a particular file descriptor from the monitor */
int nn_ofiw_remove( struct nn_ofiw * worker, void * fd );

/* Block/Unblock OFIW for thread-safe operations */
int nn_ofiw_block( struct nn_ofiw * worker );
int nn_ofiw_unblock( struct nn_ofiw * worker );

#endif