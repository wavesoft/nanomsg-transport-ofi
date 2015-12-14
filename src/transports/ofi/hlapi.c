/**
 * NanoMsg libFabric Transport - High-Level API To libFabric
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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <netdb.h>
#include <unistd.h>

#ifndef __STANDALONE_TESTS
#include "../../utils/alloc.h"
#include "../../utils/err.h"
#else
#include <nn_standalone_func.h>
#endif

#include "hlapi.h"

#define FT_PRINTERR(call, retv) \
	do { fprintf(stderr, "OFI: Error on " call "(): %s:%d, ret=%d (%s)\n", __FILE__, __LINE__, (int) retv, fi_strerror((int) -retv)); } while (0)

#define FT_ERR(fmt, ...) \
	do { fprintf(stderr, "OFI: %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); } while (0)

#define FT_EP_BIND(ep, fd, flags)							\
	do {													\
		int ret;											\
		if ((fd)) {											\
			ret = fi_ep_bind((ep), &(fd)->fid, (flags));	\
			if (ret) {										\
				FT_PRINTERR("fi_ep_bind", ret);				\
				return ret;									\
			}												\
		}													\
	} while (0)

#define FT_PROCESS_QUEUE_ERR(readerr, rd, queue, fn, str)	\
	do {							\
		if (rd == -FI_EAVAIL) {				\
			readerr(queue, fn " " str);		\
		} else {					\
			FT_PRINTERR(fn, rd);			\
		}						\
	} while (0)

#define FT_PROCESS_EQ_ERR(rd, eq, fn, str) \
	FT_PROCESS_QUEUE_ERR(dbg_readerr, rd, eq, fn, str)

#define MAX(a,b) ((a>b) ? a : b)


//////////////////////////////////////////////////////////////////////////////////////////
// OFI Helper Functions
//////////////////////////////////////////////////////////////////////////////////////////

/**
 * Get Tx Prefix size acccording to the tx_attr
 */
size_t ft_tx_prefix_size( struct fi_info * fi )
{
	return (fi->tx_attr->mode & FI_MSG_PREFIX) ?
		fi->ep_attr->msg_prefix_size : 0;
}

/**
 * Get Rx Prefix size acccording to the rx_attr
 */
size_t ft_rx_prefix_size( struct fi_info * fi )
{
	return (fi->rx_attr->mode & FI_MSG_PREFIX) ?
		fi->ep_attr->msg_prefix_size : 0;
}

/**
 * Wait for completion queue
 */
int ft_wait(struct fid_cq *cq)
{
	struct fi_cq_entry entry;
	int ret;

	/* CQ entry based on configured format (i.e. FI_CQ_FORMAT_CONTEXT) */
	while (1) {
		ret = fi_cq_read(cq, &entry, 1);

		/* Operation failed */
		if (ret > 0) {
			/* Success */
			return 0;

		} else if (ret < 0 && ret != -FI_EAGAIN) {
			/* Error */
			if (ret == -FI_EAVAIL) {
				struct fi_cq_err_entry err_entry;

				/* Handle error */
				ret = fi_cq_readerr(cq, &err_entry, 0);
				printf("OFI: Error %s %s\n",
					fi_strerror(err_entry.err),
					fi_cq_strerror(cq, err_entry.prov_errno, err_entry.err_data, NULL, 0)
				);
				return ret;
			} else {
				FT_PRINTERR("fi_cq_read", ret);
			}

		}
	}
}

/**
 * Helper to duplicate address
 */
static int ft_dupaddr(void **dst_addr, size_t *dst_addrlen, void *src_addr, size_t src_addrlen)
{
	*dst_addr = nn_alloc (src_addrlen, "hofi");
    alloc_assert (*dst_addr);
	if (!*dst_addr) {
		FT_ERR("address allocation failed\n");
		return EAI_MEMORY;
	}
	*dst_addrlen = src_addrlen;
	memcpy(*dst_addr, src_addr, src_addrlen);
	return 0;
}

/**
 * Insert one or more addresses in the address vector specified
 */
