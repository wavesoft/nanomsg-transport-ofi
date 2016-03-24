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

#include <string.h>

#include "ofi.h"
#include "ofimr.h"

#include "../../utils/alloc.h"
#include "../../utils/cont.h"
#include "../../utils/err.h"

/* ########################################################################## */
/*  Helper Functions                                                          */
/* ########################################################################## */

/**
 * Unregister the memory region associated with the given bank
 *
 * WARNING: This function assumes that a mutex is already acquired for the bank!
 */
static int ofi_mr_unregister( struct ofi_mr_manager * self,
	struct ofi_mr_bank * bank )
{
	int ret;

	/* Make sure it's registered */
	if (!(bank->flags & OFI_MR_BANK_REGISTERED)) {
		return 0;
	}

	/* Try to close the MR FID */
	_ofi_debug("OFI[M]: Unregistering base=%p, len=%lu\n",
		bank->base, bank->len);
	ret = fi_close(&bank->mr->fid);
	if (ret) {
		return ret;
	}

	/* Remove registration flag */
	bank->flags &= ~OFI_MR_BANK_REGISTERED;

	/* Reset bank */
	bank->base = NULL;
	bank->len = 0;

	/* Success */
	return 0;
}

/**
 * Register the memory region associated with the given bank
 *
 * WARNING: This function assumes that a mutex is already acquired for the bank!
 */
static int ofi_mr_register( struct ofi_mr_manager * self,
	struct ofi_mr_bank * bank )
{
	int ret;
    int index;

	/* Unregister bank if it's already registered */
	if (bank->flags & OFI_MR_BANK_REGISTERED) {

		/* Try to unregister and return in case there was a failure */
		ret = ofi_mr_unregister( self, bank );
		if (ret) {
			return ret;
		}
	}

	/* Calculate the index of this bank */
	index = (int)(bank - &self->banks[0]);
	_ofi_debug("OFI[M]: Registering base=%p, len=%lu, key=%04llx\n",
		bank->base, bank->len, self->base_key+index);

	/* Try to register the memory region */
    ret = fi_mr_reg(self->domain->domain, bank->base, bank->len, 
        self->access_flags, 0, self->base_key + index, 0, 
        &bank->mr, NULL);
    if (ret) {
    	FT_PRINTERR("fi_mr_reg", ret);
    	return ret;
    }

    /* Mark this memory region as registered */
    bank->flags |= OFI_MR_BANK_REGISTERED;

    /* Update bank's age */
    bank->age = ++self->age;

    /* Success */
    return 0;
}

/**
 * Find a MR bank that has a reference counter equal to zero
 *
 * WARNING: This function will *NOT* release the acquired mutex upon successful	
 * 			completion!
 */
static int ofi_mr_get_free_bank( struct ofi_mr_manager * self, 
	struct ofi_mr_bank ** pick_bank )
{
	int i;

	/* Initialize banks */
	for (i=0; i<self->size; ++i) {
		struct ofi_mr_bank * bank = &self->banks[i];
		nn_mutex_lock(&bank->mutex);

		/* Look for a free bank */
		if ((bank->ref == 0) && (bank->flags == 0)) {

			/* DO NOT release the mutex, since the function that
			   called us will most probably keep working on the bank */
			*pick_bank = bank;
			return 0;

		}		

		nn_mutex_unlock(&bank->mutex);
	}

	/* Out of memory */
	return -ENOMEM;
}

/**
 * Find a MR bank that can contain the specified memory region
 */
static int ofi_mr_find_bank( struct ofi_mr_manager *self, void *ptr, size_t len,
	struct ofi_mr_bank ** pick_bank )
{
	int i, ret;
	uint32_t age = self->banks[0].age;
	void * ptr_end = ((uint8_t*)ptr) + len;
	struct ofi_mr_bank * oldest_bank = NULL;

