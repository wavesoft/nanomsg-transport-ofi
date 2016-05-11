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

/* Nanomsg-Specific EQ events injected to queues */
#define FI_NN_SHUTDOWN  0xF0F00001

/* ============================== */
/*       HELPER FUNCTIONS         */
/* ============================== */

/**
 * Calculate how many kilo-cycles we can run per millisecond
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
    kinst_ms = 1000000000 / ((b.tv_sec - a.tv_sec) * 1000000000ULL + b.tv_nsec - a.tv_nsec);

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
 * Wait until mutex thread is idle and place a block request
 */
static void nn_ofiw_lock_thread( struct nn_ofiw_pool* self )
{
    /* Skip locking when safe */
    if (nn_fast( self->lock_safe ))
        return;

    /* Place lock request */
    nn_mutex_lock( &self->lock_mutex );
    self->lock_state = 1;

#ifndef OFI_USE_WAITSET
    /* Wait for thread to lock */
    nn_efd_wait( &self->efd_lock_ack, -1 );
    nn_efd_unsignal( &self->efd_lock_ack );
#endif

}

/**
 * Unblock a previous block request
 */
static void nn_ofiw_unlock_thread( struct nn_ofiw_pool* self )
{
    /* Skip unlocking when safe */
    if (nn_fast( self->lock_safe ))
        return;

#ifndef OFI_USE_WAITSET
    /* Unlock thread */
    nn_efd_signal( &self->efd_lock_req );
#endif

    /* Release lock request */
    self->lock_state = 0;
    nn_mutex_unlock( &self->lock_mutex );
}

