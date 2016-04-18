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

#include <unistd.h>
#include <sched.h>

#include "oficommon.h"
#include "ofiw.h"
#include "ofi.h"

#include "../../utils/alloc.h"
#include "../../utils/cont.h"
#include "../../utils/err.h"
#include "../../utils/fast.h"
#include "../../aio/ctx.h"

#define NN_OFI_UNSIGNAL_NO      0
#define NN_OFI_UNSIGNAL_YES     1

#define NN_OFIW_STATE_INACTIVE  0
#define NN_OFIW_STATE_ACTIVE    1

/* ============================== */
/*       HELPER FUNCTIONS         */
/* ============================== */

/**
 * An item in the reaper thread
 */
struct nn_ofiw_reap_item {

    /* Item on list */
    struct nn_list_item item;

    /* Pointer to reap */
    struct nn_thread * ptr_thread;

    /* Pointer to free */
    struct nn_thread * ptr_free;

};

/**
 * Reap all pending items on list
 */
static void nn_ofiw_reap_list( struct nn_ofiw_pool * self, uint8_t unsignal )
{
    struct nn_ofiw_reap_item *item;
    struct nn_list_item *it;

    /* Enter reap context */
    nn_mutex_lock( &self->reap_mutex );
    _ofi_debug("OFI[w]: Thread reaper: Got signal\n");

    /* Reap all items in list */
    while ((it=nn_list_begin(&self->reap_list)) != nn_list_end (&self->reap_list)) {
        item = nn_cont (it, struct nn_ofiw_reap_item, item);

        /* Remove item from list */
        nn_list_erase( &self->reap_list, &item->item );
        nn_list_item_term( &item->item );

        /* Reap/free */
        if (item->ptr_thread) {
            nn_thread_term( item->ptr_thread );
            _ofi_debug("OFI[w]: Thread reaper: Reap thread=%p\n", item->ptr_thread);
        }
        if (item->ptr_free) {
            nn_free( item->ptr_free );
            _ofi_debug("OFI[w]: Thread reaper: Reap free=%p\n", item->ptr_free);
        }

        /* Free item */
        nn_free( item );

    }

    /* We drained the reap_list, unsignal */
    if (unsignal) {
        nn_efd_unsignal( &self->reap_do );
        _ofi_debug("OFI[w]: Thread reaper: Resetting signal\n");
    }

    /* leave reap context */
    nn_mutex_unlock( &self->reap_mutex );

}

/**
 * A thread that takes care of reaping the
 * CQ/EQ threads
 */
static void nn_ofiw_thread_reaper( void * dat )
{
    struct nn_ofiw_pool * self  = dat;

    while (self->state == NN_OFIW_STATE_ACTIVE) {

        /* Wait for reap signal */
        nn_efd_wait( &self->reap_do, -1 );

        /* Reap list & unsignal efd */
        nn_ofiw_reap_list( self, NN_OFI_UNSIGNAL_YES );

    }
}

/**
 * Helper function to reap a thread outside current scope
 */
static void nn_ofiw_pool_reap( struct nn_ofiw_pool * self, struct nn_thread * t,
    void * f )
{

    /* Allocate new reap item structure */
    struct nn_ofiw_reap_item * reap = nn_alloc( sizeof(struct nn_ofiw_reap_item), 
        "reap item" );

    /* Initialize */
    reap->ptr_thread = t;
    reap->ptr_free = f;
    nn_list_item_init( &reap->item );

    /* Acquire lock */
    nn_mutex_lock( &self->reap_mutex );

    /* If that's the first item, signal */
    if (nn_list_begin(&self->reap_list) != nn_list_end (&self->reap_list)) {
        _ofi_debug("OFI[w]: Placing thread reap request\n");
        nn_efd_signal( &self->reap_do );
    }

    /* Add item on list */
    nn_list_insert (&self->reap_list, &reap->item,
        nn_list_end (&self->reap_list));

    /* Free lock */
    nn_mutex_unlock( &self->reap_mutex );

}

/**
 * Polling thread of an OFI CQ item
 */
static void nn_ofiw_thread_cq( void * dat )
{
    int i;
    int ret;
    struct nn_ofiw_item * self = dat;
    struct nn_ofiw * worker = self->worker;

    /* Start poller */
    _ofi_debug("OFI[w]: CQ Thread: Started\n");
    while (nn_fast( self->state == NN_OFIW_STATE_ACTIVE)) {

        /* Read completion queue */
        ret = fi_cq_sread( (struct fid_cq *)self->fd,
            self->data, self->data_size, NULL, 1000);
        if (nn_slow(ret > 0)) {

            _ofi_debug("OFI[w]: Got %i CQ Event(s) from src=%i,"
               " worker=%p, fd=%p\n",ret,self->src,worker,self);

            /* Feed event(s) to the FSM */
            nn_ctx_enter (worker->owner->ctx);
            for (i=0; i<ret; i++) {
                nn_fsm_feed (worker->owner, 
                    self->src,
                    NN_OFIW_COMPLETED,
                    &((struct fi_cq_msg_entry *)self->data)[i]
                );
            }
            nn_ctx_leave (worker->owner->ctx);

        } else if (nn_slow(ret != -FI_EAGAIN)) {

            /* Get error details */
            ret = fi_cq_readerr( (struct fid_cq *)self->fd,
                &self->data_err.cq_err_entry, 0);

            _ofi_debug("OFI[w]: Got CQ Error from src=%i, worker=%p, fd=%p\n",
                self->src, worker, self);

            /* Feed event to the FSM */
            nn_ctx_enter (worker->owner->ctx);
            nn_fsm_feed (worker->owner, 
                self->src,
                NN_OFIW_ERROR,
                &self->data_err.cq_err_entry
            );
            nn_ctx_leave (worker->owner->ctx);

        }

    }
    _ofi_debug("OFI[w]: CQ Thread: Exited\n");
}

