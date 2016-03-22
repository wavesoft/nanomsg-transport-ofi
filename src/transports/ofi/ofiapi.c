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

#include "ofiapi.h"

#include "../../utils/alloc.h"
#include "../../utils/err.h"
#include "../../utils/cont.h"

/* ########################################################################## */
/*  Utility Functions                                                         */
/* ########################################################################## */

/**
 * Parse string address that follows the following specifications:
 *
 *  - [ip]:[service]
 *  - [ip]:[service]@[fabric]
 *  - [ip]:[service]@[fabric]:[provider]
 *  - *@[fabric]
 *
 */
static int ofi_match_fabric( const char * addr, enum ofi_fabric_addr_flags addf, 
    struct fi_info * hints, struct fi_info ** ans )
{
    int ret;
    uint64_t flags = 0;
    size_t len = strlen(addr);
    char * node = nn_alloc( len+1, "address" );
    char *service, *fabric, *provider;
    struct fi_info *fabrics, *f, *f_pick;

    /* Copy const addr to local buffer */
    memcpy( node, addr, len+1 );
    node[len] = '\0';

    /* Find possible fabric specs */
    fabric = strrchr(node, '@');
    if (fabric == NULL) {
        provider = NULL;
    } else {

        /* Terminate address scanning at fabric */
        *(char*)(fabric) = '\0'; fabric++;

        /* Locate possible provider */
        provider = strrchr(fabric, ':');
        if (provider != NULL) {
            *(char*)(provider) = '\0'; provider++;
        }

    }

    /* Get node and service */
    service = strrchr (node, ':');
    if (service == NULL) {
        nn_free( node );
        return -EINVAL;
    }
    *(char*)(service) = '\0'; service++;

    /* Check if this is supposed to be a local address, and if true, add
       the FI_SOURCE flag for the resolution. */
    if (addf == OFI_ADDR_LOCAL)
        flags = FI_SOURCE;

    /* Try to find a matching fabric according to specs & hints */
    ret = fi_getinfo(FT_FIVERSION, node, service, flags, hints, &fabrics);
    if (ret) {
        FT_PRINTERR("fi_getinfo", ret);
        nn_free(node);
        return ret;
    }

    /* Iterate over fabrics and test fabric & provider arags */
    f_pick = NULL;
    while (f != NULL) {

        /* Assume that's our candidate */
        f_pick = f;

        /* Test if fabric or provider does not match */
        if (fabric != NULL) {
            if (strcmp(fabric, f->fabric_attr->name) != 0) {
                f_pick = NULL;
            }
        }
        if (provider != NULL) {
            if (strcmp(provider, f->fabric_attr->prov_name) != 0) {
                f_pick = NULL;
            }
        }

        /* Try next */
        f = fabrics->next;

    };

    /* Check if nothing found */
    if (f_pick == NULL) {
        fi_freeinfo( fabrics );
        nn_free( node );
        return -ESOCKTNOSUPPORT;
    }

    /* Duplicate picked FI structure, free resources and return */
    *ans = fi_dupinfo( f_pick );
    fi_freeinfo( fabrics );
    return 0;
}

/* ########################################################################## */
/*  Global Functions                                                          */
/* ########################################################################## */

/**
 * Allocate hints and prepare core structures
 */
int ofi_init( struct ofi_resources * R, enum fi_ep_type ep_type )
{

    /* Allocate hints */
    R->hints = fi_allocinfo();
    if (!R->hints) {
        printf("OFI[H]: Unable to allocate hints structure!\n");
        R->err = 255;
        return 255;
    }

    /* Domains */
    R->hints->domain_attr->mr_mode       = FI_MR_UNSPEC;
    R->hints->domain_attr->threading     = FI_THREAD_UNSPEC;

    /* Endpoints */
    R->hints->ep_attr->type = ep_type;

    /* Fabric */
    R->hints->caps          = FI_MSG;
    R->hints->mode          = FI_CONTEXT | FI_LOCAL_MR;

    /* Specify the address format we are using */
    R->hints->addr_format   = FI_FORMAT_UNSPEC;

    /* Initialize fabric list */
    nn_list_init( &R->fabrics );

    /* Success */
    R->err = 0;
    return 0;
}

