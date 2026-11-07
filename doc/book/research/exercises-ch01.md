# Chapter 1 exercises, run on Nadav's laptop (2026-11-05)

Machine: Intel Core Ultra 7 258V (Lunar Lake), 8 cores, 1 socket, 1 NUMA
node, no SMT. CPUs 0-3 are performance cores (up to 4.7-4.8 GHz, private
L2, shared L3); CPUs 4-7 are low-power efficiency cores (3.7 GHz, one
shared L2, no L3 listed by `lscpu -e`).

## Exercise 2: shared and per-thread counters

`count.cc`, `g++ -O2`, 100 million increments per thread, `std::atomic<long>`:

| Variant | 1 thread | 8 threads |
|---|---|---|
| one shared counter | 0.42 s (4.2 ns each) | 15.80 s (158 ns each) |
| a counter per thread, one per cache line | 0.44 s | 0.55 s |
| a counter per thread, adjacent (false sharing) | 0.43 s | 17.86 s |
| plain `long` per thread | 0.00 s (loop optimized away) | |

Contention grows with the number of threads: a shared counter costs 24-38 ns
per increment with 2 threads, 65-73 ns with 4, 158 ns with 8. Performance
vs. efficiency cores made little difference (2 threads: P+P 25.7 ns, E+E
38.3 ns, P+E 23.9 ns).

## Exercise 3: Valkey 9.0.6 under valkey-benchmark

Server: `valkey-server --port 16379 --save '' --appendonly no` (io-threads 1).

- `-t set,get -n 1000000 -P 16`: SET 1.91M/s, GET 2.56M/s. Finishes in about
  a second, too fast to watch in `top`.
- `-t get -n 20000000 -P 16`: 2.50M/s; server's one thread 84% of a core,
  client 51%.
- Same with `--threads 4`: 2.18M/s, server thread 80%: the client is not the
  limit.
- `-P 64 -c 200 --threads 4`: 3.47M/s, server thread 92%, p50 3.3 ms. Pinning
  the server to one P-core: same 3.47M/s.
- In all runs only the server's main thread was busy; its other threads
  (bio_*) were idle.

## Choosing the exercise 3 command

- `-l` (loop) is misleading: each round is only 100,000 requests (the default
  `-n`), reconnecting between rounds dominates, and it reported 0.40M GET/s.
- The exercise uses `valkey-benchmark -t get -P 16 --threads 4 -n 20000000 -q`
  instead: 7.8 s on this laptop, 2.58M GET/s, p50 0.27 ms.
- Nadav: 7.8 s is too short to catch in `top` (it refreshes every few
  seconds). Changed to `-n 100000000` (about 40 s at 2.5M GET/s), with a note
  to increase it if it finishes sooner.
