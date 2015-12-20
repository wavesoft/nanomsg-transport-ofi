
OFI_DIR=../src/transports/ofi

# Change this to point to your nanomsg sources if you prefer
#NANOMSG_LIB_DIR=/usr/local/lib
NANOMSG_LIB_DIR=/Users/icharala/Develop/predrag/nanomsg-0.8-beta/.libs

.PHONY: all

all: test_msg test_dgram test_nanmsg_timing

hlapi.o: $(OFI_DIR)/hlapi.c
	gcc -D__STANDALONE_TESTS -c -I$(OFI_DIR) -I$(PWD) $(OFI_DIR)/hlapi.c -g -o hlapi.o

test_dgram: hlapi.o fabric_dgram.c
	gcc -D__STANDALONE_TESTS -I$(OFI_DIR) hlapi.o fabric_dgram.c -lfabric -g -o test_dgram

test_msg: hlapi.o fabric_msg.c
	gcc -D__STANDALONE_TESTS -I$(OFI_DIR) hlapi.o fabric_msg.c -lfabric -g -o test_msg

test_nanmsg_timing: nanomsg_timing.c
	gcc nanomsg_timing.c $(NANOMSG_LIB_DIR)/libnanomsg.a -lfabric -o test_nanmsg_timing

clean:
	rm -rf \
		hlapi.o \
		test_dgram \
		test_dgram.dSYM \
		test_msg \
		test_msg.dSYM \
		test_nanmsg_timing \
		test_nanmsg_timing.dSYM