/**
 * Polling thread of an OFI EQ item
 */
static void nn_ofiw_thread_eq( void * dat )
{
    int ret;
    ssize_t sret;
    uint32_t event = 0;
    struct nn_ofiw_item * self = dat;
    struct nn_ofiw * worker = self->worker;

    /* Start poller */
    _ofi_debug("OFI[w]: EQ Thread: Started\n");
    while (nn_fast( self->state == NN_OFIW_STATE_ACTIVE)) {

        /* Read event queue */
        ret = fi_eq_sread( (struct fid_eq *)self->fd, 
            &event, self->data, self->data_size, 1000, 0);
        if (nn_slow(ret != -FI_EAGAIN)) {

            if (nn_slow( ret == -FI_EAVAIL )) {

                sret = fi_eq_readerr( (struct fid_eq *)self->fd,
                    &self->data_err.eq_err_entry, 0);
                if (nn_slow( sret != sizeof(struct fi_eq_err_entry) )) {
                    FT_PRINTERR("fi_eq_readerr", sret);
                    break;
                }

                _ofi_debug("OFI[w]: Got EQ Error Event from "
                    "src=%i, worker=%p, fd=%p, error=%i\n",
                    self->src, worker, self,
                    self->data_err.eq_err_entry.err);

                /* Feed event to the FSM */
                nn_ctx_enter (worker->owner->ctx);
                nn_fsm_feed (worker->owner, 
                    self->src,
                    -self->data_err.eq_err_entry.err,
                    &self->data_err.eq_err_entry
                );
                nn_ctx_leave (worker->owner->ctx);

            } else {

                _ofi_debug("OFI[w]: Got EQ Event from src=%i, "
                    "worker=%p, fd=%p, event=%i\n",
                    self->src, worker, self, event);

                /* Feed event to the FSM */
                nn_ctx_enter (worker->owner->ctx);
                nn_fsm_feed (worker->owner, 
                    self->src,
                    event,
                    self->data
                );
                nn_ctx_leave (worker->owner->ctx);

            }

        }

    }
    _ofi_debug("OFI[w]: EQ Thread: Exited\n");
}


/**
 * Reap an ofiw item
 */
static void nn_ofiw_term_item( struct nn_ofiw_item * item )
{
    if (item->state == NN_OFIW_STATE_ACTIVE) {

        /* Deactivate thread */
        item->state = NN_OFIW_STATE_INACTIVE;

        /* Reap it */
        nn_ofiw_pool_reap( item->worker->parent, &item->thread, item );

    }
}


/* ============================== */
/*      INTERFACE FUNCTIONS       */
/* ============================== */

/* Initialize an OFI worker pool */
int nn_ofiw_pool_init( struct nn_ofiw_pool * self, struct fid_fabric *fabric )
{
    int ret;

    /* Initialize properties */
    self->fabric = fabric;
    self->state = NN_OFIW_STATE_ACTIVE;

    /* Initialize structures */
    nn_list_init( &self->workers );

    /* Initialize thread reaper */
    nn_list_init( &self->reap_list );
    nn_efd_init( &self->reap_do );
    nn_mutex_init( &self->reap_mutex );
    nn_thread_init( &self->reap_thread, &nn_ofiw_thread_reaper, self );

    /* Success */
    return 0;
}

/* Terminate an OFI worker pool */
int nn_ofiw_pool_term( struct nn_ofiw_pool * self )
{
    struct nn_ofiw *worker;
    struct nn_list_item *it;

    /* Deny requests to get a worker */
    self->state = NN_OFIW_STATE_INACTIVE;

    /* Stop all workers */
    while ((it=nn_list_begin(&self->workers)) != nn_list_end (&self->workers)) {
        worker = nn_cont (it, struct nn_ofiw, item);

        /* Terminate worker (this also removes the item from list) */
        nn_ofiw_term( worker );

    }

    /* Free list */
    nn_list_term( &self->workers );

    /* Reap reaping thread & manually reap possibly pending items */
    nn_thread_term( &self->reap_thread );
    nn_ofiw_reap_list( self, NN_OFI_UNSIGNAL_NO );

    /* Free reaper resources */
    nn_efd_term( &self->reap_do );
    nn_mutex_term( &self->reap_mutex );

    /* Success */
    return 0;
}

