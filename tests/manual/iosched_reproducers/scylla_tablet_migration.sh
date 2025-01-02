#!/usr/bin/env bash

# Test scenario:
# Simulation of a ScyllaDB workload which prompted some changes to the IO scheduler:
# database queries concurrent with tablet streaming.
# 
# All 7 shards are running a low-priority (200 shares) batch IO workload
# and a high-priority (1000 shares), moderate-bandwidth, interactive workload.
#
# The interactive workload requires about 30% of the node's
# total bandwidth (as measured in tokens), in small random reads.
# The batch workload does large sequential reads and wants to utilize all
# spare bandwidth.
#
# This workload is almost symmetric across shards, but is slightly skewed
# and shard 0 is slightly more loaded. But even on this shard, the workload
# doesn't need more than 35% of the fair bandwidth of this shard.
#
# Due to the distribution of shares across IO classes, the user expects that
# the interactive workload should be guaranteed (1000 / (1000 + 200)) == ~84% of 
# the disk bandwidth on each shard. So if it's only asking for less than 35%,
# the lower-priority job shouldn't disturb it.
#
# But before the relevant IO scheduler changes, this goal wasn't met,
# and the interactive workload on shard 0 was instead starved for IO
# by the low-priority workloads on other shards.

if [ $# -ne 1 ]; then
    echo "Usage: $0 IO_TESTER_EXECUTABLE" >&2
    exit 1
fi

"$1" --smp=7 --storage=/dev/null --conf=<(cat <<'EOF'
- name: tablet-streaming
  data_size: 1GB
  shards: all
  type: seqread
  shard_info:
    parallelism: 50
    reqsize: 128kB
    shares: 200
- name: cassandra-stress
  shards: all
  type: randread
  data_size: 1GB
  shard_info:
    parallelism: 100
    reqsize: 1536
    shares: 1000
    rps: 75
  options:
    pause_distribution: poisson
    sleep_type: steady
- name: cassandra-stress-slight-imbalance
  shards: [0]
  type: randread
  data_size: 1GB
  shard_info:
    parallelism: 100
    reqsize: 1536
    class: cassandra-stress
    rps: 10
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
