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

#include "hlapi.h"

#include "../../utils/list.h"
#include "../../utils/thread.h"
#include "../../utils/mutex.h"

/**
 * Turn on this flag if the libfabric provider you are using
 * supports waitsets in order to use the optimised version of the code.
 */
#define OFI_USE_WAITSET

/**
 * Mask overlaied over the item ID 
 */
#define NN_OFIW_COMPLETED   1
#define NN_OFIW_ERROR       2

/**
 * Enums of different polling types
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

    /* Operation mutex */
    struct nn_mutex         mutex;

    /* === libfabric Specific === */

    /* Pollset for waiting for events across various CQ/EQ */
    struct fid_poll         *pollset;

    /* The fabric associated with this pool */
    struct fid_fabric       *fabric;

    /* The EQ that passes signals to main thread */
    struct fid_eq           *eq;

    /* Context used by various operations */
    struct fi_context       context;

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
        struct fi_eq_cm_entry   eq_entry;
        struct fi_cq_err_entry  cq_err_entry;
        struct fi_cq_data_entry cq_entry;
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

/* Initialize an OFI worker pool */
int nn_ofiw_pool_init( struct nn_ofiw_pool * pool, struct fid_fabric *fabric );

/* Terminate an OFI worker pool */
int nn_ofiw_pool_term( struct nn_ofiw_pool * pool );


/* Get an OFI worker from the specified worker pool */
struct nn_ofiw * nn_ofiw_pool_getworker( struct nn_ofiw_pool * pool,
    struct nn_fsm * owner );

/* Close a worker */
void nn_ofiw_term( struct nn_ofiw * self );

/* Monitor the specified OFI Completion Queue, and trigger the specified type
   event to the handling FSM */
int nn_ofiw_add_cq( struct nn_ofiw * worker, struct fid_cq * cq, int src );

/* Monitor the specified OFI Completion Queue, and trigger the specified type
   event to the handling FSM */
int nn_ofiw_add_eq( struct nn_ofiw * worker, struct fid_eq * eq, int src );

/* Remove a particular file descriptor from the monitor */
int nn_ofiw_remove( struct nn_ofiw * worker, void * fd );

#endif