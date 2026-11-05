# Chapter 6 measurements and exercises, run on Nadav's laptop (2026-11-07)

Intel Core Ultra 7 258V (CPUs 0-3 fast, 4-7 slow), Fedora 44, GCC 16,
Clang 22, Seastar 6a752c502 (release, C++23), Valkey 9.0.6, redis-py
5.2.1. Servers pinned to CPU 0 (`--cpuset 0`), clients on CPUs 1-3.

## The text's measurements

All timings below were taken on a quiet machine, 2026-11-07 11:57-12:25,
by scripts that wait until no other process uses 15% or more of a core
(gnome-shell excepted) and log what else ran (`~/tmp/tmp-ch6/quiet-bench.sh`,
`quiet-cost.sh`). Earlier runs that day overlapped another session's
build and test loop: they were 20-30% slower in cost.cc, and showed a
Clang-built Shoal 15% slower than a GCC one, which was false.

- **cost.cc** (`-c1 -m512M --cpuset 0`, 100 million calls each; with 10
  million, the first row varied 0.4-1.2 ns, apparently because the core
  was still raising its clock), ns per call, three runs with GCC, two with
  Clang (the third waited for VS Code and was dropped):

  | | GCC 16 | Clang 22 |
  |---|---|---|
  | ordinary function | 0.4, 0.4, 0.4 | 0.4, 0.4 |
  | `make_ready_future()` | 2.0, 2.0, 2.0 | 1.6, 1.6 |
  | coroutine that doesn't wait | 8.0, 8.0, 8.0 | 3.8, 3.7 |
  | coroutine that waits once (`yield()`) | 25.0, 25.8, 25.2 | 26.9, 27.8 |

  A first version summed into an `int`, which overflowed (undefined
  behavior); fixed to `int64_t`. The machine code was checked: both
  compilers really call `plain()` every iteration (Clang unrolls by two),
  and call the others through the function pointer, with the preemption
  check, every iteration.
- **Shoal v2 against v3** (then called v1.1 and v1.2), `valkey-benchmark
  -t set,get -n 40000000 -P 128 -c 96 --threads 2 -q`, three rounds,
  alternating, SET/s and GET/s:
  - GCC v2: 3,633,391 / 3,552,714 / 3,547,672; 4,321,522 / 4,201,681 /
    4,314,995.
  - GCC v3: 3,996,004 / 3,894,081 / 3,899,396; 4,435,574 / 4,568,818 /
    4,569,340.
  - Clang v2: 3,547,672 / 3,547,987 / 3,633,391; 4,092,909 / 4,098,781 /
    4,099,201.
  - Clang v3: 3,551,767 / 3,633,061 / 3,547,357; 3,990,821 / 4,099,201 /
    4,099,621.
- **perf stat** (`instructions:u,cycles:u`, `-p` Shoal's pid, 20M SETs,
  same client), two runs: v2 75.3G and 75.5G instructions, 21.3G and
  21.7G cycles; v3 56.8G and 57.1G instructions, 19.1G and 19.1G cycles.
  Throughput under perf was lower (2.1-2.7M SET/s), but the counts are of
  Shoal's own user-space work.

## Exercises

