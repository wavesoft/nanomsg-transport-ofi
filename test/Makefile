
OFI_DIR=../src/transports/ofi

# Change this to point to your nanomsg sources if you prefer
NANOMSG_LIB_DIR=/usr/local/lib

.PHONY: all

all: test_nanmsg_timing test_msg test_msg_timed test_dgram

hlapi.o: $(OFI_DIR)/hlapi.c
	gcc -D__STANDALONE_TESTS -c -I$(OFI_DIR) -I$(PWD) $(OFI_DIR)/hlapi.c -g -o hlapi.o

test_dgram: hlapi.o fabric_dgram.c
	gcc -D__STANDALONE_TESTS -I$(OFI_DIR) hlapi.o fabric_dgram.c -lfabric -g -o test_dgram

test_msg: hlapi.o fabric_msg.c
	gcc -D__STANDALONE_TESTS -I$(OFI_DIR) hlapi.o fabric_msg.c -lfabric -g -o test_msg

test_msg_timed: hlapi.o fabric_msg.c
	gcc -D__STANDALONE_TESTS -I$(OFI_DIR) hlapi.o fabric_msg_timed.c -lfabric -g -o test_msg_timed

test_nanmsg_timing: nanomsg_timing.c $(NANOMSG_LIB_DIR)/libnanomsg.a
	gcc nanomsg_timing.c $(NANOMSG_LIB_DIR)/libnanomsg.a -lfabric -o test_nanmsg_timing

clean:
	rm -rf \
		hlapi.o \
		test_dgram \
		test_dgram.dSYM \
		test_msg \
		test_msg.dSYM \
		test_msg_timed \
		test_msg_timed.dSYM \
		test_nanmsg_timing \
		test_nanmsg_timing.dSYM
