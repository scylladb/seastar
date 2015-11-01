#!/bin/bash
# !
# !  Usage: posix_net_conf.sh [iface name, eth0 by default]
# !
# !  Sets all IRQs of a given NIC to CPU0 and configures RPS to spreads NAPIs'
# !  handling between other CPUs in the way that NAPIs are distributed between 
# !  CPUs as equally as possible.
# !

ec2_rps()
{
    local i=0
    local rps_cpus
    local mask
    local rps_queues_count=`ls -1 /sys/class/net/$IFACE/queues/*/rps_cpus | wc -l`

    for (( i = 1; i < rps_queues_count; i++ ))
    do
        rps_cpus="/sys/class/net/$IFACE/queues/rx-$i/rps_cpus"
        mask=`printf %x $((1 << i))`
        echo "Setting mask $mask in $rps_cpus"
        echo $mask > $rps_cpus
    done
}

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

en_rps()
{
    local cpu_num=`cat /proc/cpuinfo | grep processor | wc -l`
    local rps_queues_count=`ls -1 /sys/class/net/$IFACE/queues/*/rps_cpus | wc -l`

    local cpus_per_q=$((cpu_num / rps_queues_count))
    local mask=$(( (1 << cpus_per_q) - 1 ))
    local rps_cpus
    for (( i = 0; i < rps_queues_count; i++ ))
    do
        rps_cpus="/sys/class/net/$IFACE/queues/rx-$i/rps_cpus"
        mask_hex=`make_hex_mask $(( mask & ~1 ))`
        echo "Setting mask $mask_hex in $rps_cpus"
        echo $mask_hex > $rps_cpus
        mask=$((mask << cpus_per_q))
    done
}

if [[ $# -eq 0 ]]; then 
    IFACE="eth0"
else
    IFACE=$1
fi


# bind all NIC IRQs to CPU0
for irq in `cat  /proc/interrupts | grep $IFACE | cut -d":" -f1`
do
    echo "Binding IRQ $irq to CPU0"
    echo 1 > /proc/irq/$irq/smp_affinity
done

# Setup RPS

if ethtool -i $IFACE | grep ixgbevf > /dev/null; then
    en_rps
else
    ec2_rps
fi