/* ########################################################################## */
/*  Fabric Functions                                                          */
/* ########################################################################## */

/**
 * Open or re-use a previously allocated domain
 */
int ofi_fabric_open( struct ofi_resources * R, const char * address,
    enum ofi_fabric_addr_flags flags, struct ofi_fabric ** F )
{
    int ret;
    struct fi_info *info;
    struct ofi_fabric *item;
    struct nn_list_item *it;
    uint64_t f_flags = 0;

    /* Allocate a fabric structure */
    item = nn_alloc( sizeof(struct ofi_fabric), "ofi fabric" );
    nn_assert( item );

    /* Find the fabric that most accurately describes the address */
    ret = ofi_match_fabric( address, flags, R->hints, &item->fi );
    if (ret) {
        FT_PRINTERR("fi_getinfo", ret);
        nn_free(item);
        *F = NULL;
        return ret;
    }

    /* TODO: Reuse previously allcoated fabric that matches
             the specified info. */

    /* Open fabric */
    ret = fi_fabric(item->fi->fabric_attr, &item->fabric, NULL);
    if (ret) {
        FT_PRINTERR("fi_fabric", ret);
        fi_freeinfo(item->fi);
        nn_free(item);
        *F = NULL;
        return ret;
    }

    /* Debug */
    printf("OFI[H]: Using fabric=%s, provider=%s\n", 
        item->fi->fabric_attr->name, 
        item->fi->fabric_attr->prov_name );

    /* Initialize properties */
    item->parent = R;

    /* Initialize structures */
    nn_list_init( &item->domains );
    nn_atomic_init( &item->ref, 1 );

    /* Initialize worker pool on this fabric */
    nn_ofiw_pool_init( &item->pool, item->fabric );

    /* Keep this fabric on list */
    nn_list_item_init( &item->item );
    nn_list_insert (&R->fabrics, &item->item,
        nn_list_end (&R->fabrics));

    /* We are good */
    *F = item;
    return 0;

}

/**
 * Get a worker from this fabric
 */
struct nn_ofiw * ofi_fabric_getworker( struct ofi_fabric * F,
    struct nn_fsm * owner )
{
    /* Return a worker from OFI worker pool */
    return nn_ofiw_pool_getworker( &F->pool, owner );
}

/**
 * Close a fabric previously 
 */
int ofi_fabric_close( struct ofi_fabric * F )
{
    struct ofi_domain *item;
    struct nn_list_item *it;

    /* Decrement reference counter & only continue if reached zero */
    if (nn_atomic_dec( &F->ref, 1 ) > 1)
        return 0;

    /* Close domains */
    for (it = nn_list_begin (&F->domains);
          it != nn_list_end (&F->domains);
          it = nn_list_next (&F->domains, it)) {
        item = nn_cont (it, struct ofi_domain, item);

        /* Close domains */
        ofi_domain_close( item );

    }

    /* Remove fabric from list */
    nn_list_erase (&F->parent->fabrics, &F->item);

    /* Free structures */
    nn_ofiw_pool_term(&F->pool);
    nn_list_term( &F->domains );
    nn_list_item_term(&F->item);
    nn_free(F);

    return 0;

}

/* ########################################################################## */
/*  Domain Functions                                                          */
/* ########################################################################## */

/**
 * Open or re-use the correct OFI domain for the fi_info specified. 
 * If `fi` is set to NULL, the fi_info used to resolve the fabric will be used.
 */
int ofi_domain_open( struct ofi_fabric * F, struct fi_info *fi,
    struct ofi_domain ** domain )
{
    int ret;
    struct ofi_domain *item;
    struct nn_list_item *it;

