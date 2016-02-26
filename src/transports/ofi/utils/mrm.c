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

#include "mrm.h"
#include <string.h>

#include "../ofi.h"
#include "../../../utils/alloc.h"
#include "../../../utils/err.h"

/* Private methods */
static struct nn_ofi_mrm_chunk * pick_chunk( struct nn_ofi_mrm * self, 
    void *chunk );
static int nn_ofi_mrm_manage ( struct nn_ofi_mrm * self, 
    struct nn_ofi_mrm_chunk * chunk, struct nn_chunk_desc * desc );
static int nn_ofi_mrm_unmanage ( struct nn_ofi_mrm * self,
    struct nn_ofi_mrm_chunk * chunk );

/* Initialize the memory region manager */
int nn_ofi_mrm_init( struct nn_ofi_mrm * self, struct ofi_active_endpoint * ep, 
    size_t len, int base_key, uint64_t access_flags )
{
    int ret;
    size_t ancillary_len;

    /* Keep references */
    self->ep = ep;
    self->len = len;
    self->access_flags = access_flags;
    self->base_key = base_key;

    /* Init mutex */
    nn_mutex_init( &self->sync );

    /* Allocate ancillary pointer */
    ancillary_len = NN_OFI_MRM_ANCILLARY_SIZE * len;
    self->ptr_ancillary = nn_alloc( ancillary_len, "mrm ancillary" );
    memset( self->ptr_ancillary, 0, ancillary_len );
    nn_assert( self->ptr_ancillary );

    /* Perform memory registration of ancillary buffer */
    _ofi_debug("OFI[-]: Initializing MRM with len=%lu, key=%i\n",len,base_key);
    ret = fi_mr_reg(ep->domain, self->ptr_ancillary, ancillary_len, 
        access_flags, 0, base_key, 0, &self->mr_ancillary, NULL);
    if (ret) {
        FT_PRINTERR("nn_ofi_mrm_init->fi_mr_reg", ret);
        return ret;
    }

    /* Initialize chunks */
    self->chunks = nn_alloc( sizeof(struct nn_ofi_mrm_chunk) * len,
        "mrm chunk" );
    memset( self->chunks, 0, sizeof(struct nn_ofi_mrm_chunk) * len );
    nn_assert( self->chunks != NULL );

    /* Set default values */
    uint8_t * aux_ptr = self->ptr_ancillary;
    for (struct nn_ofi_mrm_chunk * mr = self->chunks, 
            * mr_end = self->chunks + len;
         mr < mr_end; mr++ ) {

        _ofi_debug("OFI[-]: Allocated MRM chunk=%p\n", mr);

        /* Reset properties */
        mr->flags = 0;
        mr->age = 0;
        mr->mr = NULL;

        /* Get portion of ancillary data */
        mr->ancillary = aux_ptr;
        aux_ptr += NN_OFI_MRM_ANCILLARY_SIZE;

    }

    /* Success */
    return 0;
}

/* Free the memory region manager and it's managed MRs */
int nn_ofi_mrm_term( struct nn_ofi_mrm * self )
{
    _ofi_debug("OFI[-]: Terminating MRM\n");
    /* Iterate over chunks and free allocated */
    for (struct nn_ofi_mrm_chunk * mr = self->chunks, 
            * mr_end = self->chunks + self->len;
         mr < mr_end; mr++ ) {

        /* Nothing must be locked */
        nn_assert( (mr->flags & NN_OFI_MRM_FLAG_LOCKED) == 0 );

        /* Unmanage */
        if (mr->flags & NN_OFI_MRM_FLAG_ASSIGNED)
            nn_ofi_mrm_unmanage( self, mr );

        _ofi_debug("OFI[-]: Released MRM chunk=%p\n", mr);

    }

    /* Term mutex */
    nn_mutex_term( &self->sync );

    /* Close ascillary MR */
    FT_CLOSE_FID( self->mr_ancillary );

    /* Free dynamic pointers */
    nn_free( self->ptr_ancillary );
    nn_free( self->chunks );

    /* Success */
    return 0;
}

/* Pick and set-up appropriate MR */
int nn_ofi_mrm_lock( struct nn_ofi_mrm * self, struct nn_ofi_mrm_chunk ** mrmc,
    struct nn_chunkref * chunkref )
{
    struct nn_ofi_mrm_chunk * chunk;
    nn_mutex_lock( &self->sync );

    /* Pick most appropriate chunk */
    chunk = pick_chunk( self, nn_chunkref_getchunk(chunkref) );
    if (!chunk) {
        nn_mutex_unlock( &self->sync );
        return -ENOMEM;
    }

    /* First populate mrmc pointer because it's referred later */
    *mrmc = chunk;

    /* Populate chunk details */
    chunk->data.mr_iov[0].iov_base = chunk->ancillary;
    chunk->data.mr_iov[1].iov_base = nn_chunkref_data( chunkref );
    chunk->data.mr_desc[0] = fi_mr_desc( self->mr_ancillary );
    chunk->data.mr_desc[1] = fi_mr_desc( chunk->mr );

    /* Lock chunk */
    chunk->flags |= NN_OFI_MRM_FLAG_LOCKED;
    nn_mutex_unlock( &self->sync );
    return 0;
}

/* Release the memory region chunk */
int nn_ofi_mrm_unlock( struct nn_ofi_mrm * self, struct nn_ofi_mrm_chunk * chunk )
{
    nn_mutex_lock( &self->sync );
    /* Unlock memory region */
    nn_assert( chunk->flags & NN_OFI_MRM_FLAG_LOCKED );    
    chunk->flags &= ~NN_OFI_MRM_FLAG_LOCKED;
    nn_mutex_unlock( &self->sync );
    return 0;
}