/* OFI worker poller thread */
static void nn_ofiw_poller_thread( void *arg )
{
    struct nn_ofiw_pool * self = (struct nn_ofiw_pool*) arg;
    struct nn_ofiw_item *item;
    struct nn_ofiw *worker;
    struct nn_list_item *it, *jt;
    ssize_t sret;
    int ret;
    int i;

    struct fi_eq_cm_entry   eq_entry;
    uint32_t                event;

#ifndef OFI_USE_WAITSET
    uint32_t                spinwait;
    uint32_t                kinstr;
    spinwait = self->kinst_per_ms;
    kinstr = 10000; /* 10 ms of spinlock */
#endif

    _ofi_debug("OFI[w]: Starting OFIW pool thread\n");
    while (self->active) {

#ifdef OFI_USE_WAITSET

        /* If waitsets are available, we have an optimized way for
           waiting for an event across all of our objects. */
        ret = fi_wait( self->waitset, -1 );
        if (nn_slow( (ret < 0) && (ret != -FI_EAGAIN) )) {
            FT_PRINTERR("fi_wait", ret);
            break;
        }

#else

        /* Handle block request before we start iterating */
        if (nn_slow( self->lock_state )) {
            nn_efd_signal( &self->efd_lock_ack );
            nn_efd_wait( &self->efd_lock_req, -1 );
            nn_efd_unsignal( &self->efd_lock_req );
        }

#endif

        nn_mutex_lock( &self->glob_mutex );

        /* Iterate over workers */
        for (it = nn_list_begin (&self->workers);
              it != nn_list_end (&self->workers);
              it = nn_list_next (&self->workers, it)) {
            worker = nn_cont (it, struct nn_ofiw, item);

            /* Skip inactive workers */
            if (nn_slow( worker->active < 2 )) {
                if (nn_slow( worker->active == 1)) {
                    _ofi_debug("OFI[w]: Signalling ACK\n");
                    nn_efd_signal(&worker->efd_sync);
                }
                continue;
            }

            /* Synchronisation mutex */
            nn_mutex_lock( &worker->mutex );

            /* Iterate over poll items */
            for (jt = nn_list_begin (&worker->items);
                  jt != nn_list_end (&worker->items);
                  jt = nn_list_next (&worker->items, jt)) {
                item = nn_cont (jt, struct nn_ofiw_item, item);

                /* Handle according to type */
                switch (item->fd_type) {

                    /* COMPLETION QUEUE MODE */
                    case NN_OFIW_ITEM_CQ:

                        /* Read completion queue */
                        ret = fi_cq_read( (struct fid_cq *)item->fd,
                            item->data, item->data_size);
                        if (nn_slow(ret > 0)) {
                            _ofi_debug("OFI[w]: Got %i CQ Event(s) from src=%i,"
                               " worker=%p, fd=%p\n",ret,item->src,worker,item);

                            /* Unlock worker mutex */
                            nn_mutex_unlock( &worker->mutex );
                            nn_mutex_unlock( &self->glob_mutex );

                            /* Feed event to the FSM */
                            self->lock_safe = 1;
                            nn_ctx_enter (worker->owner->ctx);
                            for (i=0; i<ret; i++) {

                                // printf("§§<< ack_ctx=%p, (ptr=%p, len=%zu)\n", 
                                //     ((struct fi_cq_msg_entry *)item->data)[i].op_context, 
                                //     ((struct fi_cq_msg_entry *)item->data)[i].buf,
                                //     ((struct fi_cq_msg_entry *)item->data)[i].len);

                                nn_fsm_feed (worker->owner, 
                                    item->src,
                                    NN_OFIW_COMPLETED,
                                    &((struct fi_cq_msg_entry *)item->data)[i]
                                );
                            }
                            nn_ctx_leave (worker->owner->ctx);
                            self->lock_safe = 0;

                            /* Exit both loops, since workers list
                               might have been altered from the FSM handler! */
                            goto continue_outer;

                        } else if (nn_slow(ret != -FI_EAGAIN)) {

                            /* Get error details */
                            ret = fi_cq_readerr( (struct fid_cq *)item->fd,
                                &item->data_err.cq_err_entry, 0);

                            _ofi_debug("OFI[w]: Got CQ Error from src=%i, worker=%p, fd=%p\n",
                                item->src, worker, item);

                            /* Unlock worker mutex */
                            nn_mutex_unlock( &worker->mutex );
                            nn_mutex_unlock( &self->glob_mutex );

                            /* Feed event to the FSM */
                            self->lock_safe = 1;
                            nn_ctx_enter (worker->owner->ctx);
                            nn_fsm_feed (worker->owner, 
                                item->src,
                                NN_OFIW_ERROR,
                                &item->data_err.cq_err_entry
                            );
                            nn_ctx_leave (worker->owner->ctx);
                            self->lock_safe = 0;

                            /* Exit both loops, since workers list
                               might have been altered from the FSM handler! */
                            goto continue_outer;

                        }

                        break;

                    /* EVENT QUEUE MODE */
                    case NN_OFIW_ITEM_EQ:

                        /* Read event queue */
                        ret = fi_eq_read( (struct fid_eq *)item->fd, 
                            &event, item->data, item->data_size,0);
                        if (nn_slow(ret != -FI_EAGAIN)) {

                            if (nn_slow( ret == -FI_EAVAIL )) {

                                sret = fi_eq_readerr( (struct fid_eq *)item->fd,
                                    &item->data_err.eq_err_entry, 0);
                                if (nn_slow( sret != sizeof(struct fi_eq_err_entry) )) {
                                    FT_PRINTERR("fi_eq_readerr", sret);
                                    break;
                                }

                                _ofi_debug("OFI[w]: Got EQ Error Event from "
                                    "src=%i, worker=%p, fd=%p, error=%i\n",
                                    item->src, worker, item,
                                    item->data_err.eq_err_entry.err);

                                /* Unlock worker mutex */
                                nn_mutex_unlock( &worker->mutex );
                                nn_mutex_unlock( &self->glob_mutex );

                                /* Feed event to the FSM */
                                self->lock_safe = 1;
                                nn_ctx_enter (worker->owner->ctx);
                                nn_fsm_feed (worker->owner, 
                                    item->src,
                                    -item->data_err.eq_err_entry.err,
                                    &item->data_err.eq_err_entry
                                );
                                nn_ctx_leave (worker->owner->ctx);
                                self->lock_safe = 0;

                            } else {

                                _ofi_debug("OFI[w]: Got EQ Event from src=%i, "
                                    "worker=%p, fd=%p, event=%i\n",
                                    item->src, worker, item, event);

                                /* Unlock worker mutex */
                                nn_mutex_unlock( &worker->mutex );
                                nn_mutex_unlock( &self->glob_mutex );

                                /* Feed event to the FSM */
                                self->lock_safe = 1;
                                nn_ctx_enter (worker->owner->ctx);
                                nn_fsm_feed (worker->owner, 
                                    item->src,
                                    event,
                                    item->data
                                );
                                nn_ctx_leave (worker->owner->ctx);
                                self->lock_safe = 0;

                            }

                            /* Exit both loops, since workers list
                               might have been altered from the FSM handler! */
                            goto continue_outer;

                        }
                        break;

                }

            }

            /* Unlock synchronisation mutex */
            nn_mutex_unlock( &worker->mutex );

        }

        /* Handle internal event queue events */
        ret = fi_eq_read( self->eq, &event, &eq_entry, sizeof(eq_entry),0);
        if (nn_slow(ret != -FI_EAGAIN)) {
            switch (event) {

                case FI_NN_SHUTDOWN:
                    /* Exit thread */
                    _ofi_debug("OFI[w]: Got worker shutdown event, breaking\n");
                    break;

            }
        }

        // unlock global mutex
        nn_mutex_unlock( &self->glob_mutex );

continue_outer:

#ifndef OFI_USE_WAITSET
        
        /* Spinwait for short time */
        if (nn_slow( !--kinstr )) {
            kinstr = 10000;
            if (nn_slow( !--spinwait )) {
                spinwait = self->kinst_per_ms;
                usleep( 100 );
            } else {
                sched_yield();
            }
        } else {
            sched_yield();
        }

#else

        /* We need a dummy instruction for the label */
        continue;

#endif

    }
    _ofi_debug("OFI[w]: Exiting OFIW pool thread\n");

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
    self->lock_state = 0;
    self->lock_safe = 0;

#ifndef OFI_USE_WAITSET
    /* Calculate how many kilo-instructions we can count per millisecond */
    self->kinst_per_ms = nn_ofiw_kinstr_per_ms();
    _ofi_debug("OFI[W]: We delay with %u kilo-instructions/ms\n",
        self->kinst_per_ms);
#endif

    /* Initialize structures */
    nn_list_init( &self->workers );
    nn_mutex_init( &self->lock_mutex );
    nn_mutex_init ( &self->glob_mutex );

#ifdef OFI_USE_WAITSET

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

#else

    /* Initialize spinlock-only structures */
    nn_efd_init( &self->efd_lock_req );
    nn_efd_init( &self->efd_lock_ack );

#endif

    /* Create an EQ that we use for signaling the termination */
    struct fi_eq_attr internal_eq = {
        .size = 1,
        .flags = FI_WRITE,
#ifdef OFI_USE_WAITSET
        .wait_obj = FI_WAIT_SET,
        .wait_set = self->waitset,
#else
        .wait_obj = FI_WAIT_NONE,
        .wait_set = NULL,
#endif
    };
    ret = fi_eq_open( fabric, &internal_eq, &self->eq, &self->context);
    if (ret) { 
        FT_PRINTERR("fi_eq_open", ret);
        return ret;
    }

    /* Start poller thread */
    self->active = 1;
    nn_thread_init( &self->thread, &nn_ofiw_poller_thread, self );

    /* Success */
    return 0;
}

