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

#ifndef NN_OFIMR_INCLUDED
#define NN_OFIMR_INCLUDED

/* Forward declarations */
struct ofi_mr_bank;
struct ofi_mr_manager;
struct ofi_mr_context;

#include "oficommon.h"
#include "ofiapi.h"

#include "../../utils/mutex.h"

/**
 * This file implements the memory region manager API, used to pick the most
 * appropriate memory regions when sending/receiving data.
 *
 * The memory region manager must be usable both by the send and receive 
 * operations and must be transparent to the user.
 *
 * The management operates solely on the memory regions, therefore the interface
 * is the following:
 * 
 */

/* ########################################################################## */
/*  Global Structures                                                         */
/* ########################################################################## */

/* Bank flags */
#define OFI_MR_BANK_NONVOLATILE     0x01
#define OFI_MR_BANK_REGISTERED      0x02

/* Slab flags */
#define OFI_MR_SLAB_INUSE           0x01

/* Maximum number of banks that can be managed in a single context.
   This limits the maximum number of vectors that can exist in a scatter-gather
   array. This number *MUST* be smaller than 255 */
#define OFI_MR_MAX_BANKSPERCONTEXT  16
#define OFI_MR_MAX_SLABSPERCONTEXT  16

/**
 * The direction the MRM is used for. This defines the access
 * permissions of the registered memory regions.  
 */
enum ofi_mr_direction {
    OFI_MR_DIR_SEND,  
    OFI_MR_DIR_RECV,
    OFI_MR_DIR_BOTH
};

/**
 * OFI Memory Bank Attributes
 */
struct ofi_mr_bank_attr {

    /* The domain were to perform MR operations */
    struct ofi_domain *domain;

    /* The number of banks to allocate */
    size_t bank_count;

    /* The number of slab slots to allocate */
    size_t slab_count;

    /* The size of each slab */
    size_t slab_size;

    /* The registration direction */
    enum ofi_mr_direction direction;

    /* The base key to assign to memory regions */
    uint64_t base_key;

};

/**
 * An OFI Memory Bank for the MRM
 */
struct ofi_mr_bank {

    /* Concurrency mutex */
    struct nn_mutex mutex;

    /* The bank reference counter */
    int ref;

    /* The base address and length */
    void * base;
    size_t len;

    /* The memory region registration handler */
    struct fid_mr *mr;

    /* The age of the bank */
    uint32_t age;

    /* The bank flags */
    uint8_t flags;

};

/**
 * An OFI Memory Slab for small memory chunks
 */
struct ofi_mr_slab {

    /* The base address */
    void * addr;

    /* The slab flags */
    uint8_t flags;

};

/**
 * Core structure of the OFI Memory Region Manager
 */
struct ofi_mr_manager {

    /* The available MR banks */
    struct ofi_mr_bank  *banks;

    /* Attributes used to construct the manager */
    struct ofi_mr_bank_attr attr;

    /* The access flags for the MR registration */
    uint64_t access_flags;

    /* The age of the manager */
    uint32_t age;

    /* Properties for the slab logic */
    struct nn_mutex slab_mutex;
    struct ofi_mr_slab *slabs;
    struct fid_mr *slab_mr;
    void *slab_mem;

};

/* ########################################################################## */
/*  Interface Functions                                                       */
/* ########################################################################## */

/**
 * Initialize the memory region manager with the specified capacity of memory
 * registration banks.
 */
int ofi_mr_init( struct ofi_mr_manager * self, 
    const struct ofi_mr_bank_attr * attr );

/**
 * Clean-up the memory region manager resources
 */
int ofi_mr_term( struct ofi_mr_manager * self );

/**
 * Add a memory registration hint for the specified region
 *
 * This will reserve a non-volatile MRM bank with the specified information and
 * perform a memory registration right away. You should call this function if
 * you have allocated a particular memory region yourself that you will later
 * use for Tx/Rx operations.
 *
 * This is also useful when you are alocating a bigger memory region from which
 * you will send smaller chunks. MRM will find and return memory regions with
 * smaller chunks.
 *
 * This function will return -ENOMEM if there are no free banks available
 * to hold the mark information.
 *
 */
int ofi_mr_mark( struct ofi_mr_manager * self, void * base, size_t len );

/**
 * Invalidate the specified memory region, forcing intersecting memory
 * registrations to be released.
 *
 * This is useful when you are releasing a pointer previously registered
 * with `ofi_mr_mark`.
 *
 * This function will return -EBUSY if the memory region specified is
 * currently in use by some bank.
 */
int ofi_mr_invalidate( struct ofi_mr_manager * self, void * base, size_t len );

/**
 * Populate the local descriptors of the `fi_msg` structure and perform
 * the appropriate memory registrations if needed.
 *
 * This function will do the following:
 *
 * - Check every element in the scatter-gather array and pick the most
 *   appropriate memory region.
 * - If nothing is available, register the new memory regions.
 * - If no free banks are available, return -EAGAIN
 * - Populate the `desc` fields accordingly.
 * - Replace the user's context with an `ofi_mr_context` that contains the
 *   additional registration information.
 * 
 * Upon successful transmission/completion the user *MUST* call the
 * `ofi_mr_release` function in order to make the banks available for re-use!
 *
 */
int ofi_mr_describe( struct ofi_mr_manager * self, struct fi_msg * msg, void ** handle );

/**
 * Re-use the memory registration banks used for the particular transmission
 * or reception operation.
 * 
 * This function will also replace the passed context with the original
 * context the user specified in the `fi_msg` structure.
 */
int ofi_mr_release( void * handle );

#endif
