#!/bin/bash
################################################################
#
#    Utility Script to run a program with the correct afinity
#         in order to share the same IRQs with the NIC
#
# -------------------------------------------------------------
# This utility identifies the IRQs of the specified NIC, validates
# that they are all on the same NUMA domain, and then runs taskset
# with the appropriate arguments in order to use only the cores
# on which the NIC sends interrupt signals upon.
################################################################

#
# Get all relevant IRqs of this network device
#
function get_iface_irq_list {
    local IFACE=$1
    local IFACE_PATH="/sys/class/net/$IFACE"
    local IFACE_DEV_IRQS="$IFACE_PATH/device/msi_irqs"
    if [ -d $IFACE_PATH ]; then
        if [ $(cat /proc/interrupts | grep -c "$IFACE-" ) -eq 0 ]; then
            if [ -d $IFACE_DEV_IRQS ]; then
                ls $IFACE_DEV_IRQS
            else
                echo "ERROR: No device IRQs for $IFACE" 1>&2
            fi
        else
            cat /proc/interrupts | grep "$IFACE-" | awk '{print $1}' | tr -d ':'
        fi
    else
        echo "ERROR: Missing interface $IFACE" 1>&2
    fi
}

#
# Check if CPU arch is sandy bridge
#
function has_sandy {
    local CPU_FAMILY=$(cat /proc/cpuinfo | grep -i "CPU family" -m 1 | awk '{print $NF}' )
    local MODEL=$(cat /proc/cpuinfo | grep -i "Model" -m 1 | awk '{print $NF}' )
    if [ $CPU_FAMILY -eq 6 ] && [ $MODEL -eq 45 ] ; then
        echo 1
    else    
        echo 0
    fi
}

#
# Get specified network interface NUMA node
#
function get_iface_numa_node {
    local IFACE=$1
    local HAS_NUMA=$(ls /sys/class/net/$IFACE/device | grep -c numa_node)
    if [ $HAS_NUMA -eq 0 ]; then
        # Unknown numa node
        echo -1
    else
        # Get numa node
        cat /sys/class/net/$IFACE/device/numa_node
    fi
}

#
# Get CPU IDs on this numa node
#
function get_numa_cores {
    local NODE=$1
    if [ $NODE -lt 0 ]; then
        # We don't have a numa node (ex. no sandy bridge)
        # Get all cores
        cat /proc/cpuinfo  | grep processor | awk -F':' '{print $2}'
    else
        # Get the
        numactl --hardware | grep "node $NODE cpus" | awk -F':' '{print $2}'
    fi
}

#
# Convert a core list to an afinity list
#
function core_to_afinity {
    local MAX_CORES=$(cat /proc/cpuinfo | grep -c processor)
    local BLOCK_1=0
    local BLOCK_2=0

    # Convert cores to afinity lists
    for CORE in $*; do
        if [ $CORE -gt 31 ]; then
            let CORE-=31
            let "V=1<<$CORE"
            let "BLOCK_2|=$V"
        else
            let "V=1<<$CORE"
            let "BLOCK_1|=$V"
        fi
    done

    # Render
    if [ $MAX_CORES -gt 31 ]; then
        printf '%08x,%08x' $BLOCK_2 $BLOCK_1
    else
        printf '%08x' $BLOCK_1
    fi
}

#
# Convert an afinity list to core list
#
function afinity_to_core {
    local MAX_CORES=$(cat /proc/cpuinfo | grep -c processor)
    local BLOCK_2=$(echo $1 | awk -F',' '{print $1}')
    local BLOCK_1=$(echo $1 | awk -F',' '{print $2}')
    [ -z "$BLOCK_1" ] && BLOCK_1="$BLOCK_2"

    # Convert to number
    BLOCK_1=$((16#$BLOCK_1))
    BLOCK_2=$((16#$BLOCK_2))

    # Render
    local V=1
    for I in $(seq 0 31); do
        let "T=BLOCK_1&V"
        let "V=V<<1"
        [ $T -ne 0 ] && echo $I
    done
    if [ $MAX_CORES -gt 31 ]; then
        V=1
        for I in $(seq 32 63); do
            let "T=BLOCK_2&V"
            let "V=V<<1"
            [ $T -ne 0 ] && echo $I
        done
    fi

}

#
# Get core afinity of the specified IRQ
#
function get_irq_core_afinity {
    {
        for IRQ in $*; do
            afinity_to_core $(cat /proc/irq/$IRQ/smp_affinity)
        done
    } | sort -n | uniq
}

#
# Test if the IRQs of the specified interface 
# are all on the same numa domain 
#
function check_numa_domain {
    local IFACE=$1
    let FOUND=0

    # Get the cores of the IRQs of this iface
    local IFACE_IRQS=$(get_iface_irq_list $IFACE)
    local IRQ_CORES=$(get_irq_core_afinity $IFACE_IRQS)

    # Get the NUMA node of this interface
    local IFACE_NODE=$(get_iface_numa_node $IFACE)
    local NUMA_CORES=$(get_numa_cores $IFACE_NODE)

    # Test if all IRQ cores are in the numa cores
    for C in $IRQ_CORES; do
        FOUND=0
        for N in $NUMA_CORES; do
            if [ $C -eq $N ]; then
                FOUND=1
                break
            fi
        done
        if [ $FOUND -eq 0 ]; then
            echo "ERROR: IRQ core $C not in NUMA node $IFACE_NODE" 1>&2
            echo 0
            return
        fi
    done

    # All tested succssfully
    echo 1
    return
}

#
# Get a list of 
#
function get_taskset_cores {
    local IFACE=$1
    local IFACE_IRQS=$(get_iface_irq_list $IFACE)
    local IFACE_CORES=$(get_irq_core_afinity $IFACE_IRQS)
    local CORE_LIST=""

    # Get the interface cores
    for C in $IFACE_CORES; do
        [ ! -z "$CORE_LIST" ] && CORE_LIST="$CORE_LIST,"
        CORE_LIST="$CORE_LIST$C"
    done

    echo $CORE_LIST
}

# Validate command-line
if [ -z "$1" ]; then
    echo "ERROR: Please specify the interface to run your command on" 1>&2
    echo "Usage: $0 [interface] [command ...]"
    exit 1
fi

# Test command-line
IFACE="$1"; shift
if [ -z "$1" ]; then
    echo "ERROR: Please specify a command-line to run" 1>&2
    echo "Usage: $0 [interface] [command ...]"
    exit 1
fi

# Get interface
if [ ! -d /sys/class/net/$IFACE ]; then
    echo "ERROR: Interface $IFACE not found!" 1>&2
    echo "Usage: $0 [interface] [command ...]"
    exit 1
fi

# Validate NUMA
if [ $(check_numa_domain $IFACE) -eq 0 ]; then
    echo "ERROR: The interface's interrupts have afinity not part of the same NUMA node!" 1>&2
    exit 2
else
    echo "INFO: IRQs are correctly assigned to the same NUMA node!" 1>&2
fi

# Run
IFACE_CORES=$(get_taskset_cores $IFACE)
echo "INFO: Running on cores: $IFACE_CORES" 1>&2
taskset -c $IFACE_CORES $*
