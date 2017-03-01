#!/bin/bash
# !
# !  Usage: posix_net_conf.sh [iface name, eth0 by default] [-mq|-sq] [--cpu-mask] [-h|--help] [--use-cpu-mask <mask>]
# !
# !  Ban NIC IRQs from being moved by irqbalance.
# !
# !  -sq - set all IRQs of a given NIC to CPU0 and configure RPS
# !        to spreads NAPIs' handling between other CPUs.
# !
# !  -mq - distribute NIC's IRQs among all CPUs instead of binding
# !        them all to CPU0. In this mode RPS is always enabled to
# !        spreads NAPIs' handling between all CPUs.
# !
# !  If there isn't any mode given script will use a default mode:
# !     - If number of physical CPU cores per Rx HW queue is greater than 4 - use the '-sq' mode.
# !     - Otherwise use the '-mq' mode.
# !
# !  --use-cpu-mask <mask> - mask of cores to use, by default use all available cores
# !
# !  --cpu-mask - Print out RPS CPU assignments. On MQ NIC, just print all cpus.
# !
# !  -h|--help - print this help information
# !
# !  Enable XPS, increase the default values of somaxconn and tcp_max_syn_backlog.
# !

#
#  set_one_mask <config file> <CPU mask>
#
set_one_mask()
{
    local cpuset_conf_file=$1
    local mask=`echo $2 | sed -e 's/0x//g'`
    echo "Setting mask $mask in $cpuset_conf_file"
    echo $mask > $cpuset_conf_file
}