/* Get an OFI worker from the specified worker pool */
struct nn_ofiw * nn_ofiw_pool_getworker( struct nn_ofiw_pool * self,
    struct nn_fsm * owner )
{
    nn_assert( self->state == NN_OFIW_STATE_ACTIVE );
    struct nn_ofiw * worker;

    /* Allocate a new worker */
    worker = nn_alloc( sizeof(struct nn_ofiw), "OFI worker" );
    nn_assert( worker );

    /* Initialize */
    worker->active = NN_OFIW_STATE_ACTIVE;
    worker->owner = owner;
    worker->parent = self;
    nn_list_init( &worker->items );

    /* Add to list */
    nn_list_item_init( &worker->item );
    nn_list_insert (&self->workers, &worker->item,
        nn_list_end (&self->workers));

    /* Return worker */
    _ofi_debug("OFI[w]: Allocated new worker %p\n", worker);
    return worker;
}

/* Terminate a worker */
void nn_ofiw_term( struct nn_ofiw * self )
{
    struct nn_ofiw_item *item;
    struct nn_list_item *it;

    /* Remove from worker list */
    nn_list_erase( &self->parent->workers, &self->item );
    nn_list_item_term( &self->item );

    /* Terminate all items */
    while ((it = nn_list_begin (&self->items)) != nn_list_end (&self->items)) {
        item = nn_cont (it, struct nn_ofiw_item, item);

        /* Free structures */
        nn_list_erase( &self->items, &item->item );
        nn_list_item_term( &item->item );
        nn_free( item->data );

        /* Terminate (and free) item */
        nn_ofiw_term_item( item );

    }

    /* Terminate structures */
    nn_list_term( &self->items );

    /* Free worker */
    nn_free( self );
}

/* Monitor the specified OFI Completion Queue, and trigger the specified type
   event to the handling FSM */
int nn_ofiw_add_cq( struct nn_ofiw * self, struct fid_cq * cq, int cq_count, 
    int src )
{
    /* Allocate new item */
    struct nn_ofiw_item * item = nn_alloc( sizeof(struct nn_ofiw_item), 
        "ofiw item");
    nn_assert(item);

    /* Prepare */
    item->worker = self;
    item->src = src;
    item->fd = cq;
    item->data = nn_alloc( sizeof(struct fi_cq_msg_entry) * cq_count, 
                            "ofiw item data");
    item->data_size = cq_count;

    /* Put item on list queue */
    nn_list_item_init( &item->item );
    nn_list_insert (&self->items, &item->item,
        nn_list_end (&self->items));

    /* Start polling thread */
    item->state = NN_OFIW_STATE_ACTIVE;
    nn_thread_init( &item->thread, &nn_ofiw_thread_cq, item );

    _ofi_debug("OFI[w]: Added CQ fd=%p on worker=%p\n", item, self);

    /* Success */
    return 0;
}

/* Return the oppened CQ/EQ wait set */
struct fid_wait * nn_ofiw_waitset( struct nn_ofiw * worker )
{
    /* We are not using waitsets */
    return NULL;
}

/* Monitor the specified OFI Event Queue, and trigger the specified type
   event to the handling FSM */
int nn_ofiw_add_eq( struct nn_ofiw * self, struct fid_eq * eq, int src )
{
    /* Allocate new item */
    struct nn_ofiw_item * item = nn_alloc( sizeof(struct nn_ofiw_item), 
        "ofiw item");
    nn_assert(item);

    /* Prepare */
    item->worker = self;
    item->src = src;
    item->fd = eq;
    item->data = nn_alloc( sizeof(struct fi_eq_cm_entry), "ofiw item data" );
    item->data_size = sizeof(struct fi_eq_cm_entry);

    /* Put item on list queue */
    nn_list_item_init( &item->item );
    nn_list_insert (&self->items, &item->item,
        nn_list_end (&self->items));

    /* Start polling thread */
    item->state = NN_OFIW_STATE_ACTIVE;
    nn_thread_init( &item->thread, &nn_ofiw_thread_eq, item );

    _ofi_debug("OFI[w]: Added EQ fd=%p on worker=%p\n", item, self);

    /* Success */
    return 0;
}

/* Remove the specified file descriptor from the worker stack */
int nn_ofiw_remove( struct nn_ofiw * self, void * fd )
{
    struct nn_ofiw_item *item;
    struct nn_list_item *it;

    /* Lookup specified item */
    for (it = nn_list_begin (&self->items);
          it != nn_list_end (&self->items);
          it = nn_list_next (&self->items, it)) {
        item = nn_cont (it, struct nn_ofiw_item, item);

        /* Check for matching fd */
        if (item->fd == fd) {

            _ofi_debug("OFI[w]: Removed fd=%p from worker=%p\n", item, self);

            /* Free structures */
            nn_list_erase( &self->items, &item->item );
            nn_list_item_term( &item->item );
            nn_free( item->data );

            /* Terminate (and free) item */
            nn_ofiw_term_item( item );
            
            /* Break */
            return 0;
        }

    }

    /* Not found */
    return -ENOENT;
}