/* Terminate an OFI worker pool */
int nn_ofiw_pool_term( struct nn_ofiw_pool * self )
{

    /* Signal the shutdown EQ */
    struct fi_eq_cm_entry entry = {0};
    ssize_t rd;
    rd = fi_eq_write( self->eq, FI_NN_SHUTDOWN, &entry, sizeof(entry), 0 );

    /* Stop thread */
    self->active = 0;
    nn_thread_term( &self->thread );

    /* Clean-up lock resources */
    nn_mutex_term( &self->lock_mutex );
#ifndef OFI_USE_WAITSET
    nn_efd_term( &self->efd_lock_req );
    nn_efd_term( &self->efd_lock_ack );
#endif
    
    /* Success */
    return 0;
}

/* Get an OFI worker from the specified worker pool */
struct nn_ofiw * nn_ofiw_pool_getworker( struct nn_ofiw_pool * self,
    struct nn_fsm * owner )
{
    struct nn_ofiw * worker;
    nn_ofiw_lock_thread( self );

    /* Allocate a new worker */
    worker = nn_alloc( sizeof(struct nn_ofiw), "OFI worker" );
    nn_assert( worker );

    /* Initialize */
    worker->active = 2;
    worker->owner = owner;
    worker->parent = self;
    nn_list_init( &worker->items );
    nn_efd_init( &worker->efd_sync );
    nn_mutex_init( &worker->mutex );

