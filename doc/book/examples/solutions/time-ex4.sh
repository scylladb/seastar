#!/bin/bash
# Chapter 9, exercise 4: loads about a million keys with EX 5 into a Shoal
# with --round-expiry, and shows its log, which tells how long its timer
# functions ran, when the keys expired. Usage: time-ex4.sh SHOAL PORT
SHOAL=$1
PORT=$2
log=$(mktemp)
$SHOAL --cpuset 0 -m2G --port $PORT --round-expiry > $log 2>&1 &
shoal=$!
until valkey-cli -p $PORT PING > /dev/null 2>&1; do sleep 0.1; done
start=$(date +%s.%N)
taskset -c 1-3 valkey-benchmark -p $PORT -n 1000000 -r 100000000 -P 64 \
    -q SET key:__rand_int__ x EX 5 > /dev/null 2>&1
loaded=$(echo "$(date +%s.%N) - $start" | bc)
keys() { valkey-cli -p $PORT KEYS 'key:*' | grep -c key: ; }
echo "loaded $(keys) keys in $loaded s"
sleep 12
echo "left: $(keys) keys"
kill $shoal
wait $shoal
grep -E 'timer functions|calls deleted' $log | sed 's/^.*\] //'
rm -f $log
