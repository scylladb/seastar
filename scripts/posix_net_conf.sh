#!/bin/bash

ec2_rps()
{
    local i=0
    local rps_cpus
    local mask
    local rps_queues_count=`ls -1 /sys/class/net/eth0/queues/*/rps_cpus | wc -l`

    for (( i = 1; i < rps_queues_count; i++ ))
    do
        rps_cpus="/sys/class/net/eth0/queues/rx-$i/rps_cpus"
        mask=`printf %x $((1 << i))`
        echo "Setting mask $mask in $rps_cpus"
        echo $mask > $rps_cpus
    done
}

en_rps()
{
    local cpu_num=`cat /proc/cpuinfo | grep processor | wc -l`
    local rps_queues_count=`ls -1 /sys/class/net/eth0/queues/*/rps_cpus | wc -l`

    local cpus_per_q=$((cpu_num / rps_queues_count))
    local mask=$(( (1 << cpus_per_q) - 1 ))
    local rps_cpus
    for (( i = 0; i < rps_queues_count; i++ ))
    do
        rps_cpus="/sys/class/net/eth0/queues/rx-$i/rps_cpus"
        mask_hex=`printf %x $(( mask & ~1 ))`
        echo "Setting mask $mask_hex in $rps_cpus"
        echo $mask_hex > $rps_cpus
        mask=$((mask << cpus_per_q))
    done
}

# bind all NIC IRQs to CPU0
for irq in `cat  /proc/interrupts | grep eth0 | cut -d":" -f1`
do
    echo "Binding IRQ $irq to CPU0"
    echo 1 > /proc/irq/$irq/smp_affinity
done

# Setup RPS

if ethtool -i eth0 | grep ixgbevf > /dev/null; then
    en_rps
else
    ec2_rps
fi

