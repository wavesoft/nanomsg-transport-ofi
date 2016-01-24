/**
 * NanoMsg libFabric Transport - Shared Functions
 * Copyright (c) 2015 Ioannis Charalampidis
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef NN_OFI_SHARED_INCLUDED
#define NN_OFI_SHARED_INCLUDED

#include <time.h>
#ifdef __APPLE__
#include "platf/osx.h"
#endif

#include <rdma/fabric.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>

/* Maximum buffer size to allocate */
#define MAX_MSG_SIZE 		1024

/* Control message size (Smallest packet size) */
#define FT_MAX_CTRL_MSG 	64

/* LibFabric Protocol to use */
#define FT_FIVERSION 		FI_VERSION(1,1)

/* Error flag that denotes that remote socket disconnected */
#define FI_REMOTE_DISCONNECT	513

/* Forward declarations */
struct ofi_mr;

/**
 * OFI Active Endpoint
 */
struct ofi_active_endpoint
{

	/* Domain */
	struct fid_domain 	*domain;

	/* Active endpoint structures */
	struct fid_av 		*av;
	struct fid_ep 		*ep;
	struct fid_eq 		*eq;
	struct fid_cq 		*tx_cq, *rx_cq;
	struct fi_context 	tx_ctx, rx_ctx;

	/* Addresses */
	fi_addr_t 			remote_fi_addr;

	/* Size of prefix */
	unsigned char 		tx_prefix_size;
	unsigned char		rx_prefix_size;

};

/**
 * OFI Passive Endpoint
 */
struct ofi_passive_endpoint
{
	/* Passive endpoint when listening */
	struct fid_pep  	*pep;
	struct fid_eq 		*eq;
};

/**
 * OFI Resources Structure
 */
struct ofi_resources
{
	/* Hints and attributes */
	struct fi_info 		*hints;
	uint64_t			flags;

	/* Fabric core structures */
	struct fi_info 		*fi;
	struct fid_fabric 	*fabric;

};

/**
 * Precision of the get_elapsed function
 */
enum ofi_time_precision {
	NANO = 1,
	MICRO = 1000,
	MILLI = 1000000,
};

/**
 * Smart memory region
 */
enum ofi_mr_flags {
	MR_SEND = 0x01,
	MR_RECV = 0x02
};

/**
 * High-level shared memory regions
 */
struct ofi_mr {

	/* Hinting information */
	void * 				ptr;

	/* Pointer objects */
	struct fid_mr 		*mr;

};

// /**
//  * Helper function to allocate the receiving IO Vectors
//  */
// int (*ofi_alloc_iov)( size_t msgLen, struct iovec **msg_iov, void **msg_iov_desc, size_t *iov_count );

/**
 * Allocate hints and prepare core structures
 */
int ofi_alloc( struct ofi_resources * R, enum fi_ep_type ep_type );

/**
 * OFI messages Rx/Tx
 */
ssize_t ofi_tx_msg( struct ofi_active_endpoint * EP, const struct iovec *msg_iov, void ** msg_iov_desc, 
	size_t iov_count, uint64_t flags, int timeout );
ssize_t ofi_rx_msg( struct ofi_active_endpoint * EP, const struct iovec *msg_iov, void ** msg_iov_desc, 
		size_t iov_count, size_t * rx_size, uint64_t flags, int timeout );

ssize_t ofi_rx_waitmsg( struct ofi_active_endpoint * EP, int timeout  );
ssize_t ofi_rx_postmsg( struct ofi_active_endpoint * EP, const struct iovec *msg_iov, void ** msg_iov_desc, 
		size_t iov_count, uint64_t flags );

// /**
//  * Tagged ofi Rx/Tx with additional control information
//  */
// ssize_t ofi_ttx_msg( struct ofi_active_endpoint * EP, const struct iovec *msg_iov, void ** msg_iov_desc,
// 	size_t iov_count, uint64_t flags, int timeout );
// ssize_t ofi_trx_msg( struct ofi_active_endpoint * EP, const struct iovec *msg_iov, void ** msg_iov_desc,
// 	size_t iov_count, uint64_t flags, int timeout );

/**
 * Resolve an address
 */
int ofi_resolve_address( struct ofi_resources * R, const char * node, const char * service, void ** addr, size_t * addr_len );

/**
 * Initialize as a connectionless endpoint, bound on 
 * the specified node/service
 */
int ofi_init_connectionless( struct ofi_resources * R, struct ofi_active_endpoint * EP, uint64_t flags, 
							 unsigned int addr_format, const char * node, const char * service );

/**
 * Specify and configure remote address
 */
int ofi_add_remote( struct ofi_resources * R, struct ofi_active_endpoint * EP, 
					const char * node, const char * service );

/**
 * Create a bound socket and listen for incoming connections
 */
int ofi_init_server( struct ofi_resources * R, struct ofi_passive_endpoint * PEP, unsigned int addr_format, 
					const char * node, const char * service );

/**
 * Wait for incoming connections and accept
 */
int ofi_server_accept( struct ofi_resources * R, struct ofi_passive_endpoint * PEP, struct ofi_active_endpoint * EP );

/**
 * Initialize memory regions of an active endpoint (use this before tx/rx operations!)
 */
int ofi_active_ep_init_mr( struct ofi_resources * R, struct ofi_active_endpoint * EP, size_t rx_size, size_t tx_size );

/**
 * Initialize as a connectionless endpoint, bound on 
 * the specified node/service
 */
int ofi_init_client( struct ofi_resources * R, struct ofi_active_endpoint * EP, unsigned int addr_format, 
					const char * node, const char * service );

/**
 * Shutdown an active endpoint
 */
int ofi_shutdown_ep( struct ofi_active_endpoint * EP );

/**
 * Shutdown an passive endpoint
 */
int ofi_shutdown_pep( struct ofi_passive_endpoint * PEP );

/**
 * Free hints and core structures
 */
int ofi_free( struct ofi_resources * R );

/**
 * Free passive endpoint
 */
int ofi_free_pep( struct ofi_passive_endpoint * ep );

/**
 * Free active endpoint
 */
int ofi_free_ep( struct ofi_active_endpoint * ep );

/**
 * Return elapsed time in microseconds
 */
int64_t ofi_get_elapsed(const struct timespec *b, const struct timespec *a,
		    enum ofi_time_precision p);

/**
 * Shared region managements
 */
int ofi_mr_alloc( struct ofi_active_endpoint * ep, struct ofi_mr ** mr );
int ofi_mr_manage( struct ofi_active_endpoint * EP, struct ofi_mr * mr, void * buf, size_t len, int requested_key, enum ofi_mr_flags flags );
int ofi_mr_unmanage( struct ofi_active_endpoint * ep, struct ofi_mr * mr );
int ofi_mr_free( struct ofi_active_endpoint * ep, struct ofi_mr ** mr );

#endif /* NN_OFI_SHARED_INCLUDED */
