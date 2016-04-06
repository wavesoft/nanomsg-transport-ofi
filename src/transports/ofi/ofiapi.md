
# OpenFabrics Interface API for NanoMSG

This class provides the abstraction for interfacing nanomsg to libfabric. I tried to consolidate all the logic into two files: the `ofiapi.c` and `ofiw.c` in order to simplify the migration to other transports, such as `0MQ`.

## Global API

The following functions must be the first thing the library should call and they initialize the libfabric subsystem.

### `ofi_init`

```c
int ofi_init( struct ofi_resources * R, enum fi_ep_type ep_type );
```

Initialize a new libFabric instance, and prepare the core hints structure that will be used later.

### `ofi_term`

```c
int ofi_term( struct ofi_resources * R );
```

Free resources previously allocated with `ofi_init`, along with all currently reserved fabrics, domains and endpoint resources.

## Fabric API

The _fabric_ is a particular NIC in the computer, on top of which one or more different addresses (or _domains_) can exist. The fabric is the first resource that must be initialized before you can use this library.

### `ofi_fabric_open`

```c
int ofi_fabric_open( struct ofi_resources * R, const char * address,
    enum ofi_fabric_addr_flags flags, struct ofi_fabric ** F );
```

Open or re-use a previously allocated fabric that match the particular address specified. Upon successful completion, this function will populate the `F` argument with the appropriate fabric instance. 

The address format is the following:

 * `[ip]:[port]` : Were [ip] is an IPv4 or IPv6 IP address and [port] is the service number
 * `[ip]:[port]@[fabric]` : Were [fabric] is the name of the fabric, as libfabric understands (ex. `usnic_0` or `IP`).
 * `[ip]:[port]@[fabric]:[provider]` : Where [provider] is the name of the provider that manages the given fabric (ex. `usnic` or `sockets`).

It's important to specify if the address you are trying to resolve is local or remote, through the `flags` property.

For example, if the address you have specified is a local one (ex. for binding) you can use the following:

```c
ret = ofi_fabric_open( &R, "127.0.0.1:5050", OFI_ADDR_LOCAL, &fabric );
```

Or if you are trying to contact a remote address, you should use the following:

```c
ret = ofi_fabric_open( &R "192.168.78.2:5050", OFI_ADDR_REMOTE, &fabric );
```
