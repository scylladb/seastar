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
# !   --options-file <YAML file> - YAML file with perftune.py options
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
                MQ_MODE="--mode mq"
                ;;
            "-sq")
                MQ_MODE="--mode sq"
                ;;
            "--cpu-mask")
                CPU_MASK="--get-cpu-mask"
                ;;
            "--use-cpu-mask")
                CPU_FILTER_MASK="--cpu-mask $2"
                shift
                ;;
            "--options-file")
                OPTIONS_FILE="--options-file $2"
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

IFACE="eth0"
MQ_MODE=""
CPU_FILTER_MASK=""
CPU_MASK=""
MY_DIR=`dirname $0`
OPTIONS_FILE=""

parse_args $@

$MY_DIR/perftune.py --nic $IFACE $MQ_MODE $CPU_FILTER_MASK $CPU_MASK $OPTIONS_FILE --tune net
