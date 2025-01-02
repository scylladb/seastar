#!/usr/bin/env bash

# There is a `tau` mechanism in `fair_queue` which lets newly-activated
# IO classes to monopolize the shard's IO queue for a while.
#
# This isn't very useful and can result in major performance problems,
# as this test illustrates. The `highprio` workload could have tail latency
# of about 2 milliseconds, but the `bursty_lowprio` is allowed by `tau` to butt in
# periodically and preempt `highprio` for ~30ms, bringing its tail latency
# to that threshold.

if [ $# -ne 1 ]; then
    echo "Usage: $0 IO_TESTER_EXECUTABLE" >&2
    exit 1
fi

"$1" --smp=7 --storage=/dev/null --conf=<(cat <<'EOF'
- name: filler
  data_size: 1GB
  shards: all
  type: seqread
  shard_info:
    parallelism: 10
    reqsize: 128kB
    shares: 10
- name: bursty_lowprio
  data_size: 1GB
  shards: all
  type: seqread
  shard_info:
    parallelism: 1
    reqsize: 128kB
    shares: 100
    batch: 50
    rps: 8
- name: highprio
  shards: all
  type: randread
  data_size: 1GB
  shard_info:
    parallelism: 100
    reqsize: 1536
    shares: 1000
    rps: 50
  options:
    pause_distribution: poisson
    sleep_type: steady
EOF
) --io-properties-file=<(cat <<'EOF'
# i4i.2xlarge
disks:
- mountpoint: /dev
  read_bandwidth: 1542559872
  read_iops: 218786
  write_bandwidth: 1130867072
  write_iops: 121499
EOF
)