	/* Initialize banks */
	for (i=0; i<self->size; ++i) {
		struct ofi_mr_bank * bank = &self->banks[i];
		void * bank_end = ((uint8_t*)bank->base) + bank->len;
		nn_mutex_lock(&bank->mutex);

		/* Check if the specified pointer is within the bank range */
		if ((ptr >= bank->base) && (ptr_end <= bank_end)) {
			/* Unlock oldest bank mutex since we found a better match */
			if (oldest_bank) nn_mutex_unlock( &oldest_bank->mutex );

			/* DO NOT release the mutex, since the function that
			   called us will most probably keep working on the bank */

			*pick_bank = bank;
			return 0;
		}

		/* Also check for the oldest, non-reserved, non-volatile bank */
		if ((bank->ref == 0) && !(bank->flags & OFI_MR_BANK_NONVOLATILE)) {
			if (!oldest_bank) {
				oldest_bank = bank;
			} else {
				/* Pick the one with the smallest age (oldest) */
				if (bank->age < oldest_bank->age) {

					/* First unlock the mutex of the oldest bank, since
					   the test on the end of the loop prohibited it
					   from being unlocked. That's intentional since
					   we don't know if we are going to use the oldest_bank
					   until the oldest one is correctly picked. */
					nn_mutex_unlock( &oldest_bank->mutex );

					/* Then replace oldest bank */
					oldest_bank = bank;

				}
			}
		}

		/* Don't unlock the mutex of the oldest bank, until we are
		   sure we are not going to use it. */
		if (oldest_bank != bank)
			nn_mutex_unlock(&bank->mutex);
	}

	/* Check if we found an old memory region that we can re-use */
	if (oldest_bank) {

		/* Update bank region */
		oldest_bank->base = ptr;
		oldest_bank->len = len;

		/* Register to the new region */
		ret = ofi_mr_register( self, oldest_bank );
		if (ret) {
			return ret;
		}

		/* DO NOT release the mutex, since the function that
		   called us will most probably keep working on the bank */

		/* We found our bank */
		*pick_bank = oldest_bank;
		return 0;

	}

	/* No free regions, try again later */
	return -EAGAIN;

}

/* ########################################################################## */
/*  Interface Functions                                                       */
/* ########################################################################## */

/**
 * Initialize the memory region manager with the specified capacity of memory
 * registration banks.
 */
int ofi_mr_init( struct ofi_mr_manager * self, struct ofi_domain *domain, 
	size_t size, enum ofi_mr_direction direction, uint64_t base_key )
{
	int i;

	/* Allocate memory for the banks */
	self->banks = nn_alloc( sizeof(struct ofi_mr_bank) * size, "ofi mrm banks");
	nn_assert(self->banks);

	/* Initialize properties */
	self->size = size;
	self->age = 0;
	self->domain = domain;
	self->base_key = base_key;

	/* Initialize mr access flags */
	switch (direction) {
		case OFI_MR_DIR_SEND:
			self->access_flags = FI_SEND | FI_WRITE | FI_REMOTE_READ;
			break;
		case OFI_MR_DIR_RECV:
			self->access_flags = FI_RECV | FI_READ | FI_REMOTE_WRITE;
			break;
		case OFI_MR_DIR_BOTH:
			self->access_flags = FI_RECV | FI_READ | FI_REMOTE_WRITE
							   | FI_SEND | FI_WRITE | FI_REMOTE_READ;
			break;
	}

	/* Initialize banks */
	for (i=0; i<self->size; ++i) {
		struct ofi_mr_bank * bank = &self->banks[i];

		/* Initialize properties */
		bank->base = NULL;
		bank->len = 0;
		bank->age = 0;
		bank->flags = 0;
		bank->ref = 0;

		/* Initialize structures */
		nn_mutex_init( &bank->mutex );
	}

	/* Success */
	return 0;

}

/**
 * Clean-up the memory region manager resources
 */
int ofi_mr_term( struct ofi_mr_manager * self )
{
	int i;

	/* Free banks */
	for (i=0; i<self->size; ++i) {
		struct ofi_mr_bank * bank = &self->banks[i];
		nn_mutex_lock( &bank->mutex );

		/* No banks must be reserved */
		nn_assert( bank->ref == 0 );

		/* Unregister memory regions */
		ofi_mr_unregister( self, bank );

		/* Terminate structures */
		nn_mutex_unlock( &bank->mutex );
		nn_mutex_term( &bank->mutex );

	}

	/* Free memory */
	nn_free(self->banks);

	/* Success */
	return 0;
}

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
 */
int ofi_mr_mark( struct ofi_mr_manager * self, void * base, size_t len )
{
	struct ofi_mr_bank * bank;
	int ret;

	/* Find a free bank */
	ret = ofi_mr_get_free_bank( self, &bank );
	if (ret) {
		return ret;
	}

	/* Define it and make it non-volatile */
	bank->base = base;
	bank->len = len;
	bank->flags = OFI_MR_BANK_NONVOLATILE;

	/* Register it */
	ret = ofi_mr_register( self, bank );
	if (ret) {
		nn_mutex_lock(&bank->mutex);
		return ret;
	}

	/* Success */
	nn_mutex_lock(&bank->mutex);
	return 0;
}

/**
 * Invalidate the specified memory region, forcing intersecting memory
 * registrations to be released.
 *
 * This is useful when you are releasing a pointer previously registered
 * with `ofi_mr_mark`.
 */
