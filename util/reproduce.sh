#!/bin/bash
################################################################
#
#    Utility Script to help through the build sequence of
#    nanomsg and the OFI transport for debugging purposes.
#
################################################################

WORK_DIR=$(pwd)/nanomsg-reproduce
LOCAL_DIR=$WORK_DIR/local
BUILD_DIR=$WORK_DIR/build
NANOMSG_DIR=$WORK_DIR/nanomsg
NANOMSG_TRANSPORT_DIR=$WORK_DIR/nanomsg-transport-ofi
TEST_BIN=$NANOMSG_TRANSPORT_DIR/test/test_nanomsg_timing

# Helper log function
function log {
	echo "INFO: $*" 1>&2
}

# Get numer of cpus in the system
function num_cpus {
	if [ -z "$(which sysctl)" ]; then
		grep -c processor /proc/cpuinfo
	else
		sysctl -n hw.ncpu
	fi
}

# Help screen
function help {
	echo "Use: reproduce.sh build [patch hash] [nanomsg hash] [--logs]"
	echo "     reproduce.sh clean"
	echo "     reproduce.sh server ofi://[ip]:[port] [packet size]"
	echo "     reproduce.sh client ofi://[ip]:[port] [packet size]"
	echo ""
	echo " build  : Build nanomsg including the OFI transport"
	echo "          The --logs flag will enable verbose logging"
	echo " clean  : Remove all files related to the test"
	echo " server : Start a listening node on the specified IP"
	echo " client : Start a connecting node on the specified IP"
	echo ""
	exit 1
}

# Show an error message and exit
function die {
	echo "ERROR: $*" 1>&1
	echo "*** FAILED ***" 1>&2
	exit 1
}

# Create workdir and local dir if missing
function prepare_workdir {
	if [ ! -d $WORK_DIR ]; then log "Creating $WORK_DIR" && mkdir $WORK_DIR || return 1; fi
	if [ ! -d $LOCAL_DIR ]; then log "Creating $LOCAL_DIR" && mkdir $LOCAL_DIR || return 1; fi
	if [ ! -d $BUILD_DIR ]; then log "Creating $BUILD_DIR" && mkdir $BUILD_DIR || return 1; fi
	return 0
}

# Clean (re-)deply of nanomsg
function deploy_nanomsg {
	local BRANCH=$1
	if [ -d $NANOMSG_DIR ]; then
		# Update nanomsg dir
		cd $NANOMSG_DIR
		log "Updating nanomsg"
		git pull || return 1
	else
		# Or check-out
		log "Downloading nanomsg"
		git clone https://github.com/wavesoft/nanomsg.git $NANOMSG_DIR || return 1
		cd $NANOMSG_DIR
	fi

	# Switch branch
	local CURRENT=$(git rev-parse --abbrev-ref HEAD)
	if [ "$CURRENT" != "$BRANCH" ]; then
		log "Reseting nanomsg git repository"
		git reset --hard || return 1
		git clean -f -x -d || return 1
		log "Switching to nanosmg branch $BRANCH"
		git checkout $BRANCH || return 1
	fi

	# Generate build files
	./autogen.sh || return 1
	return 0	
}

# Apply OFI Transport patch
function deploy_ofi_patch {
	local BRANCH=$1
	if [ -d $NANOMSG_TRANSPORT_DIR ]; then
		# Update transport dir
		cd $NANOMSG_TRANSPORT_DIR
		log "Updating nanomsg-transport-ofi"
		git pull || return 1
	else
		# Or check-out
		log "Downloading nanomsg-transport-ofi patch"
		git clone https://github.com/wavesoft/nanomsg-transport-ofi.git \
			$NANOMSG_TRANSPORT_DIR || return 1
		cd $NANOMSG_TRANSPORT_DIR
	fi

	# Switch branch
	local CURRENT=$(git rev-parse --abbrev-ref HEAD)
	if [ "$CURRENT" != "$BRANCH" ]; then
		log "Switching to nanomsg-transport-ofi branch $BRANCH"
		git checkout $BRANCH
	fi

	# Apply patch
	log "Applying patch to nanomsg"
	./patch-nanomsg.sh $NANOMSG_DIR || return 1
	return 0
}

# Configure for rebuild
function build_nanomsg {
	local LOGS_ENABLED="$1"

	cd $BUILD_DIR
	log "Configuring nanomsg"
	$NANOMSG_DIR/configure --enable-ofi --$LOGS_ENABLED-ofi-logs --prefix=$LOCAL_DIR || return 1
	log "Building nanomsg"
	CFLAGS=$FLAGS make -j$(num_cpus) || return 1
	log "Installing nanomsg"
	make install || return 1
	return 0
}

# Build nanomsg test files
function build_tests {
	cd $NANOMSG_TRANSPORT_DIR/test
	log "Building nanomsg-transport-ofi tests"
	make nanomsg NANOMSG_DIR=$LOCAL_DIR || return 1
	return 0
}

# Validate command
CMD=$1
if [ -z "$CMD" ]; then
	echo "ERROR: Missing command" 1>&2
	help
fi

# Handle cases
case $CMD in

	build)
		
		# Default values
		LOGS="disable"
		BRANCH_NN="pull-nn_allocmsg_ptr"
		BRANCH_PATCH="devel-ofiw"
		IDX=0

		# Process arguments
		shift
		while [ ! -z "$1" ]; do
			case $1 in
				--logs)
					log "Enabling verbose logging in OFI" 
					LOGS="enable" ;;
				--*) die "Unknown flag '$1'" ;;
				*) if [ $IDX -eq 0 ]; then
				   		BRANCH_PATCH="$1"
				   elif [ $IDX -eq 1 ]; then
				   		BRANCH_NN="$1"
				   else
					   	die "Unexpected argument '$1' ($IDX)"
				   fi
				   let IDX++
				   ;;
			esac
			shift
		done

		# Prepare
		prepare_workdir || die "Cannot prepare working directory"
		deploy_nanomsg $BRANCH_NN || die "Cannot deploy nanomsg"
		deploy_ofi_patch $BRANCH_PATCH || die "Cannot deploy OFI patch"
		build_nanomsg $LOGS || die "Cannot build nanomsg"
		build_tests || die "Cannot build tests"
		log "Ready! You can now use 'server' and 'client' commands"
		echo "*** SUCCESS ***" 1>&2
		;;

	clean)
		log "Removing $WORK_DIR"
		[ -d $WORK_DIR ] && rm -rf $WORK_DIR || die "Cannot remove working directory"
		echo "*** SUCCESS ***" 1>&2
		;;

	server)
		[ ! -f $TEST_BIN ] && die "Cannot find $TEST_BIN, make sure you ran 'build' first!"
		shift
		export LD_LIBRARY_PATH=$LOCAL_DIR/lib:$LD_LIBRARY_PATH
		export DYLD_LIBRARY_PATH=$LOCAL_DIR/lib:$LD_LIBRARY_PATH
		$TEST_BIN node0 $*
		;;

	client)
		[ ! -f $TEST_BIN ] && die "Cannot find $TEST_BIN, make sure you ran 'build' first!"
		shift
		export LD_LIBRARY_PATH=$LOCAL_DIR/lib:$LD_LIBRARY_PATH
		export DYLD_LIBRARY_PATH=$LOCAL_DIR/lib:$LD_LIBRARY_PATH
		$TEST_BIN node1 $*
		;;

	*)	echo "ERROR: Unknown command $CMD!" 1>&2
		help
		;;
esac