    /* TODO: Reuse previously allcoated domain that matches
             the specified info. */
    /*
    for (it = nn_list_begin (&F->domains);
          it != nn_list_end (&F->domains);
          it = nn_list_next (&F->domains, it)) {
        item = nn_cont (it, struct ofi_domain, item);
    }
    */

    /* Allocate new domain */
    item = nn_alloc( sizeof(struct ofi_domain), "ofi domain" );
    nn_assert( item );

    /* Use fabric FI if fi missing */
    if (!fi) fi = F->fi;

    /* Open new domain */
    ret = fi_domain(F->fabric, fi, &item->domain, NULL);
    if (ret) {
        FT_PRINTERR("fi_domain", ret);
        nn_free(item);
        *domain = NULL;
        return ret;
    }

    /* Prepare properties */
    item->parent = F;

    /* Keep this domain on list */
    nn_list_item_init( &item->item );
    nn_list_insert (&F->domains, &item->item,
        nn_list_end (&F->domains));

    /* We are good */
    *domain = item;
    return 0;

}

/**
 * Close a domain previously created with `ofi_domain_open`.
 */
int ofi_domain_close( struct ofi_domain * domain )
{

    /* Release libfabric structures */
    FT_CLOSE_FID( domain->domain );

    /* Remove domain from list */
    nn_list_erase (&domain->parent->domains, &domain->item);

    /* Free nanomsg structures */
    nn_list_item_term( &domain->item );

    /* Free memory */
    nn_free(domain);

    /* Success */
    return 0;

}

/* ########################################################################## */
/*  Passive Endpoint Functions                                                */
/* ########################################################################## */

/**
 * Open a new passive endpoint on this domain
 */
int ofi_passive_endpoint_open( struct ofi_fabric * fabric, struct nn_ofiw * wrk,
    int src, void * context, struct ofi_passive_endpoint ** _pep )
{
    int ret;
    struct ofi_passive_endpoint * pep;

    /* If we have already a PEP structure, re-use it */
    if (*_pep) {
        /* Close previous passive endpoint */
        pep = *_pep;
        ofi_passive_endpoint_close( pep );
    } else {
        /* Allocate new endpoint */
        pep = nn_alloc( sizeof(struct ofi_passive_endpoint), "ofi passive ep" );
        nn_assert( pep );
    }

    /* Open a passive endpoint */
    ret = fi_passive_ep(fabric->fabric, fabric->fi, &pep->ep, context);
    if (ret) {
        FT_PRINTERR("fi_passive_ep", ret);
        nn_free(pep);
        *_pep = NULL;
        return ret;
    }

    /* Initialize properties */
    pep->fabric = fabric;
    pep->worker = wrk;

    /* ###[ EVENT QUEUE ]#################################################### */

    /* Preare EQ attrib */
    struct fi_eq_attr eq_attr = {
        .size = 1,
        .flags = FI_WRITE,
        .wait_obj = FI_WAIT_NONE,
        .wait_set = NULL,
    };

    /* Open an event queue for this endpoint, through poller */
    ret = nn_ofiw_open_eq( wrk, src | OFI_SRC_EQ, context, &eq_attr, &pep->eq );
    if (ret) {
        FT_PRINTERR("nn_ofiw_open_eq", ret);
        nn_free(pep);
        *_pep = NULL;
        return ret;
    }

    /* Bind the EQ to the endpoint */
    ret = fi_pep_bind(pep->ep, &pep->eq->fid, 0);
    if (ret) {
        FT_PRINTERR("fi_pep_bind", ret);
        return ret;
    }

    /* Success */
    *_pep = pep;
    return 0;
}

/**
 * Close a previously allocated passive endpoint
 */
int ofi_passive_endpoint_close( struct ofi_passive_endpoint * pep )
{

    /* Remove EQ from worker, to stop monitoring them */
    nn_ofiw_remove( pep->worker, pep->eq );

    /* Drain event queue */
    struct fi_eq_cm_entry entry;
    uint32_t event;
    ssize_t rd;
    do {
        rd = fi_eq_read(pep->eq, &event, &entry, sizeof entry, 0);
    } while ((int)rd == 0);

    /* Close endpoint */
    FT_CLOSE_FID( pep->ep );
    pep->ep = NULL;

    /* Free structures */
    FT_CLOSE_FID( pep->eq );
    pep->eq = NULL;

    /* Success */
    return 0;

}

