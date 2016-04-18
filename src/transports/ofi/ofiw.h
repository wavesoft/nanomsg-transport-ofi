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
    NN_OFIW_NONE,
    NN_OFIW_ITEM_EQ,
    NN_OFIW_ITEM_CQ,
};

/**
 * Pool of OFI Workers
 */
struct nn_ofiw_pool {

    /* === NanoMsg Specific === */

    /* List of workers */
    struct nn_list          workers;

    /* === libfabric Specific === */

    /* The fabric associated with this pool */
    struct fid_fabric       *fabric;

    /* === Local properties === */

    /* Thread reaper */
    struct nn_thread        reap_thread;

    /* List of items in the thread reaper */
    struct nn_list          reap_list;

    /* Reaper signal and acknowledgement */
    struct nn_efd           reap_do;

    /* Reaper mutex */
    struct nn_mutex         reap_mutex;

    /* State of the pool */
    uint8_t                 state;

};

/**
 * An item in the ofi worker waitset
 */
struct nn_ofiw_item {

    /* Myself as an item in the worker */
    struct nn_list_item     item;

    /* Parent worker */
    struct nn_ofiw          *worker;

    /* Pool poller thread */
    struct nn_thread        thread;

    /* State of the item */
    uint8_t                 state;

    /* Event source */
    int                     src;

    /* Pointer to the FD to test */
    void *                  fd;

    /* Memory for error CQ/EQ events */
    union { 
        struct fi_eq_err_entry  eq_err_entry;
        struct fi_cq_err_entry  cq_err_entry;
    } data_err;

    /* Memory for the CQ/EQ events */
    void                    *data;
    size_t                  data_size;

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

    /* Status flag */
    uint8_t                 active;

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
int nn_ofiw_add_cq( struct nn_ofiw * worker, struct fid_cq * cq, int cq_count,
    int src );

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

#endif