/* Check if there are no locked chunks */
int nn_ofi_mrm_isidle( struct nn_ofi_mrm * self )
{
    nn_mutex_lock( &self->sync );
    /* Iterate over chunks and free allocated */
    for (struct nn_ofi_mrm_chunk * mr = self->chunks, 
            * mr_end = self->chunks + self->len;
         mr < mr_end; mr++ ) {

        /* Found at least one locked chunk, we are not idle */
        if (mr->flags & NN_OFI_MRM_FLAG_LOCKED) {
            nn_mutex_unlock( &self->sync );
            return 0;
        }

    }

    /* We are idle */
    nn_mutex_unlock( &self->sync );
    return 1;
}

/* Check if there are no unlocked chunks */
int nn_ofi_mrm_isfull( struct nn_ofi_mrm * self )
{
    nn_mutex_lock( &self->sync );
    /* Iterate over chunks and free allocated */
    for (struct nn_ofi_mrm_chunk * mr = self->chunks, 
            * mr_end = self->chunks + self->len;
         mr < mr_end; mr++ ) {

        /* Found at least one locked chunk, we are not idle */
        if (!(mr->flags & NN_OFI_MRM_FLAG_LOCKED)) {
            nn_mutex_unlock( &self->sync );
            return 0;
        }

    }

    /* We are idle */
    nn_mutex_unlock( &self->sync );
    return 1;
}


/* Manage memory region */
static int nn_ofi_mrm_manage ( struct nn_ofi_mrm * self, 
    struct nn_ofi_mrm_chunk * chunk, struct nn_chunk_desc * desc )
{
    int ret;
    int offset;

    /* Calculate key offset */
    offset = (int)(chunk - self->chunks) + 1;
    _ofi_debug("OFI[-]: Managing MRM base=%p chunk=%p index=%i\n", self->chunks, chunk, offset);
    nn_assert( offset > 0 );

    /* Register memory region */
    nn_assert( desc->base );
    nn_assert( desc->len > 0 );
    _ofi_debug("OFI[-]: Registering ptr=%p, len=%lu, key=0x%08x\n", 
        desc->base, desc->len, self->base_key + offset);
    ret = fi_mr_reg( self->ep->domain, desc->base, desc->len, 
        self->access_flags, 0, self->base_key + offset, 0, &chunk->mr, NULL);
    if (ret) {
        FT_PRINTERR("nn_ofi_mrm_manage->fi_mr_reg", ret);
        return ret;
    }

    /* Update flags and properties */
    chunk->flags |= NN_OFI_MRM_FLAG_ASSIGNED;
    chunk->age = ++self->age;
    memcpy( &chunk->desc, desc, sizeof(struct nn_chunk_desc) );

    /* Success */
    return 0;
}

/* Unmanage a memory region */
static int nn_ofi_mrm_unmanage ( struct nn_ofi_mrm * self,
    struct nn_ofi_mrm_chunk * chunk )
{
    _ofi_debug("OFI[-]: Unmanaging MRM chunk=%p\n", chunk);

    /* Close memory region and free descriptor */
    FT_CLOSE_FID( chunk->mr );

    /* Reset properties */
    chunk->flags &= ~NN_OFI_MRM_FLAG_ASSIGNED;

    /* Success */
    return 0;
}

/* Pick most appropriate mr */
static struct nn_ofi_mrm_chunk * pick_chunk( struct nn_ofi_mrm * self, 
    void *chunk )
{
    struct nn_ofi_mrm_chunk * oldest_mr = NULL;
    struct nn_ofi_mrm_chunk * free_mr = NULL;
    struct nn_chunk_desc desc;
    int oldest_age = -1;
    _ofi_debug("OFI[-]: Picking MRM chunk for ptr=%p\n", chunk);

    /* Get chunk description */
    nn_assert ( nn_chunk_describe( chunk, &desc ) == 0 );

    /* Iterate over chunks */
    for (struct nn_ofi_mrm_chunk * mr = self->chunks, 
            * mr_end = self->chunks + self->len;
         mr < mr_end; mr++ ) {

        /* If this is an unlocked MR, account it for picking by age */
        if (!(mr->flags & NN_OFI_MRM_FLAG_LOCKED)) {
            if (( oldest_age == -1 ) || ( mr->age < oldest_age )) {
                oldest_age = mr->age;
                oldest_mr = mr;
            }
        }

        /* If this is an unasigned, unlocked MR, prefer this if nothing found */
        if (!free_mr && (mr->flags == 0)) {
            free_mr = mr;
        }

        /* If this MR is already managed, return it as-is */
        if (( desc.base == mr->desc.base ) && ( desc.len == mr->desc.len )) {

            /* TODO: What happens if base ptr is re-allocated in the same
                     base address? */
            _ofi_debug("OFI[-]: Found MRM match chunk=%p\n", mr);
            return mr;

        }

    }

    /* Check if we have a free slot */
    if (free_mr) {
        _ofi_debug("OFI[-]: Found MRM free chunk=%p\n", free_mr);

        /* Manage memory region */
        nn_ofi_mrm_manage( self, free_mr, &desc );
        return free_mr;

    }

    /* Check if we have a candidate for re-use */
    if (oldest_mr) {
        _ofi_debug("OFI[-]: Reusing MRM chunk=%p, age=%i\n", oldest_mr, 
            oldest_mr->age);

        /* Free MR and check for errors */
        if (nn_ofi_mrm_unmanage( self, oldest_mr ))
            return NULL;

        /* If successful, that's our MR */
        nn_ofi_mrm_manage( self, oldest_mr, &desc );
        return oldest_mr;

    }

    /* Nothing found */
    return NULL;

}