/* ########################################################################## */
/*  Active Endpoint Functions                                                 */
/* ########################################################################## */

/**
 * Open a new passive endpoint on this domain
 */
int ofi_active_endpoint_open( struct ofi_domain* domain, struct nn_ofiw* wrk,
    int src, void *context, struct fi_info *fi, struct ofi_active_endpoint** a )
{
    int ret;
    struct ofi_active_endpoint * aep;

    /* If we have already a n endpoint structure re-use it */
    if (*a) {
        /* Close previous passive endpoint */
        aep = *a;
        ofi_active_endpoint_close( aep );
    } else {
        /* Allocate new endpoint */
        aep = nn_alloc( sizeof(struct ofi_active_endpoint), "ofi active ep" );
        nn_assert( aep );
    }

    /* Use domain FI if fi missing */
    if (!fi) fi = domain->fi;

    /* Open an active endpoint */
    ret = fi_endpoint(domain->domain, fi, &aep->ep, context);
    if (ret) {
        FT_PRINTERR("fi_endpoint", ret);
        nn_free(aep);
        *a = NULL;
        return ret;
    }

    /* Initialize properties */
    aep->domain = domain;
    aep->worker = wrk;

    /* ###[ EVENT QUEUE ]#################################################### */

    /* Preare EQ attrib */
    struct fi_eq_attr eq_attr = {
        .size = 1,
        .flags = FI_WRITE,
        .wait_obj = FI_WAIT_NONE,
        .wait_set = NULL,
    };

    /* Open an event queue for this endpoint, through poller */
    ret = nn_ofiw_open_eq( wrk, src | OFI_SRC_EQ, context, &eq_attr, &aep->eq );
    if (ret) {
        FT_PRINTERR("nn_ofiw_open_eq", ret);
        nn_free(aep);
        *a = NULL;
        return ret;
    }

    /* Bind the EQ to the endpoint */
    ret = fi_ep_bind(aep->ep, &aep->eq->fid, 0);
    if (ret) {
        FT_PRINTERR("fi_ep_bind[eq]", ret);
        return ret;
    }

    /* ###[ TX COMPLETION QUEUE ]############################################ */

    /* Prepare CQ attrib */
    struct fi_cq_attr cq_attr = {
        .size = fi->tx_attr->size,
        .flags = 0,
        .format = FI_CQ_FORMAT_DATA,
        .wait_cond = FI_CQ_COND_NONE,
        .wait_obj = FI_WAIT_NONE,
        .wait_set = NULL,
    };

    /* Open an event queue for this endpoint, through poller */
    ret = nn_ofiw_open_cq( wrk, src | OFI_SRC_CQ_TX, domain->domain, context, 
        &cq_attr, &aep->cq_tx );
    if (ret) {
        FT_PRINTERR("nn_ofiw_open_cq", ret);
        nn_free(aep);
        *a = NULL;
        return ret;
    }

    /* Bind the CQ to the endpoint */
    ret = fi_ep_bind(aep->ep, &aep->cq_tx->fid, FI_TRANSMIT);
    if (ret) {
        FT_PRINTERR("fi_ep_bind[cq_tx]", ret);
        return ret;
    }

    /* ###[ RX COMPLETION QUEUE ]############################################ */

    /* Prepare CQ attrib */
    cq_attr.size = fi->rx_attr->size;

    /* Open an event queue for this endpoint, through poller */
    ret = nn_ofiw_open_cq( wrk, src | OFI_SRC_CQ_RX, domain->domain, context, 
        &cq_attr, &aep->cq_rx );
    if (ret) {
        FT_PRINTERR("nn_ofiw_open_cq", ret);
        nn_free(aep);
        *a = NULL;
        return ret;
    }

    /* Bind the CQ to the endpoint */
    ret = fi_ep_bind(aep->ep, &aep->cq_rx->fid, FI_RECV);
    if (ret) {
        FT_PRINTERR("fi_ep_bind[cq_rx]", ret);
        return ret;
    }

    /* ###[ FINALIZATION ]################################################### */

    /* Enable endpoint */
    ret = fi_enable(aep->ep);
    if (ret) {
        FT_PRINTERR("fi_enable", ret);
        return ret;
    }

    /* Success */
    *a = aep;
    return 0;
}

