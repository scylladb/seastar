#!/usr/bin/env bash

# Test scenario:
# A single CPU-starved shard has a batch IO job.
# Goal: it should be able to utilize the entire bandwidth of the disk,
# despite the rare polls.

if [ $# -ne 1 ]; then
    echo "Usage: $0 IO_TESTER_EXECUTABLE" >&2
    exit 1
fi

"$1" --smp=7 --storage=/dev/null --conf=<(cat <<'EOF'
- name: tablet-streaming
  data_size: 1GB
  shards: [0]
  type: seqread
  shard_info:
    parallelism: 50
    reqsize: 128kB
    shares: 200
- name: cpuhog
  type: cpu
  shards: [0]
  shard_info:
    parallelism: 1
    execution_time: 550us

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
