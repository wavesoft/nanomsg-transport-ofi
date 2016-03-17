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

#define FT_PRINTERR(call, retv) \
	do { fprintf(stderr, "OFI: Error on " call "(): %s:%d, ret=%d (%s)\n", __FILE__, __LINE__, (int) retv, fi_strerror((int) -retv)); } while (0)

#define FT_CLOSE_FID(fd)			\
	do {					\
		if ((fd)) {			\
			int ret = fi_close(&(fd)->fid);	\
			if (ret) { \
				if (ret == -FI_EBUSY) { \
					printf("OFI[H]: *** Error closing FD " #fd " (FI_EBUSY)\n"); \
				} else { \
					printf("OFI[H]: *** Error closing FD " #fd " caused error = %i\n", ret); \
				} \
			} \
			fd = NULL;		\
		}				\
	} while (0)

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

#ifdef OFI_USE_WAITSET

	/* CQ & EQ Waitset */
	struct fid_wait		*waitset;

#endif

	/* Addresses */
	fi_addr_t 			remote_fi_addr;

	/* Size of prefix */
	unsigned char 		tx_prefix_size;
	unsigned char		rx_prefix_size;

	/* Sixe of Rx/Tx Queues */
	size_t 				tx_size;
	size_t 				rx_size;

	/* For fast poller loops */
	uint32_t 			kinstructions_per_ms;

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

	/* Fabric core structures */
	struct fi_info 		*fi;
	struct fid_fabric 	*fabric;

	/* For fast poller loops */
	uint32_t 			kinstructions_per_ms;

	/* Last error */
	int 				err;

};

/**
 * OFI Ring Buffers
 */
struct ofi_ringbuffers
{
	/* Base pointer to the data structure */
	void * 				buffer;

	/* Total buffer size and chunk size */
	size_t 				total_size;
	size_t 				chunk_size;
	
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
 * A memory region managed by HLAPI MR
 */
struct ofi_mr {
	void * 				ptr;
	struct fid_mr 		*mr;
};

/**
 * High-level shared memory region management
 */
struct ofi_mr_block {

	/* Maximum number of memory regions allowed */
	uint32_t				max_regions;

	/* Already allocated memory regions */
	struct ofi_mr_region * 	regions;

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
 * Free hints and core structures
 */
int ofi_free( struct ofi_resources * R );

/**
 * Scattter-gather version
 */
ssize_t ofi_tx_msg( struct ofi_active_endpoint * EP, const struct iovec *msg_iov, void ** msg_iov_desc, 
	size_t iov_count, uint64_t flags, int timeout );
ssize_t ofi_rx_msg( struct ofi_active_endpoint * EP, const struct iovec *msg_iov, void ** msg_iov_desc, 
		size_t iov_count, size_t * rx_size, uint64_t flags, int timeout );

/**
 * Single pointer version
 */
ssize_t ofi_rx_data( struct ofi_active_endpoint * EP, void * buf, const size_t max_size, 
		void *desc, size_t * rx_size, int timeout );
ssize_t ofi_tx_data( struct ofi_active_endpoint * EP, void * buf, const size_t tx_size, 
		void *desc, int timeout );

/**
 * Post/Poll receive version
 */
int ofi_rx_post( struct ofi_active_endpoint * EP, void * buf, const size_t max_size, void *desc );
int ofi_rx_poll( struct ofi_active_endpoint * EP, size_t * rx_size, uint32_t timeout );

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
int ofi_mr_init( struct ofi_active_endpoint * ep, struct ofi_mr * mr );
int ofi_mr_manage( struct ofi_active_endpoint * EP, struct ofi_mr * mr, void * buf, size_t len, int requested_key, enum ofi_mr_flags flags );
int ofi_mr_unmanage( struct ofi_active_endpoint * ep, struct ofi_mr * mr );
int ofi_mr_free( struct ofi_active_endpoint * ep, struct ofi_mr * mr );
#define OFI_MR_DESC(x) fi_mr_desc( x.mr )

#endif /* NN_OFI_SHARED_INCLUDED */