int ofi_mr_invalidate( struct ofi_mr_manager * self, void * base, size_t len )
{
	int i;
	void * ptr_end = ((uint8_t*)base) + len;

	/* Invalidate intersecting banks */
	for (i=0; i<self->size; ++i) {
		struct ofi_mr_bank * bank = &self->banks[i];
		nn_mutex_lock( &bank->mutex );
		void * bank_end = ((uint8_t*)bank->base) + bank->len;

		/* Check if bank intersects with the memory region specified */
		if ( ((base >= bank->base) && (base <= bank_end)) ||
			 ((ptr_end >= bank->base) && (ptr_end <= bank_end)) ||
			 ((bank->base >= base) && (bank->base <= ptr_end)) ||
			 ((bank_end >= base) && (bank_end <= ptr_end)) ) {

			/* If this memory region is in use return EBUSY */
			if (bank->ref > 0) {
				nn_mutex_unlock( &bank->mutex );
				return -EBUSY;
			}

			/* Unregister */
			ofi_mr_unregister( self, bank );

			/* Reset non-volatile flag (if any) */
			bank->flags &= ~OFI_MR_BANK_NONVOLATILE;

		}

		nn_mutex_unlock( &bank->mutex );
	}

	/* Success */
	return 0;
}

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
int ofi_mr_describe( struct ofi_mr_manager * self, struct fi_msg * msg )
{
	int i;
	int ret;
	struct ofi_mr_context * context;
	struct ofi_mr_bank * bank;

	/* Perform some obvious tests */
	if (msg->iov_count > OFI_MR_MAX_BANKSPERCONTEXT)
		return -E2BIG;

	/* Allocate a context for this operation */
	context = nn_alloc( sizeof(struct ofi_mr_context), "ofi mr context" );
	nn_assert( context );
	memset( context, 0, sizeof(struct ofi_mr_context) );

	/* Prepare properties */
	context->user_context = msg->context;

	/* Allocate the memory that will hold the memory region descriptors */
	context->descriptors = nn_alloc( sizeof(void*) * msg->iov_count, 
		"ofi mr descriptors" );
	nn_assert( context->descriptors );

	/* Lookup memory regions */
	for (i=0; i<msg->iov_count; ++i) {

		/* If this is an empty iov, don't populate descriptor */
		if (msg->msg_iov[i].iov_len == 0) {

			/* Update context */
			context->banks[ context->size++ ] = NULL;
			context->descriptors[i] = NULL;

		} else {

			/* Find or register a compatible bank */
			ret = ofi_mr_find_bank( self, msg->msg_iov[i].iov_base, 
				msg->msg_iov[i].iov_len, &bank );
			if (ret) {
				/* Unable to find a bank, clean what we did so far */
				goto err;
			}

			/* Increment bank's reference counter */
			bank->ref++;

			/* Update context */
			context->banks[ context->size++ ] = bank;
			context->descriptors[i] = fi_mr_desc( bank->mr );

			/* Unlock bank mutex */
			nn_mutex_unlock( &bank->mutex );

		}

	}

	/* Update message structures */
	msg->desc = context->descriptors;
	msg->context = context;

	/* Success */
	return 0;

err:

	/* Release the memory regions reserved */
	for (i=0; i<context->size; i++) {
		bank = context->banks[i];

		/* Acquire bank mutex */
		nn_mutex_lock( &bank->mutex );

		/* Decrease reference counter */
		bank->ref--;

		/* Release bank mutex */
		nn_mutex_unlock( &bank->mutex );

	}
	
	/* Failed */
	return ret;
}

/**
 * Re-use the memory registration banks used for the particular transmission
 * or reception operation.
 * 
 * This function will also replace the passed context with the original
 * context the user specified in the `fi_msg` structure.
 */
int ofi_mr_release( void ** context )
{
	int i;
	struct ofi_mr_context * ctx = (struct ofi_mr_context *) *context;
	struct ofi_mr_bank * bank;

	/* Restore user context */
	*context = ctx->user_context;

	/* Decrement the reference counters to the memory banks */
	for (i=0; i<ctx->size; i++) {
		bank = ctx->banks[i];
		if (!bank) continue;

		/* Acquire bank mutex */
		nn_mutex_lock( &bank->mutex );

		/* Decrease reference counter */
		bank->ref--;

		/* Release bank mutex */
		nn_mutex_unlock( &bank->mutex );

	}

	/* Free memory */
	nn_free(ctx->descriptors);
	nn_free(ctx);

	/* Success */
	return 0;
}




