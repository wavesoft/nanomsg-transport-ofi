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

/**
 * OFI Active Endpoint
 */
struct ofi_active_endpoint
{

	/* Domain */
	struct fid_domain 	*domain;

	/* Memory Region for I/O */
	size_t 				buf_size, tx_size, rx_size;
	void 				*buf, *tx_buf, *rx_buf;
	struct fid_mr 		*mr;

	/* Active endpoint structures */
	struct fid_av 		*av;
	struct fid_ep 		*ep;
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
	struct fid_eq 		*eq;

};

/**
 * Allocate hints and prepare core structures
 */
int ofi_alloc( struct ofi_resources * R, enum fi_ep_type ep_type );

/**
 * Receive data from OFI
 */
ssize_t ofi_tx( struct ofi_active_endpoint * R, size_t size );

/**
 * Receive data from OFI
 */
ssize_t ofi_rx( struct ofi_active_endpoint * R, size_t size );

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


#endif /* NN_OFI_SHARED_INCLUDED */
