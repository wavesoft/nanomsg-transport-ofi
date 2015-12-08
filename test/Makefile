
OFI_DIR=../src/transports/ofi

.PHONY: all

all: test_msg test_dgram

hlapi.o: $(OFI_DIR)/hlapi.c
	gcc -c -I$(OFI_DIR) $(OFI_DIR)/hlapi.c -g -o hlapi.o

test_dgram: hlapi.o fabric_dgram.c
	gcc -I$(OFI_DIR) hlapi.o fabric_dgram.c -lfabric -g -o test_dgram

test_msg: hlapi.o fabric_msg.c
	gcc -I$(OFI_DIR) hlapi.o fabric_msg.c -lfabric -g -o test_msg

clean:
	rm -rf \
		hlapi.o \
		test_dgram \
		test_dgram.dSYM \
		test_msg \
		test_msg.dSYM
