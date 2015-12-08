
.PHONY: all

all: fabric_msg fabric_dgram

fabric_msg: fabric_msg.c
	gcc ../src/transports/ofi/libfabric_api.c fabric_msg.c -I../src/transports/ofi -lfabric -g -o fabric_msg

fabric_dgram: fabric_dgram.c
	gcc ../src/transports/ofi/libfabric_api.c fabric_dgram.c -I../src/transports/ofi -lfabric -g -o fabric_dgram