int ft_av_insert(struct fid_av *av, void *addr, size_t count, fi_addr_t *fi_addr,
		uint64_t flags, void *context)
{
	int ret;

	/* Try to insert address */
	ret = fi_av_insert(av, addr, count, fi_addr, flags, context);
	if (ret < 0) {
		FT_PRINTERR("fi_av_insert", ret);
		return ret;
	} else if (ret != count) {
		FT_ERR("fi_av_insert: number of addresses inserted = %d;"
			       " number of addresses given = %zd\n", ret, count);
		return -EXIT_FAILURE;
	}

	return 0;
}

/**
 * List OFI providers
 */
int dbg_show_providers( struct fi_info *list )
{
	printf("OFI: Available fabrics as reported from libOFI:\n");

	// Iterate over the identified providers
	int i = 1;
	struct fi_info *curr = list;
	while (curr != NULL)
	{

		// Debug
		printf("OFI:  %2i) fabric='%s', provider='%s' %s\n", 
			i,
			curr->fabric_attr->name, 
			curr->fabric_attr->prov_name,
			(i == 1) ? "(SELECTED)" : " "
			);

		// Go to next
		curr = curr->next;
		i++;
	}

	// We are good
	return 0;
}

/**
 * Get the queue error
 */
void dbg_readerr(struct fid_eq *eq, const char *eq_str)
{
	struct fi_eq_err_entry eq_err;
	const char *err_str;
	int rd;

	rd = fi_eq_readerr(eq, &eq_err, 0);
	if (rd != sizeof(eq_err)) {
		FT_PRINTERR("fi_eq_readerr", rd);
	} else {
		err_str = fi_eq_strerror(eq, eq_err.prov_errno, eq_err.err_data, NULL, 0);
		fprintf(stderr, "%s: %d %s\n", eq_str, eq_err.err,
				fi_strerror(eq_err.err));
		fprintf(stderr, "%s: prov_err: %s (%d)\n", eq_str, err_str,
				eq_err.prov_errno);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
// OFI Low-Level Functions
//////////////////////////////////////////////////////////////////////////////////////////

/**
 * Allocate hints and prepare core structures
 */
int ofi_alloc( struct ofi_resources * R, enum fi_ep_type ep_type )
{

	/* Allocate hints */
	R->hints = fi_allocinfo();
	if (!R->hints) {
		printf("OFI: Unable to allocate hints structure!\n");
		return 255;
	}

	/* Setup hints capabilities and more */
	R->hints->ep_attr->type	= ep_type;
	R->hints->caps			= FI_MSG;
	R->hints->mode			= FI_CONTEXT | FI_LOCAL_MR;

	/* Prepare flags */
	R->flags = 0;

	/* Success */
	return 0;
}

/**
 * Receive data from OFI
 */
ssize_t ofi_tx( struct ofi_active_endpoint * R, size_t size )
{
	ssize_t ret;

	/* Send data */
	ret = fi_send(R->ep, R->tx_buf, size + R->tx_prefix_size,
			fi_mr_desc(R->mr), R->remote_fi_addr, &R->tx_ctx);
	if (ret) {
		FT_PRINTERR("fi_send", ret);
		return ret;
	}

	/* Wait for Tx CQ */
	ret = ft_wait(R->tx_cq);
	if (ret) {
		FT_PRINTERR("ft_wait<tx_cq>", ret);
		return ret;
	}

	/* Success */
	return 0;
}

/**
 * Receive data from OFI
 */
ssize_t ofi_rx( struct ofi_active_endpoint * R, size_t size )
{
	ssize_t ret;

	/* Receive data */
	ret = fi_recv(R->ep, R->rx_buf, size + R->rx_prefix_size, 
			fi_mr_desc(R->mr), 0, &R->rx_ctx);
	if (ret) {
		FT_PRINTERR("fi_send", ret);
		return ret;
	}

	/* Wait for Rx CQ */
	ret = ft_wait(R->rx_cq);
	if (ret) {
		FT_PRINTERR("ft_wait<rx_cq>", ret);
		return ret;
	}

	/* Success */
	return 0;
}


/**
 * Resolve an address
 */
int ofi_resolve_address( struct ofi_resources * R, const char * node, const char * service, void ** addr, size_t * addr_len )
{
	int ret;
	struct fi_info *fi;

	/* Get fabric info, containing destination address details */
	ret = fi_getinfo(FT_FIVERSION, node, service, 0, R->hints, &fi);
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		return ret;
	}

	/* Dupliate address according to flags */
	ret = ft_dupaddr(addr, addr_len, fi->dest_addr, fi->dest_addrlen);
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		return ret;
	}

	/* Free info */
	fi_freeinfo(fi);

	/* Success */
	return 0;
}

