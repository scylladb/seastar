#!/bin/bash
# !
# !  Usage: posix_net_conf.sh [iface name, eth0 by default]
# !
# !  Sets all IRQs of a given NIC to CPU0 and configures RPS to spreads NAPIs'
# !  handling between other CPUs in the way that NAPIs are distributed between 
# !  CPUs as equally as possible.
# !

make_hex_mask()
{
    local val=$1
    local mask32=$(( (1 << 32) - 1))
    local res=`printf %x $(( val & mask32 ))`
    val=$(( val >> 32 ))

    while [[ $val -gt 0 ]]
    do
        res=`printf %x $(( val & mask32 ))`",$res"
        val=$(( val >> 32 ))
    done

    echo -n $res
}

#
#  set_one_mask <queue index> <CPU mask>
#
set_one_mask()
{
    local i=$1
    local mask=$2
    local rps_cpus="/sys/class/net/$IFACE/queues/rx-$i/rps_cpus"
    local mask_hex=`make_hex_mask $mask`
    echo "Setting mask $mask_hex in $rps_cpus"
    echo $mask_hex > $rps_cpus
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
        set_one_mask $i $mask
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
    for irq in `cat  /proc/interrupts | grep $IFACE | cut -d":" -f1`
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

if [[ $# -eq 0 ]]; then 
    IFACE="eth0"
else
    IFACE=$1
fi

# Ban irqbalance from moving NICs IRQs
restart_irqbalance

# bind all NIC IRQs to CPU0
for irq in `cat  /proc/interrupts | grep $IFACE | cut -d":" -f1`
do
    echo "Binding IRQ $irq to CPU0"
    echo 1 > /proc/irq/$irq/smp_affinity
done

# Setup RPS
setup_rps

