# Chapter 5 measurements and exercises, run on Nadav's laptop (2026-11-06)

Intel Core Ultra 7 258V (CPUs 0-3 fast, 4-7 slow), Fedora 44, GCC 16,
Seastar 6a752c502 (release, C++23), Valkey 9.0.6, redis-py 5.2.1. Servers
pinned to CPU 0 (Shoal `--cpuset 0`, Valkey `taskset -c 0`), clients on
CPUs 1-3.

## The text's measurements

- **preemption.cc** (`-c1 -m1G`, one second of work, compute() about
  0.45 us): plain, without_preemption_check and block delay the heartbeat
  by the whole second; maybe_yield and coroutine by at most 0.6-0.8 ms
  (one task quota), with about 670 beats instead of 1000; yield by at
  most 0.15 ms. yield() cost about 2% of the computation (about 10 ns per
  yield); maybe_yield() nothing measurable. The stall detector reported
  plain and without_preemption_check (66, 124, 233, 442, 853 ms), but not
  block: both detectors (posix timer on CLOCK_THREAD_CPUTIME_ID, perf
  PERF_COUNT_SW_TASK_CLOCK) measure CPU time, and a thread blocked in
  sleep uses none.
- **Stall detector misses the first batch of tasks.** Without the 10 ms
  sleep before the work, preemption --work plain got no stall reports at
  all: cpu_stall_detector::start_task_run() records tasks_processed() as
  _last_tasks_processed_seen, and on_signal() treats 0 as "not active";
  in the reactor's first run_some_tasks() the count is 0, and the work
  started in that batch. A Seastar bug, small; worth an upstream fix.
- **KEYS and FLUSHALL** with about 950,000 keys (valkey-benchmark -t set
  -n 1000000 -r 10000000 -P 128 -c 16), PING latency of one client
  (valkey-benchmark -t ping_inline -c 1 -n 400000), max over the run,
  two runs each:
  - quiet: 1.2-3.4 ms on all servers (the laptop's noise);
  - KEYS 'nomatch*' every 100 ms: Valkey 20.5, 20.2 ms; Shoal without
    yielding 62, 66 ms (stall reports of 66-68 ms); Shoal v2 4.4, 6.2 ms;
  - FLUSHALL: Valkey SYNC 44.8, 45.4 ms (commandstats: 48 ms), ASYNC 3.4
    ms; Shoal with db.clear() 63 ms (stall report 68-69 ms, in
    _Hashtable::clear); Shoal v2 3.6 ms.
  - v2 during KEYS isn't stalling (no stall report even at
    --blocked-reactor-notify-ms 2), but about 0.1% of PINGs took 3-4 ms,
    against fewer than 0.003% above 0.5 ms when quiet: every step of a
    PING waits for the next poll, which comes once per task quota while
    KEYS has work. With --task-quota-ms 0.1, fewer than 6 of 400,000
    PINGs took over 1.5 ms.
- **Rehash stalls.** Loading the keys with --blocked-reactor-notify-ms 2
  reported stalls of 2, 6, 7, 11, 16 and 33 ms, all in SET:
  insert_or_assign -> _M_insert_unique_node -> _M_rehash. Another run
  (default threshold) reported one of 67 ms in the same place.
- **A one-off 88 ms stall** in the first KEYS of a fresh Shoal, inside
  seastar::memory::small_pool::add_more_objects() (from deque::push_back
  copying a key). It didn't repeat in three more runs. Probably the
  kernel populating fresh memory (THP enabled=madvise, defrag=madvise, so
  page faults in Seastar's madvised memory may compact memory directly).
  Not mentioned in the chapter; consider for chapter 21 or 29.
- **KEYS test:** test_keys_while_changing made KEYS suspend about 2,200
  times per run (about 60 KEYS calls), and restart because of a rehash in
  6 of 8 runs. The naive version (iterator kept across maybe_yield) failed
  it in 2 of 3 runs (duplicates, missing keys), without crashing.
- **Glob matching** agrees with Valkey on 60,000 random patterns over
  random keys of 1-6 characters from "abc-]^\[*?x". Valkey differs for
  the empty key: only the pattern "*" matches it (KEYS special-cases "*";
  its matcher never matches an empty string otherwise). Shoal matches ""
  with any pattern of only stars; not worth copying.
- **Reactor backends** (Shoal v2, -t set,get -n 40000000 -P 128 -c 96
  --threads 2, client on CPUs 1-3), two runs: epoll 3.40M/4.00M and
  3.40M/3.99M; linux-aio 3.47M/4.10M and 3.40M/3.99M; io_uring
  3.55M/4.21M and 3.47M/4.20M. asymmetric_io_uring with
  --async-workers-cpuset 1, client on CPUs 2-3: 3.80M/4.57M and
  3.81M/4.56M; io_uring with the client on CPUs 2-3: 3.55M/4.21M and
  3.55M/4.20M.

## Exercises

1. **Naive KEYS** (iterator kept across maybe_yield): test_keys_while_changing
   failed in 2 of 3 runs, with duplicate keys (for example 1,927 results
   for 1,000 distinct keys) or missing ones, and no crash in a release
   build. Duplicates can only come from a rehash.
2. **Task quota** (preemption --work maybe_yield): --task-quota-ms 0.1:
   910 heartbeats, the latest 0.28 ms late; default 0.5: 668, 0.51 ms;
   --task-quota-ms 5: 201, 4.86 ms. Each beat waits for about one quota.
   Cost on a busy server: Shoal v2 with --task-quota-ms 0.05 did 3.26M
   SET/s and 3.90M GET/s, against 3.48M and 4.10M with the default (about
   6% less).
3. **Idle polling** (Shoal v2 --cpuset 0): idle, 2 ticks in 5 s (0.4%).
   `valkey-cli -r 5000 -i 0.001 PING`: 113 ticks in 5.75 s (about 20% of a
   core, about 220 us per PING) by default; 13 ticks with
   --idle-poll-time-us 0 (about 25 us per PING). The rest is the 200 us of
   polling after each request. A Python client sending a PING every 1 ms
   saw a median round trip of 46 us with polling and 57 us without; at one
   PING every 10 ms, 124 and 121 us (the client's own wakeups dominate).
   Shoal's CPU time during the backend benchmark: 93% user, 7% system.
4. **Growth stalls** (-m4G --blocked-reactor-notify-ms 2, about 3.8M keys):
   rehash stalls reported up to 37, 78 and 150 ms (each stall reported
   several times at doubling intervals), doubling with the table. Also
   2-20 ms stalls in small_pool::add_more_objects (fresh memory).
5. **FLUSHALL ASYNC in the background**: a captureless lambda coroutine
   taking the store by value, launched with (void). All 20 tests pass;
   with 950,000 keys FLUSHALL SYNC took 0.07 s and ASYNC 0.00 s, and the
   background coroutine finished. Things to be careful about: the data
   must be moved into the coroutine (a reference to the local would
   dangle), a lambda with captures would dangle too (lambda coroutine
   fiasco), and nothing waits for the background work at shutdown.

Also verified: the non-yielding KEYS and db.clear() FLUSHALL pass all 20
tests ("it works, and the tests pass").
