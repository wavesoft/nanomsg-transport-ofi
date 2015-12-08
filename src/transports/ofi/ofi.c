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

#include "ofi.h"
#include "bofi.h"
#include "cofi.h"

#include "../../ofi.h"

// #include "../utils/port.h"
// #include "../utils/iface.h"

// #include "../../utils/err.h"
// #include "../../utils/alloc.h"
// #include "../../utils/fast.h"
// #include "../../utils/list.h"
// #include "../../utils/cont.h"

#include <string.h>

/*  nn_transport interface. */
static int  nn_ofi_bind (void *hint, struct nn_epbase **epbase);
static int  nn_ofi_connect (void *hint, struct nn_epbase **epbase);

/**
 * Expose the OFI transport pointer table
 */
static struct nn_transport nn_ofi_vfptr = {
    "ofi",
    NN_OFI,
    NULL,
    NULL,
    nn_ofi_bind,
    nn_ofi_connect,
    NULL,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_transport *nn_ofi = &nn_ofi_vfptr;

/**
 * Create a new bind socket
 */
static int nn_ofi_bind (void *hint, struct nn_epbase **epbase)
{
    return nn_bofi_create(hint, epbase);
}

/**
 * Create a new connected socket
 */
static int nn_ofi_connect (void *hint, struct nn_epbase **epbase)
{
    return nn_cofi_create(hint, epbase);
}
