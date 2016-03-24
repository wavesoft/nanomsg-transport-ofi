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

#include "ring.h"

#include "../../../utils/alloc.h"
#include "../../../utils/err.h"

#define NN_RING_FLAG_OVERRUN    0x01
#define NN_RING_FLAG_UNDERRUN   0x02

/* Initialize the ring buffer with the element pointers pointing
   to the array of user-provided items starting on address `base` 
   and having size `item_size` */
void nn_ring_init( struct nn_ring * self, size_t size, void * base, 
    size_t item_size )
{
    void ** elm;
    void * src;

    /* Allocate items pointer */
    self->base = nn_alloc( sizeof(void*) * size, "ring elements");
    nn_assert(self->base);

    /* Update properties */
    self->head = self->base;
    self->tail = self->base;
    self->end = self->base + size;

    /* Populate their addresses */
    src = base; elm = self->head;
    while (elm < self->end) {
        *elm = src;
        src = (void*)(((uint8_t *) src) + item_size);
        elm++;
    }

    /* Initialize mutex */
    nn_mutex_init( &self->mutex );
    nn_efd_init( &self->efd_overrun );
    nn_efd_init( &self->efd_underrun );

}

/*  Terminate the ring structure */
void nn_ring_term( struct nn_ring *self )
{
    nn_free(self->base);
    nn_mutex_term(&self->mutex);
    nn_efd_term( &self->efd_overrun );
    nn_efd_term( &self->efd_underrun );
}

/* Push an item on the ring and return the element address.
   If the ring is full, this function will block until pop is called. */
void * nn_ring_push( struct nn_ring * self )
{
    void * item;

    /* Move tail forward */
    self->tail++;

    /* Test for overrun */
    if (self->tail > self->head) {

        /* Head is running slow, block */
        self->flags |= NN_RING_FLAG_OVERRUN;
        nn_efd_wait( &self->efd_overrun, -1 );

    } else if (self->tail > self->end) {
        if (self->head == self->base) {

            /* Head is running slow, block */
            self->flags |= NN_RING_FLAG_OVERRUN;
            nn_efd_wait( &self->efd_overrun, -1 );

        }
        self->tail = self->base;
    }

    /* Get item */
    item = *self->tail;

    /* If we have underrung, signal it now */
    if (self->flags & NN_RING_FLAG_UNDERRUN) {
        self->flags &= ~NN_RING_FLAG_UNDERRUN;
        nn_efd_signal( &self->efd_underrun );
    }

    /* Return item */
    return item;
}

/* Pop an item from the ring and return the element pointer.
   If the ring is full, this function will block until push is called. */
void * nn_ring_pop( struct nn_ring * self )
{
    void * item;

    /* Move head forward */
    self->head++;

    /* Test for underrun */
    if (self->head > self->tail) {

        /* Tail is running slow, block */
        self->flags |= NN_RING_FLAG_UNDERRUN;
        nn_efd_wait( &self->efd_underrun, -1 );

    } else if (self->head > self->end) {
        if (self->tail == self->base) {

            /* Tail is running slow, block */
            self->flags |= NN_RING_FLAG_UNDERRUN;
            nn_efd_wait( &self->efd_underrun, -1 );

        }
        self->head = self->base;
    }

    /* Get item */
    item = *self->head;

    /* If we have underrung, signal it now */
    if (self->flags & NN_RING_FLAG_OVERRUN) {
        self->flags &= ~NN_RING_FLAG_OVERRUN;
        nn_efd_signal( &self->efd_overrun );
    }

    /* Return item */
    return item;
}

/* Check if the ring is empty */
int nn_ring_empty( struct nn_ring * self )
{
    return (self->head == self->tail);
}
