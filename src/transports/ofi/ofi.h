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

/* Default slab memory size (I/O operations with this size
 * or less will be memcpy'ied to the share memory slab). Above this,
 * they will be tagged as shared and sent as-is. */
#define NN_OFI_DEFAULT_SLABMR_SIZE 65536

/* Uncomment the following to enable verose messages for debugging */
#define OFI_DEBUG_LOG

extern struct nn_transport *nn_ofi;


#endif
