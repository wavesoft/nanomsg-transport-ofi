
OFI_DIR=../src/transports/ofi

# Change this to point to your nanomsg sources if you prefer
NANOMSG_LIB_DIR=/usr/local/lib

.PHONY: all

all: test_nanmsg_timing test_msg test_msg_timed test_dgram

# We only need the high-level API from the ofi transport
hlapi.o: $(OFI_DIR)/hlapi.c
	gcc -D__STANDALONE_TESTS -c -I$(OFI_DIR) -I$(PWD) $(OFI_DIR)/hlapi.c -g -o hlapi.o

# Core Tests
test_dgram: hlapi.o fabric_dgram.c
	gcc -D__STANDALONE_TESTS -I$(OFI_DIR) hlapi.o fabric_dgram.c -lfabric -g -o test_dgram

test_msg: hlapi.o fabric_msg.c
	gcc -D__STANDALONE_TESTS -I$(OFI_DIR) hlapi.o fabric_msg.c -lfabric -g -o test_msg

test_msg_timed: hlapi.o fabric_msg.c
	gcc -D__STANDALONE_TESTS -I$(OFI_DIR) hlapi.o fabric_msg_timed.c -lfabric -g -o test_msg_timed

# Nanomsg tests
test_nanmsg_timing: nanomsg_timing.c $(NANOMSG_LIB_DIR)/libnanomsg.a
	gcc nanomsg_timing.c $(NANOMSG_LIB_DIR)/libnanomsg.a -lfabric -o test_nanmsg_timing

# Cleanup files
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

# Tests
test: test_msg_timed test_nanmsg_timing
	@echo "--------------------------------------------------"
	@echo "Core timing tests:"
	@echo "--------------------------------------------------"
	./test_msg_timed server 127.0.0.1&
	./test_msg_timed client 127.0.0.1

	@echo "--------------------------------------------------"
	@echo "Nanomsg ofi:// timing tests:"
	@echo "--------------------------------------------------"
	./test_nanmsg_timing node0 ofi://127.0.0.1:5050&
	./test_nanmsg_timing node1 ofi://127.0.0.1:5050

	@echo "--------------------------------------------------"
	@echo "Nanomsg tcp:// timing tests (for reference):"
	@echo "--------------------------------------------------"
	./test_nanmsg_timing node0 tcp://127.0.0.1:5050&
	./test_nanmsg_timing node1 tcp://127.0.0.1:5050
