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
# Bind RPS queues to CPUs other than CPU0
#
# If there is a single CPU in the system - don't enable RPS
#
# If there is more CPUs than (RPS queues plus 1) then every queue will get 
# (NCPUS - 1)/NQUEUES CPUs and (NCPUS - 1) % NQUEUES lower queues will get one 
# additional CPU.
# For instance, if NCPUs=15 and NQUEUES=3, then configuration will be as follows:
# Q0: {1,2,3,4,5}, Q1: {6,7,8,9,10} Q2: {11,12,13,14}
#
# If there is less CPUs than (RPS queues plus 1) when each queue will get a single CPU and 
# CPUs will be used in a round robin way.
# For instance, if NCPUs=3 and NQUEUES=5, then configuration will be as follows:
# Q0: {1}, Q1: {2} Q2: {1}, Q3: {2}, Q4: {1}
#
# If CPUs number equals to number of RPS queues plus 1 then each queue will get 
# a separate CPU.
#
setup_rps()
{
    # If we are in a single CPU environment - there is no point in configuring RPS
    [[ $CPU_NUM -eq 1 ]] && return

    # CPU0 will not be used since we are going to bind HW IRQs to it
    local cpu_num=$(( CPU_NUM - 1 ))

    local rps_queues_count=`ls -1 /sys/class/net/$IFACE/queues/*/rps_cpus | wc -l`
    local cpus_per_q=$((cpu_num / rps_queues_count))

    # 
    # If for some strange reason HW driver enabled more HW queues than a number 
    # of CPUs bind each HW queue to a single CPU. 
    # We will have more than a single queue per CPU in this case.
    #
    [[ $cpus_per_q -eq 0 ]] && cpus_per_q=1

    declare -a local cpus_per_q_ar
    local i

    # Fill the array with "cpus_per_q" values first
    for (( i = 0; i < rps_queues_count; i++ ))
    do
        cpus_per_q_ar[i]=$cpus_per_q
    done

    # Spread the "cpu_num - cpus_per_q * rps_queues_count" among queues 0, 1, 2,...
    local left_cpus=$(( cpu_num - cpus_per_q * rps_queues_count ))

    if [[ $left_cpus -gt 0 ]]; then
        for (( i = 0; i < rps_queues_count; i++ ))
        do
            cpus_per_q_ar[i]=$(( cpus_per_q_ar[i] + 1))
            left_cpus=$(( left_cpus - 1 ))
            [[ $left_cpus -eq 0 ]] && break
        done
    fi

    local mask
    local cur_mask_shift=0
    for (( i = 0; i < rps_queues_count; i++ ))
    do
        # Build a mask for a current queue, skip CPU0
        mask=$(( ((1 << cpus_per_q_ar[i]) - 1) << (1 + cur_mask_shift) ))

        cur_mask_shift=$(( cur_mask_shift + cpus_per_q_ar[i] ))
        if [[ $cur_mask_shift -ge $cpu_num ]]; then
            cur_mask_shift=$(( cur_mask_shift - cpu_num ))
        fi

        set_one_mask $i $mask
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

CPU_NUM=`cat /proc/cpuinfo | grep processor | wc -l`
CPUS_MASK=$(( (1 << CPU_NUM) - 1 ))
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

