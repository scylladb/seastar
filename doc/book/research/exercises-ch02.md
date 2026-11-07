# Chapter 2 exercises, run on Nadav's laptop (2026-11-06)

Intel Core Ultra 7 258V (CPUs 0-3 fast, 4-7 slow), Fedora 44, GCC 16,
Seastar 6a752c502 built in release mode with C++23. The solutions were
written as a reader would, from the chapter's programs.

1. **Threads.** `hello` with a 30-second sleep, listed with `ps -L -p`:
   4 threads with `-c1` (the main thread, `syscall-0`, two `hello-ust`),
   6 with `-c2` (adds `reactor-1`, `syscall-1`), 18 with all 8 cores.
   Each shard has a reactor thread (shard 0 uses the main thread) and a
   syscall thread; the two `-ust` threads are LTTng's.
2. **Three counts at once** (tick 3x300 ms, tock 2x500 ms, tack 4x200 ms):
   1000 ms together, the same with `-c1` and `-c4`, since all three run on
   shard 0.
3. **Echo whole lines.** With `(printf hel; sleep 1; printf 'lo\n'; sleep 1)
   | nc localhost 1234`, the original server echoes "hel" at once and
   "lo\n" a second later; the solution sends "hello\n" after a second.
4. **Load balancing.** Connections one after another all go to shard 0 by
   default; held open at once, they spread two per shard. With
   `lba = port`, the shard is the client's port modulo 4 (ports 50000-50003
   went to shards 0-3, 50005 to 1, 50010 to 2). `nc -p` sets the port.
5. **Pipelining** (rewritten 2026-11-06 with the server-bound method):
   pong on CPU 0, valkey-benchmark on CPUs 1-3, 2 threads, 96 connections:
   -P 1: 340K requests/s, p50 0.271 ms; -P 4: 1.37M, 0.279 ms; -P 16:
   5.33M, 0.263 ms; -P 128: 13.3M, 0.447 ms. Little's law: 96 x P / p50
   predicts 354K, 1.38M and 5.84M. The earlier version of this exercise
   (how many client threads, how many server cores) ended at "the client is
   the limit", which Nadav found uninteresting. With the client on the
   slow CPUs 4-7, -P 128 also gave 13.3M. The chapter's table: Valkey
   4.70M PINGs/s, pong 13.3M, each pinned to CPU 0; pong on two cores also
   gave 13.3M, all the client could send.
