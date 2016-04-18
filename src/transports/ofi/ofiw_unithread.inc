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

#define NN_OFIW_STATE_INACTIVE  0
#define NN_OFIW_STATE_ACTIVE    1

/**
 * Macro to test if a value changes across repeated checks
 * in order to make atomic updates safe. 
 *
 * Effectively we are trying to delay enough for a read, increment
 * and write operation sequence to complete.
 */
#define ATOMIC_SAFE(x) ( x || x || x || x || x || x || x || x )

/* ============================== */
/*       HELPER FUNCTIONS         */
/* ============================== */

/**
 * Add an item on the ofi
 */
static void nn_ofiw_pool_add_item( struct nn_ofiw_pool * self,
    struct nn_ofiw_item * item ) 
{

    /* Acquire lock & notify thread to use it */
    nn_mutex_lock( &self->list_mutex );
    self->list_mutex_id++;

    /* Insert item on list */
    nn_list_insert (&self->items, &item->item,
        nn_list_end (&self->items));

    /* Release mutex */
    nn_mutex_unlock( &self->list_mutex );
}

/**
 * Add an item on the ofi
 */
static void nn_ofiw_pool_del_item( struct nn_ofiw_pool * self,
    struct nn_ofiw_item * item ) 
{

    /* Acquire lock & notify thread to use it */
    nn_mutex_lock( &self->list_mutex );
    self->list_mutex_id++;

    /* Erase item from list */
    nn_list_erase (&self->items, &item->item);

    /* Release mutex */
    nn_mutex_unlock( &self->list_mutex );
}

/**
 * Terminate item
 */
static void nn_ofiw_pool_term_item( struct nn_ofiw_item * item )
{

    /* Free item data */
    nn_list_item_term( &item->item );
    nn_free( item->data );
    nn_free( item );

}

/**
 * Calculate how many kilo-cycles we can run per millisedon
 */
static uint32_t nn_ofiw_kinstr_per_ms()
{
    struct timespec a, b;
    uint64_t kinst_ms;

    /* Run one million actions and count how much time it takes */
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i=0; i<1000000; i++) {
        /* Do some moderate heap alloc/math operations */
        volatile int v = 0;
        v = v + 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);

    /* Count kinst_ms spent */
    kinst_ms = 1000000000 / (b.tv_nsec - a.tv_nsec);

    /* Wrap to maximum 32-bit */
    if (kinst_ms > 4294967295) {
        return 4294967295;
    }

    /* Return at least one */
    if (kinst_ms == 0) {
        return 1;
    }

    /* Return */
    return (uint32_t) kinst_ms;
}

/**
 * Main polling thread
 */