/**
 * Allocate and open fabric and domain
 *
 * The two arguments `hint_domain` and `hint_service` specify restrictions
 * to the selection logic of fi_
 */
int ofi_open_fabric( struct ofi_resources * R )
{
	int ret;

	/* 1) Open Fabric */
	ret = fi_fabric(R->fi->fabric_attr, &R->fabric, NULL);
	if (ret) {
		FT_PRINTERR("fi_fabric", ret);
		return ret;
	}

	/* 2) Open an event queue */
	struct fi_eq_attr eq_attr = {
		.wait_obj = FI_WAIT_UNSPEC
	};
	ret = fi_eq_open(R->fabric, &eq_attr, &R->eq, NULL);
	if (ret) {
		FT_PRINTERR("fi_eq_open", ret);
		return ret;
	}

	/* Debug */
	printf("OFI: Using fabric=%s, provider=%s\n", 
		R->fi->fabric_attr->name, 
		R->fi->fabric_attr->prov_name );

	/* Success */
	return 0;
}

/**
 * Allocate an active endpoint
 */
int ofi_open_active_ep( struct ofi_resources * R, struct ofi_active_endpoint * EP, struct fi_info * fi, int buffer_size )
{
	int ret;

	/* Open Domain */
	ret = fi_domain(R->fabric, fi, &EP->domain, NULL);
	if (ret) {
		FT_PRINTERR("fi_domain", ret);
		return ret;
	}

	/* Cache some information */
	EP->rx_prefix_size = ft_rx_prefix_size( fi );
	EP->tx_prefix_size = ft_tx_prefix_size( fi );

	/* ==== Open Completion Queues =============== */

	/* Prepare structures */
	struct fi_cq_attr cq_attr = {
		.wait_obj = FI_WAIT_NONE,
		.format = FI_CQ_FORMAT_CONTEXT
	};

	/* Create a Tx completion queue */
	cq_attr.size = fi->tx_attr->size;
	ret = fi_cq_open(EP->domain, &cq_attr, &EP->tx_cq, &EP->tx_ctx);
	if (ret) {
		FT_PRINTERR("fi_cq_open", ret);
		return ret;
	}

	/* Create a Rx completion queue */
	cq_attr.size = fi->rx_attr->size;
	ret = fi_cq_open(EP->domain, &cq_attr, &EP->rx_cq, &EP->rx_ctx);
	if (ret) {
		FT_PRINTERR("fi_cq_open", ret);
		return ret;
	}

	/* ==== Open Address Vector ================== */

	/* Prepare structure */
	struct fi_av_attr av_attr = {
		.type = FI_AV_MAP,
		.count = 1
	};

	/* If domain has a preferred address vector type, use it from there */
	if (fi->domain_attr->av_type != FI_AV_UNSPEC)
		av_attr.type = fi->domain_attr->av_type;

	/* Open Address Vector */
	ret = fi_av_open(EP->domain, &av_attr, &EP->av, NULL);
	if (ret) {
		FT_PRINTERR("fi_av_open", ret);
		return ret;
	}

	/* Open Endpoint */
	ret = fi_endpoint(EP->domain, fi, &EP->ep, NULL);
	if (ret) {
		FT_PRINTERR("fi_endpoint", ret);
		return ret;
	}

	/* ==== Bind Endpoint to CQ ================== */

	/* Bind to event queues and completion queues */
	if (fi->ep_attr->type == FI_EP_MSG)
		FT_EP_BIND(EP->ep, R->eq, 0);
	FT_EP_BIND(EP->ep, EP->av, 0);
	FT_EP_BIND(EP->ep, EP->tx_cq, FI_TRANSMIT);
	FT_EP_BIND(EP->ep, EP->rx_cq, FI_RECV);

	/* Enable endpoint */
	ret = fi_enable(EP->ep);
	if (ret) {
		FT_PRINTERR("fi_enable", ret);
		return ret;
	}

	/* Success */
	return 0;
}