1. `c.get()` on `answer_later()`'s unavailable future: the program aborts
   with "future.cc:249: void seastar::internal::future_base::do_wait():
   Assertion `thread' failed." `get()` can wait only by switching away
   from a Seastar thread's stack; a coroutine has none.
2. A loader whose first load throws: without the fix, three requests all
   fail with the same exception after 1 load (the failed shared_future is
   cached). Fix: `load_or_forget()` awaits `coroutine::as_future(load(key))`
   and erases the key if it failed, then returns the result; then the
   first request fails, the next two succeed, 2 loads. (Subtlety for a
   reader: if `load()` failed before suspending, the erase would run before
   `emplace()`; here `load()` always sleeps first.)
3. `event` with `shared_promise<> _p`: `wait()` returns
   `_p.get_shared_future()`, `signal()` does
   `std::exchange(_p, shared_promise<>()).set_value()`. Waiters 1 and 2
   woke on the first signal; waiter 3, which started after it, woke on the
   second.
4. `ignored.cc` with a 5 ms sleep: prints "dropped the future" and "done"
   only; `fail_later()` never resumes and no warning is logged; the
   program exits with the coroutine still suspended. Work left in the
   background at exit is silently abandoned.
5. With Clang (quiet-machine numbers above), v3's fast path gains about
   1% for SET and nothing for GET, against 10% and 6% with GCC: as cost.cc
   predicts, Clang's coroutine that doesn't wait costs only about 2 ns more
   than make_ready_future(), GCC's about 6 ns. `perf stat` agrees: cycles
   per SET 1,074 -> 954 with GCC, 1,064 -> 1,034 with Clang; v2 is the
   same with both compilers (about 3,770 instructions, 1,070 cycles per
   SET).

## Optimization levels and compiler flags (2026-11-07, 13:01-13:13, quiet)

- **cost.cc at -O0..-O3** (the program only; Seastar's library -O2 in all),
  four rounds each, ns per call (ordinary / ready / coroutine / waits):
  - GCC -O0: 1.1-1.2 / 41.2-42.5 / 84.7-87.2 / 162.9-166.1
  - GCC -O1: 0.3-0.4 / 2.2 / 8.0-8.1 / 26.3-27.5
  - GCC -O2: 0.4-0.5 / 1.9-2.0 / 7.9 / 24.7-25.4
  - GCC -O3: 0.4-0.5 / 2.0 / 4.6 / 25.6-26.3
  - Clang -O0: 0.8 / 38.0-38.7 / 82.6-83.4 / 179.2-182.6
  - Clang -O1: 0.4 / 1.7-1.8 / 3.7-3.8 / 27.8-29.7
  - Clang -O2: 0.3-0.4 / 1.5-1.6 / 3.3-3.7 / 26.7-27.2
  - Clang -O3: 0.3-0.4 / 1.6 / 3.3-3.7 / 26.5-28.0
  A PDF rebuild (mine) overlapped round 1 Clang -O2/-O3 and round 2 GCC
  -O0; those agree with the other rounds. Two runs overlapped Firefox and
  Evince (15-19% of a core), also in line.
- **Exercise 5 (new):** `perf record` of cost.cc at -O0 shows dozens of
  small functions each taking 1.5-3%: the awaiter's constructor,
  `future::~future`, `future_state_base::available()`, `any::move_it`,
  `any::failed()`, `any::has_result()`, `uninitialized_wrapper`'s
  constructor, `uninitialized_get()` and destructor, `detach_promise()`,
  `future_base::~future_base`. At -O2 none appear: the top functions are
  `reactor::add_task` (in the library, reactor.cc), the loop, the callees,
  `malloc`/`free_sized`. They were inlined.
- **Shoal v3 with Clang and ScyllaDB's flags** (Seastar's library as before,
  Clang -O2, no LTO/PGO), SET/s and GET/s:
  - Clang -O2: v2 3.55-3.63M, 4.09-4.10M; v3 3.55-3.63M, 4.00-4.09M.
  - All ScyllaDB compile flags (`-O3 -march=x86-64-v3 -mllvm
    -inline-threshold=2500 -fno-slp-vectorize`): v2 3.63-3.72M, 4.31M; v3
    3.90-4.00M, 4.44-4.70M (GCC -O2 v3 in the same session: 3.89-4.00M,
    4.56-4.70M).
  - Only `-mllvm -inline-threshold=2500` (with -O2): v3 3.89-4.00M,
    4.56-4.57M (three rounds).
  - Only `-O3 -march=x86-64-v3`: v3 3.63-3.71M, 4.20-4.21M (three rounds).
  So the inline threshold alone accounts for the gap between Clang and GCC.
- ScyllaDB's history (git log -S inline-threshold in scylla): bc65659a46
  (2018, Pawel Dziepak) GCC `inline-unit-growth=300`; 8068272b46 (2020,
  Avi) Clang threshold 2500: Clang lagged GCC 20% on perf_simple_query
  --smp 1 at the default, 3% at 2500; 5000 never finished compiling
  storage_proxy, 1250 lost 10%.
