#!/bin/bash

# Validate input
[ -z "$1" ] && echo "ERROR: Please specify the full path to the nanomsg sources!" && exit 1
NANOMSG_DIR=$1
pushd `dirname $0` > /dev/null
CURR_PATH=`pwd`
popd > /dev/null

# Do some quick-check to make sure that's a nanomsg directory
[ ! -f "${NANOMSG_DIR}/libnanomsg.pc.in" ] && echo -e "** FAILED **\nThis does not look like a nanomsg directory!" && exit 1

# Check if we already have the ofi directory
NANOMSG_SRC_DIR="${NANOMSG_DIR}/src/transports"
NANOMSG_TRANSPORTS_OFI_DIR="${NANOMSG_SRC_DIR}/transports/ofi"
[ -L "${NANOMSG_TRANSPORTS_OFI_DIR}" ] && echo -e "** FAILED **\nOFI Transport seems to be installed already!" && exit 1
[ -d "${NANOMSG_TRANSPORTS_OFI_DIR}" ] && echo -e "** FAILED **\nOFI Transport seems to be installed by another source!" && exit 1

# Apply patch to global source
echo ${NANOMSG_DIR}
echo "Patching..."
patch -p0 -d ${NANOMSG_DIR} < ${CURR_PATH}/patch/nanomsg-patch-global.patch
[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to apply patch!" && exit 1

# Apply patch to makefile
patch -d ${NANOMSG_DIR} < ${CURR_PATH}/patch/nanomsg-patch-makefile.patch
[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to apply patch!" && exit 1

# Link sources directory
echo "Copying..."
cp -v -r ${CURR_PATH}/src/transports/ofi ${NANOMSG_TRANSPORTS_OFI_DIR}
[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to copy sources!" && exit 1
cp -v -r ${CURR_PATH}/src/ofi.h ${NANOMSG_SRC_DIR}/ofi.h
[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to copy sources!" && exit 1

# We are done
echo "** SUCCESS **"
