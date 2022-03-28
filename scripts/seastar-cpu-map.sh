#!/bin/bash
# !
# ! Usage: ./seastar-cpu-map.sh -p <process_PID> -n <process_Name> -s (optional) <shard>
# !
# ! List CPU affinity for a particular running process
# !  providing a map of threads -> shard, for any seastar apps.
# ! Ex.: ./seastar-cpu-map.sh -n scylla
# !      ./seastar-cpu-map.sh -n scylla -s 0
# !      ./seastar-cpu-map.sh -p 34
# !      ./seastar-cpu-map.sh -p 32 -s 12

usage() {
    cat $0 | grep ^"# !" | cut -d"!" -f2-
}

while getopts 'p:n:s:' option; do
  case "$option" in
    p) PID=$OPTARG
        ;;
    n) PID=`pgrep --newest --exact $OPTARG`
        ;;
    s) SHARD=$OPTARG
        ;;
    :) printf "missing argument for -%s\n" "$OPTARG" >&2
       usage >&2
       exit 1
       ;;
    \?) printf "illegal option: -%s\n" "$OPTARG" >&2
       usage >&2
       exit 1
       ;;
  esac
done

if [ $# -eq 0 ]; then usage >&2; exit 1; fi

if [ -e "/proc/$PID/task" ]; then
  # get list of threads for given PID
  THREADS=`ls /proc/$PID/task`
  for i in $THREADS; do
    # get shards from threads
    # there were three options here to get the shard number:
    # reactor-xx, syscall-xx and timer-xx
    # syscall was preferred because reactor as a special case (reactor-0 is called scylla)
    SYSCALL=`grep syscall /proc/$i/comm | cut -d"-" -f2`
    if [ -n "$SYSCALL" ] && [ "$SYSCALL" = "$SHARD" ]; then
      echo -e "shard: $SYSCALL, cpu:$(taskset -c -p $i | cut -d":" -f2)"
    elif [ -n "$SYSCALL" ] && [ -z "$SHARD" ]; then
      echo -e "shard: $SYSCALL, cpu:$(taskset -c -p $i | cut -d":" -f2)"
    fi
  done
else
  echo "Process does not exist"
fi
