#!/bin/bash
# !
# !  Usage: posix_net_conf.sh [iface name, eth0 by default] [-mq] [-h|--help]
# !
# !  Ban NIC IRQs from being moved by irqbalance.
# !
# !  If -mq is not given - set all IRQs of a given NIC to CPU0 and configure RPS
# !  to spreads NAPIs' handling between other CPUs.
# !
# !  If "-mq" is given - distribute NIC's IRQs among all CPUs instead of binding
# !  them all to CPU0 and do not enable RPS.
# !
# !  Enable XPS, increase the default values of somaxconn and tcp_max_syn_backlog.
# !
# !  -h|--help - print this help information
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
# Bind RPS queues to CPUs other than CPU0 and its hyper-threading siblings
#
# Use hwloc-distrib for generating the appropriate CPU masks.
#
setup_rps()
{
    # If we are in a single core environment - there is no point in configuring RPS
    [[ `hwloc-calc core:0.pu:all` -eq `hwloc-calc all` ]] && return

    local rps_queues_count=`ls -1 /sys/class/net/$IFACE/queues/*/rps_cpus | wc -l`
    local mask
    local i=0

    # Distribute all cores except for CPU0 siblings
    for mask in `hwloc-distrib --restrict $(hwloc-calc all ~core:0) $rps_queues_count`
    do
        set_one_mask "/sys/class/net/$IFACE/queues/rx-$i/rps_cpus" $mask
        i=$(( i + 1 ))
    done
}

#
# Spread all XPS queues to over the full cpuset. Don't bother to exclude CPU0
# (and friends) - scylla will just not send from it if its cpuset is properly set.
#
setup_xps()
{
    local xps_queues_count=`ls -1 /sys/class/net/$IFACE/queues/*/xps_cpus | wc -l`
    local mask
    local i=0

    for mask in `hwloc-distrib $xps_queues_count`
    do
        set_one_mask "/sys/class/net/$IFACE/queues/tx-$i/xps_cpus" $mask
        i=$(( i + 1 ))
    done
}

#
# Prints IRQ numbers for the $IFACE
#
get_irqs()
{
    if [[ `ls -1 /sys/class/net/$IFACE/device/msi_irqs/ | wc -l` -gt 0 ]]; then
        # Device uses MSI IRQs
        ls -1 /sys/class/net/$IFACE/device/msi_irqs/
    else
        # Device uses INT#x
        cat /sys/class/net/$IFACE/device/irq
    fi
}

distribute_irqs()
{
    local irqs=( `get_irqs` )
    local mask
    local i=0

    for mask in `hwloc-distrib ${#irqs[*]}`
    do
        set_one_mask "/proc/irq/${irqs[$i]}/smp_affinity" $mask
        i=$(( i + 1 ))
    done
}

restart_irqbalance()
{
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
            echo "You have to prevent it from moving $IFACE IRQs manually!"
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
    for irq in `get_irqs`
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
    if [[ $# -gt 2 ]]; then
        usage
        exit 1
    fi

    for arg in $@
    do
        case "$arg" in
            "-mq")
                MQ_MODE="yes"
                ;;
            "-h"|"--help")
                usage
                exit 0
                ;;
            *)
                IFACE=$arg
                ;;
            esac
    done
}

IFACE="eth0"
MQ_MODE=""

parse_args $@

# Ban irqbalance from moving NICs IRQs
restart_irqbalance

# bind all NIC IRQs to CPU0
if [[ -z "$MQ_MODE" ]]; then
    for irq in `get_irqs`
    do
        echo "Binding IRQ $irq to CPU0"
        echo 1 > /proc/irq/$irq/smp_affinity
    done

    # Setup RPS
    setup_rps
else
    distribute_irqs
fi

# Setup XPS
setup_xps

# Increase the socket listen() backlog
echo 4096 > /proc/sys/net/core/somaxconn

# Increase the maximum number of remembered connection requests, which are still
# did not receive an acknowledgment from connecting client.
echo 4096 > /proc/sys/net/ipv4/tcp_max_syn_backlog