#
# setup_rfs <iface>
#
setup_rfs()
{
    local iface=$1
    local rfs_table_size=32768
    local rfs_limits=( `ls -1 /sys/class/net/$iface/queues/*/rps_flow_cnt` )
    local one_q_limit=$(( rfs_table_size / ${#rfs_limits[*]} ))
    local rfs_limit_cnt

    # If RFS feature is not present - get out
    ! sysctl net.core.rps_sock_flow_entries &> /dev/null && return

    # Enable RFS
    sysctl -w net.core.rps_sock_flow_entries=$rfs_table_size

    # Set each RPS queue limit
    for rfs_limit_cnt in ${rfs_limits[@]}
    do
        echo "Setting limit $one_q_limit in $rfs_limit_cnt"
        echo $one_q_limit > $rfs_limit_cnt
    done

    # Enable ntuple filtering HW offload on the NIC
    echo -n "Trying to enable ntuple filtering HW offload for $iface..."
    if ethtool -K $iface ntuple on &> /dev/null; then
        echo "ok"
    else
        echo "not supported"
    fi
}

#
# setup_rps <iface> [--no-cpu0]
#
# Bind RPS queues to spcific CPUs. if '--no-cpu0' is given CPU0 and its hyper-threading siblings are excluded.
#
# Use hwloc-distrib for generating the appropriate CPU masks.
#
setup_rps()
{
    local iface=$1
    local no_cpu0=""

    [[ "$2" == "--no-cpu0" ]] && no_cpu0="yes"

    # If we are in a single core environment - there is no point in configuring RPS
    [[ `hwloc-calc core:0.pu:all` -eq `hwloc-calc all` ]] && return

    local rps_cpus=( `get_rps_cpus $iface` )
    local mask

    # Each RPS queue is a separate RSS state machine so let them spread steams
    # between all available PUs
    if [[ -n "$no_cpu0" ]]; then
        mask=`hwloc-calc $CPU_FILTER_MASK ~core:0`
    else
        mask=`hwloc-calc $CPU_FILTER_MASK`
    fi

    local one_rps_cpus

    for one_rps_cpus in ${rps_cpus[@]}
    do
        set_one_mask "$one_rps_cpus" $mask
    done

    setup_rfs $iface
}

#
# Spread all XPS queues over the full cpuset. Don't bother to exclude CPU0
# (and friends) - scylla will just not send from it if its cpuset is properly set.
#
setup_xps()
{
    local iface=$1
    local xps_queues_count=`ls -1 /sys/class/net/$iface/queues/*/xps_cpus | wc -l`
    local mask
    local i=0

    for mask in `hwloc-distrib $xps_queues_count`
    do
        set_one_mask "/sys/class/net/$iface/queues/tx-$i/xps_cpus" $mask
        i=$(( i + 1 ))
    done
}

#
# dev_is_hw_iface <iface>
#
# Returns zero if a given interface is a physical network interface
dev_is_hw_iface()
{
    [ -e /sys/class/net/$1/device ] && return 0
    return 1
}

#
# dev_is_bond_iface <iface>
#
dev_is_bond_iface()
{
    local iface=$1

    [ ! -e /sys/class/net/bonding_masters ] && return 1

    if cat /sys/class/net/bonding_masters | grep $iface &> /dev/null; then
        return 0
    else
        return 1
    fi
}

#
# Prints IRQ numbers for the given physical interface
#
get_all_irqs_one()
{
    local iface=$1

    if test -e /sys/class/net/$iface/device/msi_irqs; then
        # Device uses MSI IRQs
        ls -1 /sys/class/net/$iface/device/msi_irqs/
    elif test -e /sys/class/net/$iface/device/irq; then
        # Device uses INT#x
        cat /sys/class/net/$iface/device/irq
    else
        # No irq file detected
        local modalias=`cat /sys/class/net/$iface/device/modalias`
        local i
        if [[ "$modalias" =~ ^virtio: ]]; then
            cd /sys/class/net/$iface/device/driver
            for i in `ls -d virtio*`; do
                grep $i /proc/interrupts|awk '{ print $1 }'|sed -e 's/:$//'
            done
            cd -
        elif [[ "$modalias" =~ ^xen:vif ]]; then
            grep $iface /proc/interrupts|awk '{ print $1 }'|sed -e 's/:$//'
        fi
    fi
}

#
# Prints IRQ numbers for the given physical interface and tries to filter the slow path queue vectors out
#
get_irqs_one()
{
    local iface=$1

    # Right now we know about Intel's and Broadcom's naming convention of the fast path queues vectors:
    #   - Intel:    <bla-bla>-TxRx-<bla-bla>
    #   - Broadcom: <bla-bla>-fp-<bla-bla>
    #
    # So, we will try to filter the etries in /proc/interrupts for IRQs we've got from get_all_irqs_one()
    # according to the patterns above.
    #
    # If as a result all IRQs are filtered out (if there are no IRQs with the names from the patterns above) then
    # this means that the given NIC uses a different IRQs naming pattern. In this case we won't filter any IRQ.
    #
    # Otherwise, we will use only IRQs which names fit one of the patterns above.
    local irqs=( `get_all_irqs_one $iface` )
    local found=""
    local irq

    for irq in ${irqs[@]}
    do
        if cat /proc/interrupts  | egrep ^"\s*$irq\:" | egrep "\-TxRx\-|\-fp\-" &> /dev/null; then
            found="yes"
            echo $irq
        fi
    done

    [ -z "$found" ] && echo ${irqs[@]}
}

#
#   get_irqs <iface>
#
get_irqs()
{
    local main_iface=$1
    local iface

    if dev_is_bond_iface $main_iface; then
        for iface in `cat /sys/class/net/$main_iface/bonding/slaves`
        do
            if dev_is_hw_iface $iface; then
                get_irqs_one $iface
            fi
        done
    else
        get_irqs_one $main_iface
    fi
}

#
# get_rps_cpus <iface>
#
# Prints all rps_cpus files names for the given HW interface.
#
# There is a single rps_cpus file for each RPS queue and there is a single RPS
# queue for each HW Rx queue. Each HW Rx queue should have an IRQ.
# Therefore the number of these files is equal to the number of fast path Rx IRQs for this interface.
#
get_rps_cpus()
{
    local iface=$1

    ls -1 /sys/class/net/$iface/queues/*/rps_cpus
}

distribute_irqs()
{
    local iface=$1
    local irqs=( `get_irqs_one $iface` )
    local mask
    local i=0

    for mask in `hwloc-distrib ${#irqs[*]} --single --restrict $CPU_FILTER_MASK`
    do
        set_one_mask "/proc/irq/${irqs[$i]}/smp_affinity" $mask
        i=$(( i + 1 ))
    done
}

restart_irqbalance()
{
    local iface=$1
    local config_file="/etc/default/irqbalance"
    local options_key="OPTIONS"
    local systemd=""

    # return early if irqbalance is not running
    ! ps -elf | grep irqbalance | grep -v grep &>/dev/null && return

    if ! test -f $config_file; then
        if test -f /etc/sysconfig/irqbalance; then
            config_file="/etc/sysconfig/irqbalance"
            options_key="IRQBALANCE_ARGS"
            systemd="yes"
        else
            echo "Unknown system configuration - not restarting irqbalance!"
            echo "You have to prevent it from moving $iface IRQs manually!"
            return
        fi
    fi

    local orig_file="$config_file.scylla.orig"

    # Save the original file
    ! test -f $orig_file && cp $config_file $orig_file

    # Remove options parameter if exists
    local tmp_file=`mktemp`
    egrep -v -w ^"\s*$options_key" $config_file > $tmp_file
    mv $tmp_file $config_file

    echo -n "Restarting irqbalance: going to ban the following IRQ numbers: "

    local new_options="$options_key=\""
    local irq
    for irq in `get_irqs $iface`
    do
        new_options="$new_options --banirq=$irq"
        echo -n "$irq "
    done

    echo "..."
    echo "Original irqbalance configuration is in $orig_file"

    new_options="$new_options\""
    echo $new_options >> $config_file

    if [[ -z "$systemd" ]]; then
        /etc/init.d/irqbalance restart
    else
        systemctl try-restart irqbalance
    fi
}

usage()
{
    cat $0 | grep ^"# !" | cut -d"!" -f2-
}

parse_args()
{
    local i
    local arg

    until [ -z "$1" ]
    do
        arg=$1
        case "$arg" in
            "-mq")
                MQ_MODE="mq"
                ;;
            "-sq")
                MQ_MODE="sq"
                ;;
            "--cpu-mask")
                CPU_MASK=1
                ;;
            "--use-cpu-mask")
                CPU_FILTER_MASK=$2
                shift
                ;;
            "-h"|"--help")
                usage
                exit 0
                ;;
            *)
                IFACE=$arg
                ;;
            esac
            shift
    done
}

#
# Prints the default MQ mode for a given networking interface
#
get_def_mq_mode()
{
    local iface=$1
    local num_irqs=`get_irqs $iface | wc -l`
    local rx_queues_count=`get_rps_cpus $iface | wc -l`
    local num_cores=`hwloc-calc --number-of core machine:0 --restrict $CPU_FILTER_MASK`

    # If RPS is not enabled, use number of IRQs as an estimate for the Rx queues number.
    [[ "$rx_queues_count" -eq "0" ]] && rx_queues_count=$num_irqs

    if (( num_cores > 4 * rx_queues_count )); then
        echo "sq"
    else
        echo "mq"
    fi
}

#
# setup_one_hw_iface <iface> <mq_mode>
#
# configure a single HW interface
#
setup_one_hw_iface()
{
    local iface=$1
    local mq_mode=$2
    local irq

    [[ -z $mq_mode ]] && mq_mode=`get_def_mq_mode $iface`

    # bind all NIC IRQs to CPU0
    if [[ "$mq_mode" == "sq" ]]; then
        for irq in `get_irqs_one $iface`
        do
            echo "Binding IRQ $irq to CPU0"
            echo 1 > /proc/irq/$irq/smp_affinity
        done

        # Setup RPS
        setup_rps $iface --no-cpu0
    else # "$mq_mode == "mq"
        distribute_irqs $iface
        setup_rps $iface
    fi

    # Setup XPS
    setup_xps $iface

}

setup_bonding_iface()
{
    local bond_iface=$1
    local mq_mode=$2
    local iface

    for iface in `cat /sys/class/net/$bond_iface/bonding/slaves`
    do
        if dev_is_hw_iface $iface; then
            echo "Setting up $iface..."
            setup_one_hw_iface $iface $mq_mode
        else
            echo "Skipping $iface (not a physical slave device?)"
        fi
    done
}

gen_cpumask_one_hw_iface()
{
    local iface=$1
    local mq_mode=$2

    [[ -z $mq_mode ]] && mq_mode=`get_def_mq_mode $iface`

    # bind all NIC IRQs to CPU0
    if [[ "$mq_mode" == "sq" ]]; then
        hwloc-calc $CPU_FILTER_MASK ~core:0
    else # "$mq_mode == "mq"
        hwloc-calc $CPU_FILTER_MASK
    fi
}

gen_cpumask_bonding_iface()
{
    local bond_iface=$1
    local mq_mode=$2
    local iface
    local found_mq=

    for iface in `cat /sys/class/net/$bond_iface/bonding/slaves`
    do
        if dev_is_hw_iface $iface; then
            [[ -z $mq_mode ]] && mq_mode=`get_def_mq_mode $iface`
            if [[ "$mq_mode" == "mq" ]]; then
                found_mq=1
            fi
        fi
    done
    if found_mq; then
        hwloc-calc $CPU_FILTER_MASK
    else
        hwloc-calc $CPU_FILTER_MASK ~core:0
    fi 
}

IFACE="eth0"
MQ_MODE=""
CPU_FILTER_MASK=`hwloc-calc all`
CPU_MASK=

parse_args $@

# Don't let the CPU_FILTER_MASK have bits outside the CPU set of this machine
CPU_FILTER_MASK=`hwloc-calc --restrict $CPU_FILTER_MASK all`

if [[ $CPU_MASK ]]; then
    if dev_is_hw_iface $IFACE; then
        gen_cpumask_one_hw_iface $IFACE $MQ_MODE
    elif dev_is_bond_iface $IFACE; then
        gen_cpumask_bonding_iface $IFACE $MQ_MODE
    else
        echo "Not supported virtual device: $IFACE"
        exit 1
    fi
    exit 0
fi

# Currently we support of HW or bonding interfaces
if dev_is_hw_iface $IFACE; then
    echo "Setting a physical interface $IFACE..."
elif dev_is_bond_iface $IFACE; then
    echo "Setting $IFACE bonding interface..."
else
    echo "Not supported virtual device: $IFACE"
    exit 1
fi

# Ban irqbalance from moving NICs IRQs
restart_irqbalance $IFACE

if dev_is_hw_iface $IFACE; then
    # setup a HW NIC
    setup_one_hw_iface $IFACE $MQ_MODE
else # setup a bonding interface
    setup_bonding_iface $IFACE $MQ_MODE
fi

# Increase the socket listen() backlog
echo 4096 > /proc/sys/net/core/somaxconn

# Increase the maximum number of remembered connection requests, which are still
# did not receive an acknowledgment from connecting client.
echo 4096 > /proc/sys/net/ipv4/tcp_max_syn_backlog
