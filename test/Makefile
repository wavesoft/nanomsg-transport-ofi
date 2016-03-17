

# Change this to point to the appropriate locations in your set-up
#
# ex: make NANOMSG_DIR=/usr
# 

# Nanomsg base directory
NANOMSG_DIR=/usr/local
NANOMSG_LIB_DIR=$(NANOMSG_DIR)/lib
NANOMSG_INCLUDE_DIR=$(NANOMSG_DIR)/include

# Libfabric base directory
LIBFABRIC_DIR=/usr/local
LIBFABRIC_LIB_DIR=$(LIBFABRIC_DIR)/lib
LIBFABRIC_INCLUDE_DIR=$(LIBFABRIC_DIR)/include

# Base dir to the nanomsg-transport-ofi directory
OFI_DIR=../src/transports/ofi

# Flags for nanomsg tests
CCFLAGS_NN=-I$(NANOMSG_INCLUDE_DIR) -I$(LIBFABRIC_INCLUDE_DIR)
LDFLAGS_NN=-L$(NANOMSG_LIB_DIR) -L$(LIBFABRIC_LIB_DIR)

# Flags for ofi tests
CCFLAGS_OFI=-D__STANDALONE_TESTS -I$(LIBFABRIC_INCLUDE_DIR) -I$(OFI_DIR)
LDFLAGS_OFI=-L$(LIBFABRIC_LIB_DIR)

# Common flags
CCFLAGS=-O0 -g
LDFLAGS=-lfabric

# Patches for linux
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CCFLAGS += -lpthread -lanl
endif

.PHONY: all clean test fabric nanomsg

# ================================================
#  Target groups
# ================================================

all: fabric nanomsg

fabric: test_fabric_dgram test_fabric_msg test_fabric_msg_timed

nanomsg: test_nanomsg_timing

# ================================================
#  Object files
# ================================================

hlapi.o: $(OFI_DIR)/hlapi.c
	gcc $(CCFLAGS_OFI) $(CCFLAGS) -I$(PWD) $(OFI_DIR)/hlapi.c -c -o $@

common.o: common.c
	gcc $(CCFLAGS_OFI) $(CCFLAGS) common.c -c -o $@

# ================================================
#  Target binaries
# ================================================

# Fabric tests
test_fabric_dgram: fabric_dgram.c hlapi.o
	gcc $(CCFLAGS_OFI) $(CCFLAGS) $^ $(LDFLAGS_OFI) $(LDFLAGS) -o $@

test_fabric_msg: fabric_msg.c hlapi.o
	gcc $(CCFLAGS_OFI) $(CCFLAGS) $^ $(LDFLAGS_OFI) $(LDFLAGS) -o $@

test_fabric_msg_timed: fabric_msg_timed.c hlapi.o
	gcc $(CCFLAGS_OFI) $(CCFLAGS) $^ $(LDFLAGS_OFI) $(LDFLAGS) -o $@

# Nanomsg tests
test_nanomsg_timing: nanomsg_timing.c common.o hlapi.o $(NANOMSG_LIB_DIR)/libnanomsg.a
	gcc $(CCFLAGS_NN) $(CCFLAGS) $^ $(LDFLAGS_NN) $(LDFLAGS) -o $@

# ================================================
#  Phony targets
# ================================================

# Cleanup files
clean:
	rm -rf \
		hlapi.o \
		test_fabric_dgram \
		test_fabric_dgram.dSYM \
		test_fabric_msg \
		test_fabric_msg.dSYM \
		test_fabric_msg_timed \
		test_fabric_msg_timed.dSYM \
		test_nanomsg_timing \
		test_nanomsg_timing.dSYM

# # Tests
# test: test_msg_timed test_nanomsg_timing
# 	@echo "--------------------------------------------------"
# 	@echo "Core timing tests:"
# 	@echo "--------------------------------------------------"
# 	./test_msg_timed server 127.0.0.1&
# 	./test_msg_timed client 127.0.0.1

# 	@echo "--------------------------------------------------"
# 	@echo "Nanomsg ofi:// timing tests:"
# 	@echo "--------------------------------------------------"
# 	./test_nanomsg_timing node0 ofi://127.0.0.1:5050&
# 	./test_nanomsg_timing node1 ofi://127.0.0.1:5050

# 	@echo "--------------------------------------------------"
# 	@echo "Nanomsg tcp:// timing tests (for reference):"
# 	@echo "--------------------------------------------------"
# 	./test_nanomsg_timing node0 tcp://127.0.0.1:5050&
# 	./test_nanomsg_timing node1 tcp://127.0.0.1:5050
