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

#ifndef NN_OFICOMMON_INCLUDED
#define NN_OFICOMMON_INCLUDED

/**
 * This file contains the common libfabric-related macros and definitions, in
 * order to avoid circular references.
 */

#include <time.h>
#ifdef __APPLE__
#include "platf/osx.h"
#endif

#include <rdma/fabric.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>

/* ########################################################################## */
/*  Global Tool Macros                                                        */
/* ########################################################################## */

/* Debug print a libfabric error */
#define FT_PRINTERR(call, retv) \
    do { fprintf(stderr, "OFI: Error on " call "(): %s:%d, ret=%d (%s)\n", \
        __FILE__, __LINE__, (int) retv, fi_strerror((int) -retv)); } while (0)

/* Close a libfabric file descriptor, including error debug logs */
#define FT_CLOSE_FID(fd)            \
    do {                    \
        if ((fd)) {         \
            int ret = fi_close(&(fd)->fid); \
            if (ret) { \
                if (ret == -FI_EBUSY) { \
                    printf("OFI[H]: *** Error closing FD " #fd " (FI_EBUSY)\n"); \
                } else { \
                    printf("OFI[H]: *** Error closing FD " #fd " caused error = %i\n", ret); \
                } \
            } \
            fd = NULL;      \
        }               \
    } while (0)


#endif