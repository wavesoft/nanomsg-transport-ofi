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
static int pick_chunk( struct nn_ofi_mrm * self, 
    void *chunk, struct nn_ofi_mrm_chunk ** result );
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

        /* Initialize EFD */
        nn_efd_init( &mr->efd );

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

        /* Free EFD */
        nn_efd_term( &mr->efd );
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
    int ret;
    struct nn_ofi_mrm_chunk * chunk;
    nn_mutex_lock( &self->sync );

    while (1) {

        /* Pick most appropriate chunk */
        ret = pick_chunk( self, nn_chunkref_getchunk(chunkref), &chunk );

        /* Check if this MR is busy */
        if (ret == -EAGAIN) {

            /* Wait for the MR to unlock */
            nn_mutex_unlock( &self->sync );
            nn_efd_wait( &chunk->efd, -1 );
            nn_mutex_lock( &self->sync );

        /* Check on errors */
        } else if (ret < 0) {
            nn_mutex_unlock( &self->sync );
            return ret;
        
        /* Break on ret=0 */
        } else {
            break;

        }

    }

    /* This SHOULD NOT be locked! */
    nn_assert( ! (chunk->flags & NN_OFI_MRM_FLAG_LOCKED) );

    /* First populate mrmc pointer because it's referred later */
    *mrmc = chunk;

    /* Populate chunk details */
    chunk->data.mr_iov[0].iov_base = chunk->ancillary;
    chunk->data.mr_iov[1].iov_base = nn_chunkref_data( chunkref );
    chunk->data.mr_desc[0] = fi_mr_desc( self->mr_ancillary );
    chunk->data.mr_desc[1] = fi_mr_desc( chunk->mr );

    /* Lock chunk */
    _ofi_debug("--LOCKING %p--\n", chunk);

    chunk->flags |= NN_OFI_MRM_FLAG_LOCKED;
    nn_efd_unsignal( &chunk->efd );
    nn_mutex_unlock( &self->sync );
    return 0;
}

/* Release the memory region chunk */
int nn_ofi_mrm_unlock( struct nn_ofi_mrm * self, struct nn_ofi_mrm_chunk * chunk )
{
    nn_mutex_lock( &self->sync );
    /* Unlock memory region */
    _ofi_debug("OFI[-]: Unlocking MR chunk %p\n", chunk);
    nn_assert( chunk->flags & NN_OFI_MRM_FLAG_LOCKED );    

    _ofi_debug("--UNLOCKING %p--\n", chunk);
    chunk->flags &= ~NN_OFI_MRM_FLAG_LOCKED;
    nn_efd_signal( &chunk->efd );
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
static int pick_chunk( struct nn_ofi_mrm * self, 
    void *chunk, struct nn_ofi_mrm_chunk ** result )
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

        /* If this is an unlocked MR, it's a candidate for pick-up */
        if (!(mr->flags & NN_OFI_MRM_FLAG_LOCKED)) {

            /* Account it for oldest-picking */
            if (( oldest_age == -1 ) || ( mr->age < oldest_age )) {
                oldest_age = mr->age;
                oldest_mr = mr;
            }

        }

        /* If this is an unasigned, unlocked MR, prefer this if nothing found */
        if (!free_mr && (mr->flags == 0)) {
            free_mr = mr;
        }

        /* Check if this MR is already managed */
        if (( desc.base == mr->desc.base ) && ( desc.len == mr->desc.len )) {

            /* We are processing this differently if this chunk is locked */
            if (mr->flags & NN_OFI_MRM_FLAG_LOCKED) {

                /* If this is exactly the same MR, this is bad... it means that the
                   user tried to send the same pointer for the second time, but the
                   previous one wasn't sent already.

                   Effectively, this means that the user has possibly changed the data
                   in memory and the previous request is now in unspecified state.

                   There is no way to solve this, just wait for the previous MR to be
                   released. */

                _ofi_debug("OFI[-]: Found overlapping, locked MRM chunk=%p\n", mr);
                *result = mr;
                return -EAGAIN;

            } else {

                /* If the chunk we know is invalidated (ex. free'd and malloc'd at the same
                    location), re-manage the same region */
                if (desc.id != mr->desc.id) {

                    /* That's our target */
                    _ofi_debug("OFI[-]: Found MRM chunk=%p with different ID (%u <-> %u)\n", 
                        mr, desc.id, mr->desc.id);
                    oldest_mr = mr;
                    free_mr = NULL;
                    break;

                } else {

                    /* We found a properly managed address */
                    _ofi_debug("OFI[-]: Found MRM match chunk=%p\n", mr);
                    *result = mr;
                    return 0;

                }

            }

        }

    }

    /* Check if we have a free slot */
    if (free_mr) {
        _ofi_debug("OFI[-]: Found MRM free chunk=%p\n", free_mr);

        /* Manage memory region */
        nn_ofi_mrm_manage( self, free_mr, &desc );
        *result = free_mr;
        return 0;

    }

    /* Check if we have a candidate for re-use */
    if (oldest_mr) {
        _ofi_debug("OFI[-]: Reusing MRM chunk=%p, age=%i\n", oldest_mr, 
            oldest_mr->age);

        /* Free MR and check for errors */
        if (nn_ofi_mrm_unmanage( self, oldest_mr )) {
            *result = NULL;
            return 0;
        }

        /* If successful, that's our MR */
        nn_ofi_mrm_manage( self, oldest_mr, &desc );
        *result = oldest_mr;
        return 0;

    }

    /* Nothing found */
    *result = NULL;
    return -ENOMEM;

}
