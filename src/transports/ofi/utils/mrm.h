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


#ifndef NN_OFI_UTILS_MRM
#define NN_OFI_UTILS_MRM

#include "../hlapi.h"
#include "../../../utils/chunk.h"
#include "../../../utils/chunkref.h"
#include "../../../utils/mutex.h"

/* How many bytes to allocate for ancillary data for every chunk */
#define NN_OFI_MRM_ANCILLARY_SIZE   64

/* MRM Chunk Flags */
#define NN_OFI_MRM_FLAG_ASSIGNED    0x00000001
#define NN_OFI_MRM_FLAG_LOCKED      0x00000002

/* TODO: The current complexity of this code is O(n), but it can be easily
         converted to O(log(n)) with a binary search on the pointer indices. */

/* Memory region manager chunk information */
struct nn_ofi_mrm_chunk {

    /* Flags describing the state of the memory region */
    uint32_t flags;

    /* Age of the chunk in order to dispose the oldest one */
    uint32_t age;

    /* Chunk description for the registered MR chunk */
    struct nn_chunk_desc desc;

    /* The description of the memory region with libfabric */
    struct fid_mr *mr;

    /* The pointer to the shared MR for ancillary data */
    void * ancillary;

    /* The libfabric context for putting the chunk as Rx/Tx context parameter */
    struct fi_context context;

    /* Piggyback data, used by other operations, exploiting the fact that the 
       memory is already allocated. */
    struct {
        struct iovec    mr_iov[2];
        void *          mr_desc[2];
    } data;

};

/* Memory region manager structure that tracks registered MRs with the NIC */
struct nn_ofi_mrm {

    /* The libfabric endpoint to bind the MRs on */
    struct ofi_active_endpoint * ep;

    /* Chunks registered in the system */
    struct nn_ofi_mrm_chunk *chunks;

    /* Maximum number of chunks to manage */
    size_t len;

    /* Age of the manager */
    uint32_t age;

    /* Access flags for new MRs */
    uint64_t access_flags;

    /* Base key to use for MRs. Each new MR gets a number after it */
    int base_key;

    /* Mutex for synchronisation */
    struct nn_mutex sync;

    /* A memory region given to each chunk for ancillary data 
       (ex. protocol headers) */
    struct fid_mr *mr_ancillary;
    void * ptr_ancillary;

};

/* Initialize the memory region manager */
int nn_ofi_mrm_init( struct nn_ofi_mrm * self, struct ofi_active_endpoint * ep, 
    size_t len, int base_key, uint64_t access_flags );

/* Free the memory region manager and it's managed MRs */
int nn_ofi_mrm_term( struct nn_ofi_mrm * self );

/* Check if there are no locked chunks */
int nn_ofi_mrm_isidle( struct nn_ofi_mrm * self );

/* Check if there are no unlocked chunks */
int nn_ofi_mrm_isfull( struct nn_ofi_mrm * self );

/* Pick the appropriate memory region for the given chunk reference and 
   extract the pointers and region descriptions to use with tx/rx operations.
   This function will also mark the memory region as 'in transit' and will
   not be released until the `nn_ofi_mrm_unlock` is called. */
int nn_ofi_mrm_lock( struct nn_ofi_mrm * self, struct nn_ofi_mrm_chunk ** mrmc,
    struct nn_chunkref * chunkref );

/* Release the memory region chunk */
int nn_ofi_mrm_unlock( struct nn_ofi_mrm * self, struct nn_ofi_mrm_chunk * mrmc );

#endif