/**
 * Close a previously allocated passive endpoint
 */
int ofi_active_endpoint_close( struct ofi_active_endpoint * aep )
{

    /* Remove EQ/CQs from worker, to stop monitoring them */
    nn_ofiw_remove( aep->worker, aep->eq );
    nn_ofiw_remove( aep->worker, aep->cq_tx );
    nn_ofiw_remove( aep->worker, aep->cq_rx );

    /* Drain event queue */
    struct fi_eq_cm_entry entry;
    uint32_t event;
    ssize_t rd;
    do {
        rd = fi_eq_read(aep->eq, &event, &entry, sizeof entry, 0);
    } while ((int)rd == 0);

    /* Close endpoint */
    FT_CLOSE_FID( aep->ep );
    aep->ep = NULL;

    /* Close CQs */
    FT_CLOSE_FID( aep->cq_tx );
    aep->cq_tx = NULL;
    FT_CLOSE_FID( aep->cq_rx );
    aep->cq_rx = NULL;

    return 0;
}

/* ########################################################################## */
/*  Connection Management Operations                                          */
/* ########################################################################## */

/**
 * Bring a passive endpoint in listening state. When a connection request is
 * sent, the the FI_CONNREQ event will be triggered by the associated worker.
 *
 * Note tha the source will be masked with OFI_SRC_EQ
 */
int ofi_cm_listen( struct ofi_passive_endpoint * pep )
{
    int ret;

    /* Listen for incoming connection */
    ret = fi_listen(pep->ep);
    if (ret) {
        FT_PRINTERR("fi_listen", ret);
        return ret;
    }

    /* Success */
    return 0;
}

/**
 * Accept an incoming request
 *
 * The active endpoint must be created using the `fi_info` that came as
 * part of the FI_CONNREQ EQ event. 
 *
 * You can optionally specify an payload that will be sent as part of the
 * handshake sequence to the other end. This data will be available as
 * part of the FI_CONNECTED EQ event.
 *
 * This will trigger a FI_CONNECTED event to the endpoint's EQ when the
 * connection is established.
 *
 */
int ofi_cm_accept( struct ofi_active_endpoint * ep, const void *data, 
    size_t datalen )
{
    int ret;

    /* Accept the incoming connection */
    ret = fi_accept(ep->ep, data, datalen);
    if (ret) {
        FT_PRINTERR("fi_accept", ret);
        return ret;
    }

    /* Success */
    return 0;
}

/**
 * Connect to a remote endpoint
 *
 * The destination address is either specified as an argument, or if `NULL`,
 * it's assumed to be the address used to create the domain.
 *
 * You can optionally specify an payload that will be sent as part of the
 * handshake sequence to the other end. This data will be available as
 * part of the FI_CONNECTED EQ event.
 */
int ofi_cm_connect( struct ofi_active_endpoint * ep, void *addr,
    const void *data, size_t datalen )
{
    int ret;

    /* Get domain's dest address if addr is null */
    if (!addr)
        addr = ep->domain->fi->dest_addr;

    /* Connect to server */
    ret = fi_connect(ep->ep, addr, data, datalen);
    if (ret) {
        FT_PRINTERR("fi_connect", ret);
        return ret;
    }

    /* Success */
    return 0;
}

/* ########################################################################## */
/*  Data Transmission Operations                                              */
/* ########################################################################## */