/**
 * Initialize memory regions of active endpoint
 */
int ofi_active_ep_init_mr( struct ofi_resources * R, struct ofi_active_endpoint * EP, size_t rx_size, size_t tx_size )
{
	int ret;

	/* ==== Allocate Memory Region =============== */

	/* Calculate tx,rx and buffer size */
	EP->rx_size = rx_size + EP->rx_prefix_size;
	EP->tx_size = tx_size + EP->tx_prefix_size;
	EP->buf_size = MAX(EP->tx_size, FT_MAX_CTRL_MSG) + MAX(EP->rx_size, FT_MAX_CTRL_MSG);

	/* Allocate buffer */
	EP->buf = nn_alloc (EP->buf_size, "hofi");
    alloc_assert (EP->buf);
	if (!EP->buf) {
		perror("malloc");
		return -FI_ENOMEM;
	}

	/* Setup rx/tx buf */
	EP->rx_buf = EP->buf;
	EP->tx_buf = (char *) EP->buf + MAX(EP->rx_size, FT_MAX_CTRL_MSG);

	/* Register buffer */
	ret = fi_mr_reg(EP->domain, EP->buf, EP->buf_size, FI_RECV | FI_SEND,
			0, 0, 0, &EP->mr, NULL);
	if (ret) {
		FT_PRINTERR("fi_mr_reg", ret);
		return ret;
	}

	/* Success */
	return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////
// OFI High-Level Function - Connectionless
//////////////////////////////////////////////////////////////////////////////////////////

/**
 * Initialize as a connectionless endpoint, bound on 
 * the specified node/service
 */
int ofi_init_connectionless( struct ofi_resources * R, struct ofi_active_endpoint * EP, uint64_t flags, 
							 unsigned int addr_format, const char * node, const char * service )
{
	int ret;

	/* Specify the address format we are using */
	R->hints->addr_format = addr_format;

	/* Enumerate fabrics that match the specified details  */
	ret = fi_getinfo(FT_FIVERSION, node, service, flags, R->hints, &R->fi);
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		return ret;
	}

	/* Open fabric */
	ret = ofi_open_fabric( R );
	if (ret)
		return ret;

	/* Open active endpoint */
	ret = ofi_open_active_ep( R, EP, R->fi, MAX_MSG_SIZE );
	if (ret)
		return ret;

	/* Success */
	return 0;
}

/**
 * Specify and configure remote address
 */
int ofi_add_remote( struct ofi_resources * R, struct ofi_active_endpoint * EP, const char * node, const char * service )
{
	int ret;
	void * dest_addr;
	size_t dest_addr_len;

	/* Resolve address */
	ret = ofi_resolve_address( R, node, service, &dest_addr, &dest_addr_len );
	if (ret)
		return ret;

	/* Insert address in the address vector */
	ret = ft_av_insert( EP->av, dest_addr, 1, &EP->remote_fi_addr, 0, NULL);
	if (ret)
		return ret;

	/* Success */
	return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////
// OFI High-Level Function - Connected
//////////////////////////////////////////////////////////////////////////////////////////

/**
 * Create a bound socket and listen for incoming connections
 */
int ofi_init_server( struct ofi_resources * R, struct ofi_passive_endpoint * PEP, unsigned int addr_format, const char * node, const char * service )
{
	int ret;

	/* Specify the address format we are using */
	R->hints->addr_format = addr_format;

	/* Enumerate fabrics that match the specified details  */
	ret = fi_getinfo(FT_FIVERSION, node, service, FI_SOURCE, R->hints, &R->fi);
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		return ret;
	}

	/* Open fabric */
	ret = ofi_open_fabric( R );
	if (ret)
		return ret;

	/* Open a passive endpoint */
	ret = fi_passive_ep(R->fabric, R->fi, &PEP->pep, NULL);
	if (ret) {
		FT_PRINTERR("fi_passive_ep", ret);
		return ret;
	}

	/* Bind on event queue */
	ret = fi_pep_bind(PEP->pep, &R->eq->fid, 0);
	if (ret) {
		FT_PRINTERR("fi_pep_bind", ret);
		return ret;
	}

	/* Listen for incoming connection */
	ret = fi_listen(PEP->pep);
	if (ret) {
		FT_PRINTERR("fi_listen", ret);
		return ret;
	}

	/* Success */
	return 0;
}

