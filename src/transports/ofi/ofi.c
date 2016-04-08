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

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include "ofi.h"
#include "bofi.h"
#include "cofi.h"
#include "ofiapi.h"

#include "../../ofi.h"
#include "../../core/ep.h"
#include "../../utils/err.h"
#include "../../utils/alloc.h"
#include "../../utils/fast.h"
#include "../../utils/cont.h"
#include "../../utils/list.h"

/* OFI-Specific socket options */
struct nn_ofi_optset {
    struct nn_optset base;
    int rx_queue_size;
    int tx_queue_size;
    int slab_size;
    size_t mem_align;
};

/* Optset interface */
static struct nn_optset *nn_ofi_optset (void);
static void nn_ofi_optset_destroy (struct nn_optset *self);
static int nn_ofi_optset_setopt (struct nn_optset *self, int option,
    const void *optval, size_t optvallen);
static int nn_ofi_optset_getopt (struct nn_optset *self, int option,
    void *optval, size_t *optvallen);
static const struct nn_optset_vfptr nn_ofi_optset_vfptr = {
    nn_ofi_optset_destroy,
    nn_ofi_optset_setopt,
    nn_ofi_optset_getopt
};

/*  nn_transport interface. */
static void nn_ofi_init ();
static void nn_ofi_term ();
static int  nn_ofi_bind (void *hint, struct nn_epbase **epbase);
static int  nn_ofi_connect (void *hint, struct nn_epbase **epbase);

/* Transport-wide static configuration */
static struct ofi_resources nn_ofi_resources;

/**
 * Expose the OFI transport pointer table
 */
static struct nn_transport nn_ofi_vfptr = {
    "ofi",
    NN_OFI,
    nn_ofi_init,
    nn_ofi_term,
    nn_ofi_bind,
    nn_ofi_connect,
    nn_ofi_optset,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_transport *nn_ofi = &nn_ofi_vfptr;

/**
 * Initialize OFI
 */
static void nn_ofi_init ()
{
    /* Initiailize OFI Resources */
    ofi_init( &nn_ofi_resources, FI_EP_MSG );
}

/**
 * Terminate OFI
 */
static void nn_ofi_term ()
{
    /* Terminate OFI resources */
    ofi_term( &nn_ofi_resources );
}

/**
 * Create a new bind socket
 */
static int nn_ofi_bind (void *hint, struct nn_epbase **epbase)
{
    int ret;
    struct ofi_fabric * fabric;

    /* Open a fabric for the specified address */
    ret = ofi_fabric_open(&nn_ofi_resources,nn_ep_getaddr((struct nn_ep*) hint),
        OFI_ADDR_LOCAL, &fabric );
    if (ret) {
        return ret;
    }

    /* Open a BOFI */
    return nn_bofi_create(hint, epbase, fabric);
}

/**
 * Create a new connected socket
 */
static int nn_ofi_connect (void *hint, struct nn_epbase **epbase)
{
    int ret;
    struct ofi_fabric * fabric;

    /* Open a fabric for the specified address */
    ret = ofi_fabric_open(&nn_ofi_resources,nn_ep_getaddr((struct nn_ep*) hint),
        OFI_ADDR_REMOTE, &fabric );
    if (ret) {
        return ret;
    }

    /* Open a COFI */
    return nn_cofi_create(hint, epbase, fabric);
}

/**
 * Create and return a new nn_ofi_optset
 */
static struct nn_optset *nn_ofi_optset (void)
{
    struct nn_ofi_optset *optset;

    optset = nn_alloc (sizeof (struct nn_ofi_optset), "optset (ofi)");
    alloc_assert (optset);
    optset->base.vfptr = &nn_ofi_optset_vfptr;

    /*  Default values for OFI socket options (0=max). */
    optset->rx_queue_size = 2;
    optset->tx_queue_size = 2;
    optset->slab_size = 4096;
#if _POSIX_C_SOURCE >= 200112L
    optset->mem_align = sysconf(_SC_PAGESIZE);
#else
    optset->mem_align = sizeof(void*);
#endif

    return &optset->base;
}

static void nn_ofi_optset_destroy (struct nn_optset *self)
{
    struct nn_ofi_optset *optset;

    optset = nn_cont (self, struct nn_ofi_optset, base);
    nn_free (optset);
}

static int nn_ofi_optset_setopt (struct nn_optset *self, int option,
    const void *optval, size_t optvallen)
{
    struct nn_ofi_optset *optset;
    int val;

    optset = nn_cont (self, struct nn_ofi_optset, base);

    /*  At this point we assume that all options are of type int. */
    if (optvallen != sizeof (int))
        return -EINVAL;
    val = *(int*) optval;

    switch (option) {
    case NN_OFI_RX_QUEUE_SIZE:
        if (nn_slow (val < 2))
            return -EINVAL;
        optset->rx_queue_size = val;
        return 0;
    case NN_OFI_TX_QUEUE_SIZE:
        if (nn_slow (val < 0))
            return -EINVAL;
        optset->tx_queue_size = val;
        return 0;
    case NN_OFI_MEM_ALIGN:
        if (nn_slow (val < 1))
            return -EINVAL;
        optset->mem_align = val;
        return 0;
    case NN_OFI_SLAB_SIZE:
        if (nn_slow (val < 0))
            return -EINVAL;
        optset->slab_size = val;
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

static int nn_ofi_optset_getopt (struct nn_optset *self, int option,
    void *optval, size_t *optvallen)
{
    struct nn_ofi_optset *optset;
    int intval;

    optset = nn_cont (self, struct nn_ofi_optset, base);

    switch (option) {
    case NN_OFI_RX_QUEUE_SIZE:
        intval = optset->rx_queue_size;
        break;
    case NN_OFI_TX_QUEUE_SIZE:
        intval = optset->tx_queue_size;
        break;
    case NN_OFI_MEM_ALIGN:
        intval = optset->mem_align;
        break;
    case NN_OFI_SLAB_SIZE:
        intval = optset->slab_size;
        break;
    default:
        return -ENOPROTOOPT;
    }
    memcpy (optval, &intval,
        *optvallen < sizeof (int) ? *optvallen : sizeof (int));
    *optvallen = sizeof (int);
    return 0;
}

#define PAGE_SHIFT 12
#define PAGEMAP_LENGTH 8

unsigned long get_page_frame_number_of_address(void *addr) {
   // Open the pagemap file for the current process
   FILE *pagemap = fopen("/proc/self/pagemap", "rb");

   // Seek to the page that the buffer is on
   unsigned long offset = (unsigned long)addr / getpagesize() * PAGEMAP_LENGTH;
   if(fseek(pagemap, (unsigned long)offset, SEEK_SET) != 0) {
      fprintf(stderr, "Failed to seek pagemap to proper location\n");
      exit(1);
   }

   // The page frame number is in bits 0-54 so read the first 7 bytes and clear the 55th bit
   unsigned long page_frame_number = 0;
   fread(&page_frame_number, 1, PAGEMAP_LENGTH-1, pagemap);

   page_frame_number &= 0x7FFFFFFFFFFFFF;

   fclose(pagemap);

   return page_frame_number;
}

unsigned long long get_physical_address(void *buffer) {

    // Find the difference from the buffer to the page boundary
    unsigned long page_frame_number = get_page_frame_number_of_address(buffer);
    unsigned long distance_from_page_boundary = (unsigned long)buffer % getpagesize();

    // Return physical page address
    return (page_frame_number << PAGE_SHIFT) + distance_from_page_boundary;

}
