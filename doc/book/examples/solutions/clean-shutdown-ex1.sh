#!/bin/bash
# Chapter 12, exercise 1: starts Shoal versions 6 and 7 in the background,
# with &, sends each kill -INT a second later, and tells whether it
# stopped. SHOAL6 and SHOAL7 name the two programs.
for shoal in "$SHOAL6" "$SHOAL7"; do
    $shoal -c1 -m512M --overprovisioned --port 6597 > /dev/null 2>&1 &
    pid=$!
    sleep 1
    grep SigIgn /proc/$pid/status
    kill -INT $pid
    sleep 1
    if kill -0 $pid 2>/dev/null; then
        echo "$shoal: still running after SIGINT"
        kill -KILL $pid
        wait $pid 2>/dev/null
    else
        wait $pid
        echo "$shoal: stopped, with exit status $?"
    fi
done