    /* Add to list */
    nn_list_item_init( &worker->item );
    nn_list_insert (&self->workers, &worker->item,
        nn_list_end (&self->workers));

    /* Return worker */
    nn_ofiw_unlock_thread( self );
    _ofi_debug("OFI[w]: Allocated new worker %p\n", worker);
    return worker;
}

/* Terminate a worker */
void nn_ofiw_term( struct nn_ofiw * self )
{
    struct nn_ofiw_pool * pool;
    struct nn_ofiw_item *item;
    struct nn_list_item *it;

    pool = self->parent;
    nn_ofiw_lock_thread( pool );

    /* Remove from worker list */
    nn_list_erase( &pool->workers, &self->item );
    nn_list_item_term( &self->item );
    nn_mutex_term( &self->mutex );

    /* Dispose all items */
    while ((it = nn_list_begin (&self->items)) != nn_list_end (&self->items)) {
        item = nn_cont (it, struct nn_ofiw_item, item);

        /* Free structure */
        nn_list_erase( &self->items, &item->item );
        nn_list_item_term( &item->item );
        nn_free( item );

    }

    /* Terminate structures */
    nn_list_term( &self->items );
    nn_efd_term( &self->efd_sync );

    /* Free worker */
    nn_free( self );
    nn_ofiw_unlock_thread( pool );
}

/**
 * Enable worker
 */
void nn_ofiw_start( struct nn_ofiw * worker )
{
    /* Start worker */
    _ofi_debug("OFI[w]: Starting worker %p\n", worker);
    worker->active = 2;
}

/**
 * Disable worker
 */
void nn_ofiw_stop( struct nn_ofiw * worker )
{
    /* Skip if already inactive */
    if (nn_fast( worker->active != 2))
        return;

    /* Stop worker */
    _ofi_debug("OFI[w]: Stopping worker %p\n", worker);
    if (nn_fast( worker->parent->lock_safe )) {

        /* Stop without sync */
        worker->active = 0;

    } else {

        /* Wait sync */
        worker->active = 1;
        nn_efd_wait( &worker->efd_sync, -1 );

        /* Synchronized */
        worker->active = 0;
        nn_efd_unsignal( &worker->efd_sync );

    }

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
    item->src = src;
    item->fd_type = NN_OFIW_ITEM_CQ;
    item->fd = cq;
    item->data = nn_alloc( sizeof(struct fi_cq_msg_entry) * cq_count, 
                            "ofiw item data");
    item->data_size = cq_count;

    /* Put item on list queue */
    nn_list_item_init( &item->item );
    nn_list_insert (&self->items, &item->item,
        nn_list_end (&self->items));

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
    item->src = src;
    item->fd_type = NN_OFIW_ITEM_EQ;
    item->fd = eq;
    item->data = nn_alloc( sizeof(struct fi_eq_cm_entry), "ofiw item data" );
    item->data_size = sizeof(struct fi_eq_cm_entry);

    /* Put item on list queue */
    nn_list_item_init( &item->item );
    nn_list_insert (&self->items, &item->item,
        nn_list_end (&self->items));

    _ofi_debug("OFI[w]: Added EQ fd=%p on worker=%p\n", item, self);

    /* Success */
    return 0;
}

