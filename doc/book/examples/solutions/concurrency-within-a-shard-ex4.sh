#!/bin/bash
# Chapter 7, exercise 4: how long pubsub-bench takes to connect 1,000
# subscribers, with --max-connecting 1, 50 and 1000, to an idle Shoal and
# to a Shoal frozen for a third of a second.
SHOAL=${SHOAL:-shoal/v4/build/shoal}
BENCH=${BENCH:-build/pubsub-bench}
PORT=6471

overflows() {
    nstat -az TcpExtListenOverflows | awk 'NR > 1 {print $2}'
}

run() {  # --max-connecting, and whether to freeze Shoal (yes or no)
    $SHOAL -c1 -m1G --port $PORT > /dev/null 2>&1 &
    local shoal=$!
    until valkey-cli -p $PORT PING > /dev/null 2>&1; do sleep 0.1; done
    if [ $2 = yes ]; then kill -STOP $shoal; fi
    local before=$(overflows)
    local start=$(date +%s.%N)
    $BENCH -c1 -m1G --port $PORT --subscribers 1000 --messages 1 \
        --max-connecting $1 > /dev/null 2>&1 &
    local bench=$!
    local queue=""
    if [ $2 = yes ]; then
        sleep 0.33
        queue=$(ss -ltn "sport = :$PORT" |
                awk 'NR == 2 {print "Recv-Q " $2 ", Send-Q " $3}')
        kill -CONT $shoal
    fi
    wait $bench
    local secs=$(echo "$(date +%s.%N) - $start" | bc)
    echo "--max-connecting $1, frozen $2: $secs s," \
         "$(( $(overflows) - before )) overflows ${queue:+($queue)}"
    kill $shoal
    wait $shoal 2> /dev/null || true
}

for m in 1 50 1000; do run $m no; done
for m in 50 1000; do run $m yes; done