static void nn_ofiw_polling_thread( void * data )
{

    struct nn_list_item *it, *jt;
    struct nn_ofiw_pool * self = data;
    struct nn_ofiw_item * item;
    struct nn_ofiw * worker;
    uint8_t unlock_mutex = 0;
    uint8_t last_id = self->list_mutex_id;

    uint32_t event;
    ssize_t sret;
    int ret;
    int i;

#if !defined OFI_USE_WAITSET

    /* Spinwait implementation using pre-calculated
       time of instructions per millisecond */
    uint32_t spinwait;
    uint32_t kinstr;
    spinwait = self->kinst_per_ms;
    kinstr = 1000; /* 1ms of spinlock */

#else

    /* A flag used to try more than one events after a waitset
       signal in order to drain all of them. */
    uint8_t triggered;

#endif

    /* Start event polling loop */
    while (self->state == NN_OFIW_STATE_ACTIVE) {


#if defined OFI_USE_WAITSET

        /* If waitsets are available, we have an optimized way for
           waiting for an event across all of our objects. */
        ret = fi_wait( self->waitset, -1 );
        if (nn_slow( (ret < 0) && (ret != -FI_EAGAIN) )) {
            FT_PRINTERR("fi_wait", ret);
            break;
        }

#endif

        /* Mutex lock/unlock costs, therefore we use a faster
           alternative to decide when to lock */
        if (ATOMIC_SAFE( last_id != self->list_mutex_id )) {
            last_id = self->list_mutex_id;
            nn_mutex_lock( &self->list_mutex );
            unlock_mutex = 1;
        }

#if defined OFI_USE_WAITSET

        /* When using waitset keep reading until nothing else is triggering */
        do {

            /* Reset triggered flag */
            triggered = 0;

#endif

        /* Iterate over pollable items */
        for (it = nn_list_begin (&self->items);
              it != nn_list_end (&self->items);
              it = nn_list_next (&self->items, it)) {

            /* Get item and worker */
            item = nn_cont (it, struct nn_ofiw_item, item);
            worker = item->worker;

            /* Handle according to type */
            switch (item->fd_type) {

                /* COMPLETION QUEUE MODE */
                case NN_OFIW_ITEM_CQ:

                    /* Read completion queue */
                    ret = fi_cq_read( (struct fid_cq *)item->fd,
                        item->data, item->data_size);
                    if (nn_slow(ret > 0)) {

                        /* Log */
                        _ofi_debug("OFI[w]: Got %i CQ Event(s) from src=%i, "
                          "worker=%p, fd=%p\n", ret, item->src, worker, item);

                        /* Unlock mutex */
                        if (unlock_mutex) {
                            nn_mutex_unlock( &self->list_mutex );
                            unlock_mutex = 0;
                        }

                        #if !defined OFI_USE_WAITSET
                            /* Reset spinwait timer when we have an event in order
                               to capture the next even faster. */
                            spinwait = self->kinst_per_ms;
                        #else
                            /* Mark as triggered */
                            triggered = 1;
                        #endif

                        /* Forward to FSM */
                        nn_ctx_enter (worker->owner->ctx);
                        for (i=0; i<ret; i++) {

                            /* Send to FSM */
                            nn_fsm_feed (worker->owner, 
                                item->src,
                                NN_OFIW_COMPLETED,
                                &((struct fi_cq_msg_entry *)item->data)[i]
                            );
                        }
                        nn_ctx_leave (worker->owner->ctx);

                    } else if (nn_slow(ret != -FI_EAGAIN)) {

                        /* Get error details */
                        ret = fi_cq_readerr( (struct fid_cq *)item->fd,
                            &item->data_err.cq_err_entry, 0);

                        /* Log */
                        _ofi_debug("OFI[w]: Got CQ Error from src=%i, worker=%p"
                            ", fd=%p\n", item->src, worker, item);

                        /* Unlock mutex */
                        if (unlock_mutex) {
                            nn_mutex_unlock( &self->list_mutex );
                            unlock_mutex = 0;
                        }

                        #if defined OFI_USE_WAITSET
                            /* Mark as triggered */
                            triggered = 1;
                        #endif

                        /* Forward to FSM */
                        nn_ctx_enter (worker->owner->ctx);
                        nn_fsm_feed (worker->owner, 
                            item->src,
                            NN_OFIW_ERROR,
                            &item->data_err.cq_err_entry
                        );
                        nn_ctx_leave (worker->owner->ctx);

                    }

                    /* Break case */
                    break;

                /* EVENT QUEUE MODE */
                case NN_OFIW_ITEM_EQ:

                    /* Read event queue */
                    ret = fi_eq_read( (struct fid_eq *)item->fd, 
                        &event, item->data, item->data_size, 0);
                    if (nn_slow(ret != -FI_EAGAIN)) {

                        /* Handle exit value */
                        if (nn_slow( ret == -FI_EAVAIL )) {

                            /* Read EQ error */
                            sret = fi_eq_readerr( (struct fid_eq *)item->fd,
                                &item->data_err.eq_err_entry, 0);
                            if (nn_slow( sret != sizeof(struct fi_eq_err_entry) )) {
                                FT_PRINTERR("fi_eq_readerr", sret);
                                break;
                            }

                            /* Log */
                            _ofi_debug("OFI[w]: Got EQ Error Event from "
                                "src=%i, worker=%p, fd=%p, error=%i\n",
                                item->src, worker, item,
                                item->data_err.eq_err_entry.err);

                            /* Unlock mutex */
                            if (unlock_mutex) {
                                nn_mutex_unlock( &self->list_mutex );
                                unlock_mutex = 0;
                            }

                            #if defined OFI_USE_WAITSET
                                /* Mark as triggered */
                                triggered = 1;
                            #endif

                            /* Feed event to the FSM */
                            nn_ctx_enter (worker->owner->ctx);
                            nn_fsm_feed (worker->owner, 
                                item->src,
                                -item->data_err.eq_err_entry.err,
                                &item->data_err.eq_err_entry
                            );
                            nn_ctx_leave (worker->owner->ctx);

                        } else {

                            _ofi_debug("OFI[w]: Got EQ Event from src=%i, "
                                "worker=%p, fd=%p, event=%i\n",
                                item->src, worker, item, event);

                            /* Unlock mutex */
                            if (unlock_mutex) {
                                nn_mutex_unlock( &self->list_mutex );
                                unlock_mutex = 0;
                            }

                            #if defined OFI_USE_WAITSET
                                /* Mark as triggered */
                                triggered = 1;
                            #endif

                            /* Feed event to the FSM */
                            nn_ctx_enter (worker->owner->ctx);
                            nn_fsm_feed (worker->owner, 
                                item->src,
                                event,
                                item->data
                            );
                            nn_ctx_leave (worker->owner->ctx);

                        }


                    }

                    /* Break case */
                    break;

                /* MISSING FD */
                case NN_OFIW_NONE:
                    break;

            }

            /* If structure has changed, exit loop 
               (and lock again on mutex) */
            if (ATOMIC_SAFE( last_id != self->list_mutex_id )) {
                goto break_loop;
            }

        }
        break_loop:;

#if defined OFI_USE_WAITSET

        /* Repeat loop until not triggered any more */
        } while ( triggered && (self->state == NN_OFIW_STATE_ACTIVE) );

#endif

        /* Unlock mutex */
        if (unlock_mutex) {
            nn_mutex_unlock( &self->list_mutex );
            unlock_mutex = 0;
        }


#if !defined OFI_USE_WAITSET
        
        /* Spinwait for 1ms and then usleep for 1ms */
        if (nn_slow( !--kinstr )) {
            kinstr = 1000;
            if (nn_slow( !--spinwait )) {
                spinwait = self->kinst_per_ms;
                usleep( 1000 );
            }
        }

#endif

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
    nn_list_init( &self->items );
    nn_mutex_init( &self->list_mutex );
    self->list_mutex_id = 0;

#if defined OFI_USE_WAITSET

    /* Open a waitset */
    struct fi_wait_attr wait_attr = {
        .wait_obj = FI_WAIT_UNSPEC,
        .flags = 0
    };

    /* Open waitset */
    ret = fi_wait_open( fabric, &wait_attr, &self->waitset);
    if (ret) {
        FT_PRINTERR("fi_wait_open", ret);
        return ret;
    }

#endif

    /* Calculate kilo-instructions per second */
    self->kinst_per_ms = nn_ofiw_kinstr_per_ms();

    /* Start worker thread */
    nn_thread_init( &self->thread, nn_ofiw_polling_thread, self );

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

    /* Join worker thread */
    nn_thread_term( &self->thread );

    /* Stop all workers */
    while ((it=nn_list_begin(&self->workers)) != nn_list_end (&self->workers)) {
        worker = nn_cont (it, struct nn_ofiw, item);

        /* Terminate worker (this also removes the item from list) */
        nn_ofiw_term( worker );

    }

    /* Free structures */
    nn_list_term( &self->workers );
    nn_list_term( &self->items );
    nn_mutex_term( &self->list_mutex );

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
    struct nn_ofiw_pool * pool = self->parent;
    struct nn_ofiw_item *item;
    struct nn_list_item *it;

    /* Remove from worker list */
    nn_list_erase( &self->parent->workers, &self->item );
    nn_list_item_term( &self->item );

    /* Acquire lock & notify thread to use it */
    nn_mutex_lock( &self->parent->list_mutex );
    self->parent->list_mutex_id++;

    /* Remove all my items */
    for (it = nn_list_begin (&pool->items);
          it != nn_list_end (&pool->items);
          it = nn_list_next (&pool->items, it)) {
        item = nn_cont (it, struct nn_ofiw_item, item);

        /* Erase matching workers */
        if (item->worker == self) {

            /* Erase item */
            nn_list_erase( &pool->items, &item->item );
            nn_ofiw_pool_term_item( item );

            /* Rewind iterator */
            it = nn_list_begin(&pool->items);

        }

    }

    /* Release lock */
    nn_mutex_unlock( &self->parent->list_mutex );

    /* Free worker */
    nn_free( self );
}

/* Return the oppened CQ/EQ wait set */
struct fid_wait * nn_ofiw_waitset( struct nn_ofiw * worker )
{
#if defined OFI_USE_WAITSET
    /* Return global waitsete */
    return worker->parent->waitset;
#else
    /* Return NULL */
    return NULL;
#endif
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
    item->fd_type = NN_OFIW_ITEM_CQ;
    item->data = nn_alloc( sizeof(struct fi_cq_msg_entry) * cq_count, 
                            "ofiw item data");
    item->data_size = cq_count;

    /* Put item on list queue */
    nn_list_item_init( &item->item );
    nn_ofiw_pool_add_item( self->parent, item );

    _ofi_debug("OFI[w]: Added CQ fd=%p on worker=%p\n", item, self);

    /* Success */
    return 0;
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
    item->fd_type = NN_OFIW_ITEM_EQ;
    item->data = nn_alloc( sizeof(struct fi_eq_cm_entry), "ofiw item data" );
    item->data_size = sizeof(struct fi_eq_cm_entry);

    /* Put item on list queue */
    nn_list_item_init( &item->item );
    nn_ofiw_pool_add_item( self->parent, item );

    _ofi_debug("OFI[w]: Added EQ fd=%p on worker=%p\n", item, self);

    /* Success */
    return 0;
}

/* Remove the specified file descriptor from the worker stack */
int nn_ofiw_remove( struct nn_ofiw * self, void * fd )
{
    struct nn_ofiw_pool * pool = self->parent;
    struct nn_ofiw_item *item;
    struct nn_list_item *it;

    /* Lookup specified item */
    for (it = nn_list_begin (&pool->items);
          it != nn_list_end (&pool->items);
          it = nn_list_next (&pool->items, it)) {
        item = nn_cont (it, struct nn_ofiw_item, item);

        /* Check for matching fd */
        if (item->fd == fd) {

            _ofi_debug("OFI[w]: Removed fd=%p from worker=%p\n", item, self);

            /* Delete and clean-up */
            nn_ofiw_pool_del_item( pool, item );
            nn_ofiw_pool_term_item( item );
            
            /* Break */
            return 0;
        }

    }

    /* Not found */
    return -ENOENT;
}
