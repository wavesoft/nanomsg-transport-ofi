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

#ifndef NN_OFI_INCLUDED
#define NN_OFI_INCLUDED

/* Support for stand-alone tests */
#ifndef __STANDALONE_TESTS 
#include "../../transport.h"
#endif

/* The following flags enable various source paths.
   You can specify them as compiler definitions */

/* Enable debug logs */
// #define OFI_DEBUG_LOG

/* Open one domain per endpoint */
// #define OFI_DOMAIN_PER_EP

/* Enable waitsets (on providers that supports it) for 
   a more optimized worker polling */
// #define OFI_USE_WAITSET

/* Disable handshake negotiation (usnic fix) */
#define OFI_DISABLE_HANDSHAKE

/* Helper macro to enable or disable verbose logs on console */
#ifdef OFI_DEBUG_LOG
    #include <stdio.h>
    #include <pthread.h>
    /* Enable debug */
    #define _ofi_debug(...)   { \
        struct timespec ts; \
        clock_gettime(CLOCK_MONOTONIC, &ts); \
        char __msg[1024]; \
        sprintf(__msg, __VA_ARGS__); \
        printf("[%010li.%06li] %s", ts.tv_sec, ts.tv_nsec, __msg); \
        fflush(stdout); \
    };
    // #define _ofi_debug(...)     printf(__VA_ARGS__)
#else
    /* Disable debug */
    #define _ofi_debug(...)
#endif

/* Default slab memory size (I/O operations with this size
 * or less will be memcpy'ied to the share memory slab). Above this,
 * they will be tagged as shared and sent as-is. */
#define NN_OFI_DEFAULT_SLABMR_SIZE  65536

/* Message size for blocking small I/O */
#define NN_OFI_SMALLMR_SIZE         1024

/* SOFI Source */
#define NN_OFI_SRC_SOFI             4000

extern struct nn_transport *nn_ofi;


#endif
