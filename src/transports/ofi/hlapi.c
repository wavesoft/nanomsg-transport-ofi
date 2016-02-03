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
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>

#ifndef __STANDALONE_TESTS
#include "../../utils/alloc.h"
#include "../../utils/err.h"
#else
#include <nn_standalone_func.h>
#endif

#include "hlapi.h"
#include "ofi.h"

/* Platform helpers */
#ifdef __APPLE__
#include "platf/osx.c"
#endif

/* Helper macro to enable or disable verbose logs on console */
#ifdef OFI_DEBUG_LOG
    /* Enable debug */
    #define _ofi_debug(...)   printf(__VA_ARGS__)
#else
    /* Disable debug */
    #define _ofi_debug(...)
#endif

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

#define FT_CLOSE_FID(fd)			\
	do {					\
		if ((fd)) {			\
			int ret = fi_close(&(fd)->fid);	\
			if (ret) { \
				if (ret == -FI_EBUSY) { \
					printf("OFI: *** Error closing FD " #fd " (FI_EBUSY)\n"); \
				} else { \
					printf("OFI: *** Error closing FD " #fd " caused error = %i\n", ret); \
				} \
			} \
			fd = NULL;		\
		}				\
	} while (0)

#define MAX(a,b) ((a>b) ? a : b)


//////////////////////////////////////////////////////////////////////////////////////////
// OFI Helper Functions
//////////////////////////////////////////////////////////////////////////////////////////

/**
 * Return elapsed time in microseconds
 */
int64_t ofi_get_elapsed(const struct timespec *b, const struct timespec *a,
		    enum ofi_time_precision p)
{
    int64_t elapsed;

    elapsed = (a->tv_sec - b->tv_sec) * 1000 * 1000 * 1000;
    elapsed += a->tv_nsec - b->tv_nsec;
    return elapsed / p;
}

/**
 * Calculate how many kilo-cycles we can run per millisedon
 */
uint32_t ofi_calc_kinstr_perms()
{
	struct timespec a, b;
	uint64_t kinst_ms;

	/* Run one million actions and count how much time it takes */
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (int i=0; i<1000000; i++) {
		/* Do some moderate heap alloc/math operations */
		volatile int v = 0;
		v = v + 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &b);

	/* Count kinst_ms spent */
	kinst_ms = 1000000000 / (b.tv_nsec - a.tv_nsec);

	/* Count how many mega-cycles we can run in 1 millisecond */

	/* Wrap to maximum 32-bit */
	if (kinst_ms > 4294967295) {
		return 4294967295;
	}

	/* Return at least one */
	if (kinst_ms == 0) {
		return 1;
	}

	/* Return */
	return (uint32_t) kinst_ms;
}

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

	/* CQ entry based on configured format (i.e. FI_CQ_FORMAT_DATA) */
	while (1) {
		ret = fi_cq_read(cq, &entry, 1);

		/* Operation failed */
		if (ret > 0) {
			/* Success */
			return 0;

		} else if (ret < 0 && ret != -FI_EAGAIN) {
			if (ret == -FI_EAVAIL) {
				struct fi_cq_err_entry err_entry;

				/* Handle error */
				ret = fi_cq_readerr(cq, &err_entry, 0);

				if (err_entry.err == FI_ECANCELED) {
					return -err_entry.err;
				}				/* Check if the operation was cancelled (ex. terminating connection) */


				/* Display other erors */
				printf("OFI: %s (%s)\n",
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
 * Wait for completion queue, also listening for shutdown events on the event queue
 */
int ft_wait_shutdown_aware(struct fid_cq *cq, struct fid_eq *eq, int timeout, struct fi_cq_data_entry * ret_entry)
{
	struct fi_eq_cm_entry eq_entry;
	struct fi_cq_data_entry entry;
	struct fi_cq_data_entry * entry_ptr = &entry;
	struct timespec a, b;
	uint32_t event;
	uint8_t shutdown_interval;
	uint8_t fast_poller;
	int ret;

	/* Override entry pointer if we have given a fi_cq_data_entry to fill */
	if (ret_entry != NULL)
		entry_ptr = ret_entry;

	/* Get the starting time to timeout after */
	if (timeout >= 0)
		clock_gettime(CLOCK_MONOTONIC, &a);

	/* TODO: The timeout solution looks like a HACK! Find a better solution */
	shutdown_interval = 0;

	/* CQ entry based on configured format (i.e. FI_CQ_FORMAT_DATA) */
	while (1) {

		/* Burst-check for CQ event (reduced lattency on high speeds) */
		// ret = fi_cq_sread(cq, &entry, 1, NULL, 500);
		// fast_poller=255; 
		// do {
		ret = fi_cq_read(cq, entry_ptr, 1);
		// } while (nn_fast( (ret == -FI_EAGAIN) && (--fast_poller > 0) ));

		/* Operation failed */
		if (nn_slow(ret > 0)) {
			/* Success */
			_ofi_debug("OFI: ft_wait() succeed with shutdown_interval=%i\n", shutdown_interval);
			return 0;
		} else if (nn_fast(ret < 0 && ret != -FI_EAGAIN)) {
			if (nn_fast(ret == -FI_EAVAIL)) {
				struct fi_cq_err_entry err_entry;

				/* Handle error */
				ret = fi_cq_readerr(cq, &err_entry, 0);

				/* Check if the operation was cancelled (ex. terminating connection) */
				if (err_entry.err == FI_ECANCELED) {
					_ofi_debug("OFI: ft_wait() exiting because of FI_ECANCELED\n");
					return -FI_REMOTE_DISCONNECT;
				}

				/* Display other erors */
				printf("OFI: %s (%s)\n",
					fi_strerror(err_entry.err),
					fi_cq_strerror(cq, err_entry.prov_errno, err_entry.err_data, NULL, 0)
				);
				return ret;
			} else {
				FT_PRINTERR("fi_cq_read", ret);
			}
		} else {

			/* Check for timeout */
			if (nn_slow(timeout >= 0)) {
				clock_gettime(CLOCK_MONOTONIC, &b);
				if ((b.tv_sec - a.tv_sec) > timeout) {
					_ofi_debug("OFI: ft_wait() timeout expired\n");
					return -FI_ENODATA; /* TODO: Perhaps not treat this as a remote disconnect? */
				}
			}

			/* Give some chance to intercept messages even if we received a shutdown event */
			if (nn_slow(shutdown_interval > 0)) {
				if (--shutdown_interval == 0) {
					/* We are remotely disconnected */
					_ofi_debug("OFI: ft_wait() exiting because of FI_SHUTDOWN event\n");
					return -FI_REMOTE_DISCONNECT;
				}
			}
		}

		/* Then check for shutdown event */
		if (nn_fast(shutdown_interval == 0)) {

			/* Event Queue is only initialized on EP_MSG */
			if (eq != NULL) {
				ret = fi_eq_read(eq, &event, &eq_entry, sizeof eq_entry, 0);
				if (nn_fast(ret != -FI_EAGAIN)) {
					if (event == FI_SHUTDOWN) {
						/* If no CQ is arrived within 255 cycles, consider it lost */
						shutdown_interval = 255;
					} else {
						FT_ERR("Unexpected CM event %d\n", event);
					}
				}
			}
			
		}

		// /* Let some other CPU work to be done */
		if (++fast_poller > 100) {
			usleep(10);
			fast_poller = 0;
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
// I/O Interfaces
//////////////////////////////////////////////////////////////////////////////////////////

/**
 * Send a scatter-gather array message over OFI
 */
ssize_t ofi_tx_msg( struct ofi_active_endpoint * EP, const struct iovec *msg_iov, void ** msg_iov_desc, 
	size_t iov_count, uint64_t flags, int timeout )
{
	ssize_t ret;

	// /* Count the size of the message */
	// ret = 0;
	// for (int i=0; i<iov_count; i++)
	// 	ret += msg_iov[i].iov_len;

	/* Prepare fi_msg */
	struct fi_msg msg = {
		.msg_iov = msg_iov,
		.iov_count = iov_count,
		.desc = msg_iov_desc,
		.addr = EP->remote_fi_addr,
		.context = &EP->tx_ctx,
		.data = 0
	};

	/* Send data */
	// ret = fi_sendmsg(EP->ep, msg_iov, msg_iov_desc, iov_count, EP->remote_fi_addr, &EP->tx_ctx );
	ret = fi_sendmsg(EP->ep, &msg, 0);
	if (ret) {

		/* If we are in a bad state, we were remotely disconnected */
		if (ret == -FI_EOPBADSTATE) {
			_ofi_debug("OFI: HLAPI: ofi_tx_msg() returned -FI_EOPBADSTATE, considering shutdown.\n");
			return -FI_REMOTE_DISCONNECT;			
		}

		/* Otherwise display error */
		FT_PRINTERR("ofi_tx_msg", ret);
		return ret;
	}

	/* Wait for Tx CQ event (when 'the buffer can be reused' - INJECT_COMPLETE) */
	ret = ft_wait_shutdown_aware(EP->tx_cq, EP->eq, timeout, NULL);
	if (ret) {

		/* Be silent on known errors */
		if ((ret == -FI_REMOTE_DISCONNECT) || (ret == -FI_ENODATA))
			return ret;

		/* Otherwise display error */
		FT_PRINTERR("ft_wait<tx_cq>", ret);
		return ret;
	}

	/* Success */
	return 0;
}

/**
 * Send a single-pointer data
 */
ssize_t ofi_tx_data( struct ofi_active_endpoint * EP, void * buf, const size_t tx_size, 
		void *desc, int timeout )
{
	ssize_t ret;

	/* Send data */
	// ret = fi_sendmsg(EP->ep, msg_iov, msg_iov_desc, iov_count, EP->remote_fi_addr, &EP->tx_ctx );
	ret = fi_send(EP->ep, buf, tx_size, desc, EP->remote_fi_addr, &EP->tx_ctx);
	if (ret) {

		/* If we are in a bad state, we were remotely disconnected */
		if (ret == -FI_EOPBADSTATE) {
			_ofi_debug("OFI: HLAPI: ofi_tx_msg() returned -FI_EOPBADSTATE, considering shutdown.\n");
			return -FI_REMOTE_DISCONNECT;			
		}

		/* Otherwise display error */
		FT_PRINTERR("ofi_tx_msg", ret);
		return ret;
	}

	/* Wait for Tx CQ event (when 'the buffer can be reused' - INJECT_COMPLETE) */
	ret = ft_wait_shutdown_aware(EP->tx_cq, EP->eq, timeout, NULL);
	if (ret) {

		/* Be silent on known errors */
		if ((ret == -FI_REMOTE_DISCONNECT) || (ret == -FI_ENODATA))
			return ret;

		/* Otherwise display error */
		FT_PRINTERR("ft_wait<tx_cq>", ret);
		return ret;
	}

	/* Success */
	return 0;

}

/**
 * Receive data on a single buffer
 */
ssize_t ofi_rx_data( struct ofi_active_endpoint * EP, void * buf, const size_t max_size, 
		void *desc, size_t * rx_size, int timeout )
{
	int ret;
	struct fi_cq_data_entry cq_entry;

	/* Receive data */
	ret = fi_recv(EP->ep, buf, max_size, desc, EP->remote_fi_addr, &EP->rx_ctx);
	if (ret) {

		/* If we are in a bad state, we were remotely disconnected */
		if (ret == -FI_EOPBADSTATE) {
			_ofi_debug("OFI: HLAPI: ofi_rx() returned %i, considering shutdown.\n", ret);
			return -FI_REMOTE_DISCONNECT;
		}

		/* Otherwise display error */
		FT_PRINTERR("ofi_rx_data", ret);
		return ret;
	}

	/* Wait for Rx CQ */
	ret = ft_wait_shutdown_aware(EP->rx_cq, EP->eq, timeout, &cq_entry);
	if (ret) {

		/* Be silent on known errors */
		if ((ret == -FI_REMOTE_DISCONNECT) || (ret == -FI_ENODATA))
			return ret;

		/* Otherwise display error */
		FT_PRINTERR("ft_wait<rx_cq>", ret);
		return ret;
	}

	/* Update size pointer if specified */
	if (rx_size != NULL)
		*rx_size = cq_entry.len;

	/* Return 0 */
	return 0;
}

/**
 * Receive a scatter-gather array message over OFI
 */
ssize_t ofi_rx_msg( struct ofi_active_endpoint * EP, const struct iovec *msg_iov, void ** msg_iov_desc, 
		size_t iov_count, size_t * rx_size, uint64_t flags, int timeout )
{
	int ret;
	struct fi_cq_data_entry cq_entry;

	/* Prepare fi_msg */
	struct fi_msg msg = {
		.msg_iov = msg_iov,
		.iov_count = iov_count,
		.desc = msg_iov_desc,
		.addr = EP->remote_fi_addr,
		.context = &EP->rx_ctx,
		.data = 0
	};

	/* Receive data */
	// ret = fi_recvv(EP->ep, msg_iov, msg_iov_desc, iov_count, EP->remote_fi_addr, &EP->rx_ctx);
	ret = fi_recvmsg(EP->ep, &msg, 0);
	if (ret) {

		/* If we are in a bad state, we were remotely disconnected */
		if (ret == -FI_EOPBADSTATE) {
			_ofi_debug("OFI: HLAPI: ofi_rx() returned %i, considering shutdown.\n", ret);
			return -FI_REMOTE_DISCONNECT;
		}

		/* Otherwise display error */
		FT_PRINTERR("ofi_rx_msg", ret);
		return ret;
	}

	/* Wait for Rx CQ */
	ret = ft_wait_shutdown_aware(EP->rx_cq, EP->eq, timeout, &cq_entry);
	if (ret) {

		/* Be silent on known errors */
		if ((ret == -FI_REMOTE_DISCONNECT) || (ret == -FI_ENODATA))
			return ret;

		/* Otherwise display error */
		FT_PRINTERR("ft_wait<rx_cq>", ret);
		return ret;
	}

	/* Update size pointer if specified */
	if (rx_size != NULL)
		*rx_size = cq_entry.len;

	/* Success */
	return 0;
}

/**
 * Post receive buffers
 */
int ofi_rx_post( struct ofi_active_endpoint * EP, void * buf, const size_t max_size, void *desc )
{
	int ret;

	/* Receive data */
	ret = fi_recv(EP->ep, buf, max_size, desc, EP->remote_fi_addr, &EP->rx_ctx);
	if (ret) {

		/* If we are in a bad state, we were remotely disconnected */
		if (ret == -FI_EOPBADSTATE) {
			_ofi_debug("OFI: HLAPI: ofi_rx() returned %i, considering shutdown.\n", ret);
			return -FI_REMOTE_DISCONNECT;
		}

		/* Otherwise display error */
		FT_PRINTERR("ofi_rx_data", ret);
		return ret;
	}

	/* We succeeded */
	return 0;
}

/**
 * Check for incoming cq event
 */
int ofi_rx_poll( struct ofi_active_endpoint * EP, size_t * rx_size, uint32_t timeout )
{

	/* Local properties */
	int ret;
	uint32_t ms_left = timeout;
	uint16_t k_tousand, k_ms;
	struct fi_cq_data_entry cq_entry;

	/* Loop for waiting the specified number of milliseconds */
	while (ms_left--) {

		/* Loop for waiting a millisecond */
		k_ms = EP->kinstructions_per_ms; while (k_ms--) { 
		k_tousand = 1000; while (k_tousand--) {

			/* Wait for 1 CQ event */
			ret = fi_cq_read( EP->rx_cq, &cq_entry, 1 );

			/* Break if state changes */
			if (nn_fast(ret == -FI_EAGAIN)) {
				continue;
			} else if (nn_slow(ret == -FI_EAVAIL)) {
				struct fi_cq_err_entry err_entry;

				/* Handle disconnection error */
				ret = fi_cq_readerr(EP->rx_cq, &err_entry, 0);
				if (err_entry.err == FI_ECANCELED) {
					return -FI_REMOTE_DISCONNECT;
				}

				/* Display other erors */
				printf("OFI: ofi_rx_poll: %s (%s)\n",
					fi_strerror(err_entry.err),
					fi_cq_strerror(EP->rx_cq, err_entry.prov_errno, err_entry.err_data, NULL, 0)
				);

				/* Return error */
				return -err_entry.err;

			/* Return errors */
			} else if (nn_slow(ret < 0)) {
				return ret;

			/* Anything else is a success */
			} else {

				/* If we have an rx_size pointer, update it */
				if (nn_fast(rx_size != NULL))
					*rx_size = cq_entry.len;

				return 0;

			}

		} } /* End of millisecond loop (x2) */

	} /* End of timeout loop */

	/* We timed out */
	return -FI_ENODATA;

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

	/* Debug */
	printf("OFI: Using fabric=%s, provider=%s\n", 
		R->fi->fabric_attr->name, 
		R->fi->fabric_attr->prov_name );

	/* Cache roughly how many instructions per ms does this CPU cost */
	R->kinstructions_per_ms  = ofi_calc_kinstr_perms();

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

	/* Open Endpoint */
	ret = fi_endpoint(EP->domain, fi, &EP->ep, NULL);
	if (ret) {
		FT_PRINTERR("fi_endpoint", ret);
		return ret;
	}

	/* ==== Open Completion Queues =============== */

	/* Create a Tx completion queue */
	struct fi_cq_attr cq_attr = {
		.wait_obj = FI_WAIT_NONE,
		.format = FI_CQ_FORMAT_DATA,
		.size = fi->tx_attr->size
	};
	ret = fi_cq_open(EP->domain, &cq_attr, &EP->tx_cq, &EP->tx_ctx);
	if (ret) {
		FT_PRINTERR("fi_cq_open<tx_cq>", ret);
		return ret;
	}

	/* Create a Rx completion queue */
	ret = fi_cq_open(EP->domain, &cq_attr, &EP->rx_cq, &EP->rx_ctx);
	if (ret) {
		FT_PRINTERR("fi_cq_open<rx_cq>", ret);
		return ret;
	}

	/* Bind to event queues and completion queues */
	FT_EP_BIND(EP->ep, EP->tx_cq, FI_TRANSMIT);
	FT_EP_BIND(EP->ep, EP->rx_cq, FI_RECV);

	/* ==== Open Address Vector ================== */

	/* Open Address Vector */
	if (fi->ep_attr->type == FI_EP_RDM || fi->ep_attr->type == FI_EP_DGRAM) {

		/* Prepare structure */
		struct fi_av_attr av_attr = {
			.type = FI_AV_MAP,
			.count = 1
		};

		/* If domain has a preferred address vector type, use it from there */
		if (fi->domain_attr->av_type != FI_AV_UNSPEC)
			av_attr.type = fi->domain_attr->av_type;

		/* Open address vector */
		ret = fi_av_open(EP->domain, &av_attr, &EP->av, NULL);
		if (ret) {
			FT_PRINTERR("fi_av_open", ret);
			return ret;
		}

		/* Bind endoint to AV */
		FT_EP_BIND(EP->ep, EP->av, 0);

	} else {
		
		/* Set AV to null */
		EP->av = NULL;

	}

	/* ==== Prepare Event Queue ================== */

	/* Open event queue for receiving socket events */
	if (fi->ep_attr->type == FI_EP_MSG) {

		/* Prepare structure */
		struct fi_eq_attr eq_attr = {
			.wait_obj = FI_WAIT_UNSPEC,
			.flags = FI_WRITE
		};

		/* Open event queue */
		ret = fi_eq_open(R->fabric, &eq_attr, &EP->eq, NULL);
		if (ret) {
			FT_PRINTERR("fi_eq_open", ret);
			return ret;
		}

		/* Bind on event queue */
		FT_EP_BIND(EP->ep, EP->eq, 0);
	}

	/* ==== Bind Endpoint to CQ ================== */

	/* Enable endpoint */
	ret = fi_enable(EP->ep);
	if (ret) {
		FT_PRINTERR("fi_enable", ret);
		return ret;
	}

	/* Cache kilo-instructions per second */
	EP->kinstructions_per_ms = R->kinstructions_per_ms;

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

/**
 * Open a passive endpoint
 */
int ofi_open_passive_ep( struct ofi_resources * R, struct ofi_passive_endpoint * PEP )
{
	int ret;

	/* Open a passive endpoint */
	ret = fi_passive_ep(R->fabric, R->fi, &PEP->pep, NULL);
	if (ret) {
		FT_PRINTERR("fi_passive_ep", ret);
		return ret;
	}

	/* Open an event queue */
	struct fi_eq_attr eq_attr = {
		.wait_obj = FI_WAIT_UNSPEC,
		.flags = FI_WRITE
	};
	ret = fi_eq_open(R->fabric, &eq_attr, &PEP->eq, NULL);
	if (ret) {
		FT_PRINTERR("fi_eq_open", ret);
		return ret;
	}

	/* Bind on event queue */
	ret = fi_pep_bind(PEP->pep, &PEP->eq->fid, 0);
	if (ret) {
		FT_PRINTERR("fi_pep_bind", ret);
		return ret;
	}

	/* Success */
	return 0;
}

/**
 * Open a passive endpoint
 */
int ofi_restart_passive_ep( struct ofi_resources * R, struct ofi_passive_endpoint * PEP )
{
	int ret;

	/* Drain event queue */
	struct fi_eq_cm_entry entry;
	uint32_t event;
	ssize_t rd;
	do {
		rd = fi_eq_read(PEP->eq, &event, &entry, sizeof entry, 0);
	} while ((int)rd == 0);

	/* Re-open passive endpoint */
	_ofi_debug("OFI: Restarting passive endpoint\n");
	FT_CLOSE_FID( PEP->pep );
	FT_CLOSE_FID( PEP->eq );
	ret = ofi_open_passive_ep( R, PEP );
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

	/* Open passive endpoint */
	ret = ofi_open_passive_ep( R, PEP );
	if (ret)
		return ret;

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

	/* Listen for incoming connection */
	ret = fi_listen(PEP->pep);
	if (ret) {
		FT_PRINTERR("fi_listen", ret);
		return ret;
	}

	/* Wait for connection request from client */
	rd = fi_eq_sread(PEP->eq, &event, &entry, sizeof entry, -1, 0);
	if (rd != sizeof entry) {
		FT_PROCESS_EQ_ERR(rd, PEP->eq, "fi_eq_sread", "listen");
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
	rd = fi_eq_sread(EP->eq, &event, &entry, sizeof entry, -1, 0);
	if (rd != sizeof entry) {
		FT_PROCESS_EQ_ERR(rd, EP->eq, "fi_eq_sread", "accept");
		ret = (int) rd;
		goto err;
	}

	/* Check for aborted operations */
	if (event == FI_SHUTDOWN) {
		fi_freeinfo(info);
		return FI_SHUTDOWN;

	} else if (event != FI_CONNECTED || entry.fid != &EP->ep->fid) {
		FT_ERR("Unexpected CM event %d fid %p (ep %p)\n", event, entry.fid, EP->ep);
		ret = -FI_EOTHER;
		goto err;
	}

	/* Re-open passive endpoint */
	ret = ofi_restart_passive_ep( R, PEP );
	if (ret)
		return ret;

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
	rd = fi_eq_sread(EP->eq, &event, &entry, sizeof entry, -1, 0);
	if (rd != sizeof entry) {
		FT_PROCESS_EQ_ERR(rd, EP->eq, "fi_eq_sread", "connect");
		return (int) rd;
	}

	if (event != FI_CONNECTED || entry.fid != &EP->ep->fid) {
		FT_ERR("Unexpected CM event %d fid %p (ep %p)\n", event, entry.fid, EP->ep);
		return -FI_EOTHER;
	}

	/* Success */
	return 0;
}

/**
 * Allocate a new memory region object
 */
int ofi_mr_alloc( struct ofi_active_endpoint * ep, struct ofi_mr ** mmr )
{
	int ret;

	/* Allocate desccriptor structure */
	*mmr = nn_alloc( sizeof(struct ofi_mr), "ofi_mr" );
	if (!*mmr) {
		FT_ERR("OFI: Memory region tag allocation failed\n");
		return EAI_MEMORY;
	}

	/* Init properties */
	(*mmr)->ptr = NULL;
	(*mmr)->mr = NULL;

	/* Success */
	return 0;
}

/**
 * Tag a particular memory region as shared
 */
int ofi_mr_manage( struct ofi_active_endpoint * EP, struct ofi_mr * mr, void * buf, size_t len, int requested_key, enum ofi_mr_flags flags )
{
	int ret;
	uint64_t access_flags = 0;
	assert( EP != NULL );
	assert( buf != NULL );
	assert( mr != NULL );

	/* Unmanage previous instances */
	if (mr->ptr) {
		/* Unmanage only if pointer differs */
		if (mr->ptr != buf)
			ofi_mr_unmanage( EP, mr );
	}

	/* Apply read/write flags */
	if (flags == MR_SEND) {
		access_flags |= FI_SEND | FI_WRITE | FI_REMOTE_READ;
	}
	if (flags == MR_RECV) {
		access_flags |= FI_RECV | FI_RECV | FI_REMOTE_WRITE;
	}

	/* Register buffer */
	_ofi_debug("OFI: Managing memory region (key=%i)\n", requested_key);
	ret = fi_mr_reg(EP->domain, buf, len, access_flags, 0, requested_key, 0, &mr->mr, NULL);
	if (ret) {
		FT_PRINTERR("fi_mr_reg", ret);
		return ret;
	}

	/* Keep pointer reference */
	mr->ptr = buf;

	/* Success */
	return 0;
}

/**
 * Untag a particular memory region as shared
 */
int ofi_mr_unmanage( struct ofi_active_endpoint * EP, struct ofi_mr * mr )
{

	/* Don't do anything if MR is null */
	if (!mr->mr) return 0;
	_ofi_debug("OFI: Unmanaging memory region\n");

	/* Close memory region and free descriptor */
	FT_CLOSE_FID( mr->mr );

	/* Reset properties */
	mr->mr = NULL;
	mr->ptr = NULL;

	/* Success */
	return 0;
}


/**
 * Unmanage and free memory regions
 */
int ofi_mr_free( struct ofi_active_endpoint * ep, struct ofi_mr ** mmr )
{
	int ret;

	/* Unmanage previous reservations */
	if ((*mmr)->mr) {
		ofi_mr_unmanage( ep, *mmr );
	}

	/* Free structure */
	nn_free( *mmr );

	/* Success */
	return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////
// OFI Cleanup Functions
//////////////////////////////////////////////////////////////////////////////////////////


/**
 * Shutdown an active endpoint
 */
int ofi_shutdown_ep( struct ofi_active_endpoint * EP )
{
	int ret;

	/* Send a shutdown even to event queuet */
	struct fi_eq_cm_entry entry = {0};
	ssize_t rd;
	rd = fi_eq_write( EP->eq, FI_SHUTDOWN, &entry, sizeof entry, 0 );
	if (rd != sizeof entry) {
		_ofi_debug("OFI: ERROR: Unable to signal the shutdown event to EP!");
	}

	/* Not implemented in some providers */
	fi_shutdown(EP->ep, 0);

	/* Cancel I/O operations */
	fi_cancel( &(EP->ep)->fid, &EP->tx_ctx );
	fi_cancel( &(EP->ep)->fid, &EP->rx_ctx );

	// /* Shutdown endpoint */
	// ret = fi_shutdown(EP->ep, 0);
	// if (ret) {
	// 	FT_PRINTERR("fi_shutdown", ret);
	// 	return ret;
	// }

	/* Success */
	return 0;
}

/**
 * Shutdown a passive endpoint
 */
int ofi_shutdown_pep( struct ofi_passive_endpoint * PEP )
{

	/* Send a shutdown even to event queuet */
	struct fi_eq_cm_entry entry = {0};
	ssize_t rd;
	rd = fi_eq_write( PEP->eq, FI_SHUTDOWN, &entry, sizeof entry, 0 );
	if (rd != sizeof entry) {
		_ofi_debug("OFI: ERROR: Unable to signal the shutdown event to PEP!");
	}

	/* No particular procedure, just wait for ofi_free_pep */
	return 0;
}

/**
 * Free hints and core structures
 */
int ofi_free( struct ofi_resources * R )
{

	/* Close FDs */
	FT_CLOSE_FID( R->fabric );

	/* Free resources */
	fi_freeinfo( R->hints );
	fi_freeinfo( R->fi );

	/* Success */
	return 0;
}

/**
 * Free passive endpoint
 */
int ofi_free_pep( struct ofi_passive_endpoint * ep )
{

	/* Drain event queue */
	struct fi_eq_cm_entry entry;
	uint32_t event;
	ssize_t rd;
	do {
		rd = fi_eq_read(ep->eq, &event, &entry, sizeof entry, 0);
	} while ((int)rd == 0);

	/* Close endpoint */
	FT_CLOSE_FID( ep->pep );

	/* Free structures */
	FT_CLOSE_FID( ep->eq );

	/* Success */
	return 0;
}

/**
 * Free active endpoint
 */
int ofi_free_ep( struct ofi_active_endpoint * ep )
{

	/* Drain event queue */
	struct fi_eq_cm_entry entry;
	uint32_t event;
	ssize_t rd;
	do {
		rd = fi_eq_read(ep->eq, &event, &entry, sizeof entry, 0);
	} while ((int)rd == 0);

	/* Close endpoint */
	FT_CLOSE_FID( ep->ep );
	
	/* Free structures */
	FT_CLOSE_FID( ep->tx_cq );
	FT_CLOSE_FID( ep->rx_cq );
	FT_CLOSE_FID( ep->eq );

	/* Close Address Vector */
	FT_CLOSE_FID( ep->av );

	/* Close domain */
	FT_CLOSE_FID( ep->domain );

	/* Success */
	return 0;
}
