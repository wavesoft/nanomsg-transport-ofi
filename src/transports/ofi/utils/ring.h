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

#ifndef NN_RING_INCLUDED
#define NN_RING_INCLUDED

#include "../../../utils/int.h"
#include "../../../utils/mutex.h"
#include "../../../utils/efd.h"

/* A ring buffer with back-pressure support */
struct nn_ring {

    /* The ring buffer elements */
    void **base;

    /* Head and tail pointers */
    void **head;
    void **tail;
    void **end;

    /* Push and pop efd */
    struct nn_efd efd_overrun, efd_underrun;
    uint8_t flags;

    /* I/O Mutex */
    struct nn_mutex mutex;

};

/* Initialize the ring buffer with the element pointers pointing
   to the array of user-provided items starting on address `base` 
   and having size `item_size` */
void nn_ring_init( struct nn_ring * self, size_t size, void * base, 
    size_t item_size );

/*  Terminate the ring structure */
void nn_ring_term( struct nn_ring *self );

/* Push an item on the ring and return the element address.
   If the ring is full, this function will block until pop is called. */
void * nn_ring_push( struct nn_ring * self );

/* Pop an item from the ring and return the element pointer.
   If the ring is full, this function will block until push is called. */
void * nn_ring_pop( struct nn_ring * self );

/* Check if the ring is empty */
int nn_ring_empty( struct nn_ring * self );

#endif