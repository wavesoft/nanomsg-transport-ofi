#!/bin/bash

# Validate input
[ -z "$1" ] && echo "ERROR: Please specify the full path to the nanomsg sources!" && exit 1
NANOMSG_DIR=$1
pushd `dirname $0` > /dev/null
CURR_PATH=`pwd`
popd > /dev/null

# Do some quick-check to make sure that's a nanomsg directory
[ ! -f "${NANOMSG_DIR}/libnanomsg.pc.in" ] && echo -e "** FAILED **\nThis does not look like a nanomsg directory!" && exit 1

# Get current git version
GIT_VERSION=$(git rev-parse HEAD)

# Check if we already have patched that source
if [ -f "${NANOMSG_DIR}/.nanomsg-ofi-patch.version" ]; then

	# Check if version matches
	DIR_VERSION=$(cat "${NANOMSG_DIR}/.nanomsg-ofi-patch.version")
	if [ "$DIR_VERSION" == "$GIT_VERSION" ]; then
		echo "** UP TO DATE **"
		exit 0
	fi

	# Just update sources
	echo "Updating..."
	cp -v -r ${CURR_PATH}/src/transports/ofi ${NANOMSG_TRANSPORTS_OFI_DIR}
	[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to copy sources!" && exit 1
	cp -v -r ${CURR_PATH}/src/ofi.h ${NANOMSG_SRC_DIR}/ofi.h
	[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to copy sources!" && exit 1

	# Update patch version
	echo $GIT_VERSION > "${NANOMSG_DIR}/.nanomsg-ofi-patch.version"
	echo "** UPDATED **"
	exit 0

fi

# Check if we already have the ofi directory
NANOMSG_SRC_DIR="${NANOMSG_DIR}/src"
NANOMSG_TRANSPORTS_OFI_DIR="${NANOMSG_SRC_DIR}/transports/ofi"
[ -L "${NANOMSG_TRANSPORTS_OFI_DIR}" ] && echo -e "** FAILED **\nOFI Transport seems to be installed already!" && exit 1
[ -d "${NANOMSG_TRANSPORTS_OFI_DIR}" ] && echo -e "** FAILED **\nOFI Transport seems to be installed by another source!" && exit 1

# Apply patches
echo ${NANOMSG_DIR}
echo "Patching..."
patch -p0 -d ${NANOMSG_DIR} < ${CURR_PATH}/patch/nanomsg-patch-global.patch
[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to apply patch to src/core/global.c!" && exit 1
patch -p0 -d ${NANOMSG_DIR} < ${CURR_PATH}/patch/nanomsg-patch-symbol.patch
[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to apply patch to src/core/symbol.c!" && exit 1
patch -d ${NANOMSG_DIR} < ${CURR_PATH}/patch/nanomsg-patch-makefile.patch
[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to apply patch to Makefile.am!" && exit 1

# Link sources directory
echo "Copying..."
cp -v -r ${CURR_PATH}/src/transports/ofi ${NANOMSG_TRANSPORTS_OFI_DIR}
[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to copy sources!" && exit 1
cp -v -r ${CURR_PATH}/src/ofi.h ${NANOMSG_SRC_DIR}/ofi.h
[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to copy sources!" && exit 1

# Running autoreconf
echo "Re-creating autoconfig files (to apply patches)..."
(cd ${NANOMSG_DIR} && aclocal && autoconf && automake -a)
[ $? -ne 0 ] && echo -e "** FAILED **\nUnable to run aclocal/autoconf/automake!" && exit 1

# We are done
echo $GIT_VERSION > "${NANOMSG_DIR}/.nanomsg-ofi-patch.version"
echo "** SUCCESS **"
