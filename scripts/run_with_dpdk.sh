#!/bin/bash
# !
# ! Usage: ./prepare_dpdk_env.sh <NIC to use> <number of huge pages per NUMA Node> <command to execute> [command parameters]
# !
# ! Prepares the DPDK environment (binds a given NIC to UIO, allocates the required
# ! number of huge pages) and executes the given command in it.
# ! After the command terminates the original environment is restored apart from
# ! huge pages, that remain allocated.
# !

usage()
{
    cat $0 | grep ^"# !" | cut -d"!" -f2-
}

#
#  check_stat_and_exit <error message>
#
check_stat_and_exit()
{
    if [[ $? -ne 0 ]]; then
        echo $@
        exit 1
    fi
}

rollback()
{
    echo "Binding $NIC($BDF) back to $DRIVER..."
    $SCRIPTS_DIR/dpdk_nic_bind.py -u $BDF
    $SCRIPTS_DIR/dpdk_nic_bind.py -b $DRIVER $BDF
}

check_stat_and_rollback()
{
    if [[ $? -ne 0 ]]; then
        echo $@
        rollback
        exit 1
    fi
}

# Check number of parameters
if [[ $# -lt 3 ]]; then
    usage
    exit 1
fi

NIC=$1
shift
NUM_HUGE_PAGES_PER_NODE=$1
shift
SCRIPTS_DIR=`dirname $0`


ifconfig $NIC down
check_stat_and_exit "Failed to shut down $NIC. Is $NIC present? Are your permissions sufficient?"

DRIVER=`ethtool -i $NIC | grep driver | cut -d":" -f2- | tr -d ' '`
BDF=`ethtool -i $NIC | grep bus-info | cut -d":" -f2- |  tr -d ' '`

# command to execute
CMD=$@

echo "Binding $NIC($BDF) to uio_pci_generic..."
$SCRIPTS_DIR/dpdk_nic_bind.py -u $BDF
check_stat_and_exit
$SCRIPTS_DIR/dpdk_nic_bind.py -b uio_pci_generic $BDF
check_stat_and_rollback

echo "Allocating $NUM_HUGE_PAGES_PER_NODE 2MB huge pages on each NUMA Node:"
for d in /sys/devices/system/node/node? ; do
    echo $NUM_HUGE_PAGES_PER_NODE > $d/hugepages/hugepages-2048kB/nr_hugepages
    check_stat_and_rollback
    cur_node=`basename $d`
    echo "...$cur_node done..."
done

mkdir -p /mnt/huge
check_stat_and_rollback

grep -s '/mnt/huge' /proc/mounts > /dev/null
if [[ $? -ne 0 ]] ; then
    echo "Mounting hugetlbfs at /mnt/huge..."
    mount -t hugetlbfs nodev /mnt/huge
    check_stat_and_rollback
fi

# Run scylla
echo "Running: $CMD"
$CMD
ret=$?

# Revert the NIC binding
rollback

exit $ret