/**
 * Wait for incoming connections and accept
 */
int ofi_server_accept( struct ofi_resources * R, struct ofi_passive_endpoint * PEP, struct ofi_active_endpoint * EP )
{
	struct fi_eq_cm_entry entry;
	uint32_t event;
	struct fi_info *info = NULL;
	ssize_t rd;
	int ret;

	/* Wait for connection request from client */
	rd = fi_eq_sread(R->eq, &event, &entry, sizeof entry, -1, 0);
	if (rd != sizeof entry) {
		FT_PROCESS_EQ_ERR(rd, R->eq, "fi_eq_sread", "listen");
		return (int) rd;
	}

	/* Extract info from the event */
	info = entry.info;
	if (event != FI_CONNREQ) {
		FT_ERR("Unexpected CM event %d\n", event);
		ret = -FI_EOTHER;
		goto err;
	}

	/* Open active endpoint */
	ret = ofi_open_active_ep( R, EP, info, MAX_MSG_SIZE );
	if (ret)
		return ret;

	/* Accept the incoming connection. Also transitions endpoint to active state */
	ret = fi_accept(EP->ep, NULL, 0);
	if (ret) {
		FT_PRINTERR("fi_accept", ret);
		goto err;
	}

	/* Wait for the connection to be established */
	rd = fi_eq_sread(R->eq, &event, &entry, sizeof entry, -1, 0);
	if (rd != sizeof entry) {
		FT_PROCESS_EQ_ERR(rd, R->eq, "fi_eq_sread", "accept");
		ret = (int) rd;
		goto err;
	}

	if (event != FI_CONNECTED || entry.fid != &EP->ep->fid) {
		FT_ERR("Unexpected CM event %d fid %p (ep %p)\n", event, entry.fid, EP->ep);
		ret = -FI_EOTHER;
		goto err;
	}

	/* Success */
	fi_freeinfo(info);
	return 0;

err:

	/* Failure */
	fi_reject(PEP->pep, info->handle, NULL, 0);
	fi_freeinfo(info);
	return ret;
}

/**
 * Initialize as a connectionless endpoint, bound on 
 * the specified node/service
 */
int ofi_init_client( struct ofi_resources * R, struct ofi_active_endpoint * EP, unsigned int addr_format, const char * node, const char * service )
{
	struct fi_eq_cm_entry entry;
	uint32_t event;
	ssize_t rd;
	int ret;

	/* Specify the address format we are using */
	R->hints->addr_format = addr_format;

	/* Enumerate fabrics that match the specified details  */
	ret = fi_getinfo(FT_FIVERSION, node, service, 0, R->hints, &R->fi);
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		return ret;
	}

	/* Open fabric */
	ret = ofi_open_fabric( R );
	if (ret)
		return ret;

	/* Open active endpoint */
	ret = ofi_open_active_ep( R, EP, R->fi, MAX_MSG_SIZE );
	if (ret)
		return ret;

	/* Connect to server */
	ret = fi_connect(EP->ep, R->fi->dest_addr, NULL, 0);
	if (ret) {
		FT_PRINTERR("fi_connect", ret);
		return ret;
	}

	/* Wait for the connection to be established */
	rd = fi_eq_sread(R->eq, &event, &entry, sizeof entry, -1, 0);
	if (rd != sizeof entry) {
		FT_PROCESS_EQ_ERR(rd, R->eq, "fi_eq_sread", "connect");
		return (int) rd;
	}

	if (event != FI_CONNECTED || entry.fid != &EP->ep->fid) {
		FT_ERR("Unexpected CM event %d fid %p (ep %p)\n", event, entry.fid, EP->ep);
		return -FI_EOTHER;
	}

	/* Success */
	return 0;
}