/* Wrapping function to fi_eq_open that properly registers the resulting EQ
   in the worker. This function might update EQ attributes accordingly in order
   to configure waitsets. */
int nn_ofiw_open_eq( struct nn_ofiw * self, int src, void *context, 
    struct fi_eq_attr *attr, struct fid_eq **eq )
{
    int ret;

#ifdef OFI_USE_WAITSET

    /* Use the global waitset */
    if (attr->wait_obj == FI_WAIT_FD) {
        return -EINVAL;
    } else if (attr->wait_obj == FI_WAIT_MUTEX_COND) {
        return -EINVAL;
    } else {
        attr->wait_obj = FI_WAIT_SET;
        attr->wait_set = self->parent->waitset;
    }

#endif

    /* Open EQ */
    ret = fi_eq_open( self->parent->fabric, attr, eq, context);
    if (ret) {        
        FT_PRINTERR("fi_eq_open", ret);
        return ret;
    }

    /* Register */
    return nn_ofiw_add_eq( self, *eq, src );

}

/* Wrapping function to fi_cq_open that properly registers the resulting CQ
   in the worker. This function might update CQ attributes accordingly in order
   to configure waitsets. */
int nn_ofiw_open_cq( struct nn_ofiw * self, int src, struct fid_domain *domain,
    void *context, struct fi_cq_attr *attr, struct fid_cq **cq )
{
    int ret;

#ifdef OFI_USE_WAITSET

    /* Use the global waitset */
    if (attr->wait_obj == FI_WAIT_FD) {
        return -EINVAL;
    } else if (attr->wait_obj == FI_WAIT_MUTEX_COND) {
        return -EINVAL;
    } else {
        attr->wait_obj = FI_WAIT_SET;
        attr->wait_set = self->parent->waitset;
    }

#endif

    /* Open EQ */
    ret = fi_cq_open( domain, attr, cq, context);
    if (ret) {        
        FT_PRINTERR("fi_cq_open", ret);
        return ret;
    }

    /* Register */
    return nn_ofiw_add_cq( self, *cq, attr->size, src );

}

/* Remove the specified file descriptor from the worker stack */
int nn_ofiw_remove( struct nn_ofiw * self, void * fd )
{
    struct nn_ofiw_item *item;
    struct nn_list_item *it;
    nn_ofiw_lock_thread( self->parent );

    /* Lookup specified item */
    for (it = nn_list_begin (&self->items);
          it != nn_list_end (&self->items);
          it = nn_list_next (&self->items, it)) {
        item = nn_cont (it, struct nn_ofiw_item, item);

        /* Check for matching fd */
        if (item->fd == fd) {

            _ofi_debug("OFI[w]: Removed fd=%p from worker=%p\n", item, self);

            /* Free structure */
            nn_list_erase( &self->items, &item->item );
            nn_list_item_term( &item->item );
            nn_free( item->data );
            nn_free( item );
            
            /* Break */
            break;
        }

    }

    /* Success */
    nn_ofiw_unlock_thread( self->parent );
    return 0;
}

/* Synchronisation lock */
void nn_ofiw_lock( struct nn_ofiw * worker )
{
    nn_mutex_lock( &worker->parent->glob_mutex ); //mutex );
}

/* Synchronisation unlock */
void nn_ofiw_unlock( struct nn_ofiw * worker )
{
    nn_mutex_unlock( &worker->parent->glob_mutex ); //mutex );
}
