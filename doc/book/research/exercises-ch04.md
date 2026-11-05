# Chapter 4 exercises, run on Nadav's laptop (2026-11-06)

Intel Core Ultra 7 258V (CPUs 0-3 fast, 4-7 slow), Fedora 44, GCC 16,
Seastar 6a752c502 (release, C++23), Valkey 9.0.6, redis-py 5.2.1.

1. **INCR.** Implemented in a copy of Shoal v1, with a test. redis-py's
   `incr()` sends `INCRBY key 1`, so a test using it fails against Shoal
   ("unknown command 'INCRBY'"); the exercise now says to use
   `execute_command('INCR', key)` (or implement INCRBY). With that, 16
   tests pass against both Shoal and valkey-server.
2. **APPEND.** One byte appended 100,000 and 1,000,000 times, in pipelines
   of 1000: Valkey 0.27 s and 2.68 s (linear); Shoal 0.31 s and 9.88 s
   (quadratic: each append copies the whole sstring, while Redis strings
   reserve room to grow). Found on the way: without TCP_NODELAY, Shoal took
   4.72 s for 100,000 (about 47 ms per pipeline, Nagle's algorithm against
   the client's delayed ACKs), so Shoal v1 now calls set_nodelay(true).
3. **Flushing.** The original exercise (remove a has_buffered_data() flush
   optimization) showed no difference, because Seastar's connected_socket
   output streams batch flushes (`batch_flushes = true` in
   connected_socket::output(); flush() only queues the stream for the
   reactor's flush poller). The optimization was removed from Shoal, and the
   exercise now counts sendmsg calls with strace: 160,001 for 160,000 SETs
   with -P 1, 10,077 with -P 16, although Shoal flushes after every reply.
   Throughput: 216K SET/s with -P 1, 1.6M with -P 16 (client-bound).
4. **Huge strings** (Shoal -c2 -m1G). Announcing 600 MB gets
   "-ERR Protocol error: invalid bulk length" (over the 512 MB limit).
   Announcing 500 MB makes Shoal allocate a 500 MB string before any data
   arrives; with 512 MB per shard it fails with std::bad_alloc, which Shoal
   logs ("connection with ... failed: std::bad_alloc") before dropping that
   connection; PING still works afterwards.
5. **Coroutine frames.** `errors.cc` with a plain function returning
   make_ready_future / make_exception_future: success 4 ns (coroutine:
   10 ns), failure checked with as_future 40 ns (coroutine: 48 ns).
   The chapter's table: success 10 ns, throw+catch 1,120 ns,
   return_exception+catch 770 ns, return_exception+as_future 48 ns.
6. **One huge pipeline.** A million APPENDs in a single redis-py pipeline:
   Valkey finishes in 3.06 s (it keeps reading and buffers the replies);
   Shoal deadlocks: `ss -tn` showed 4.1 MB queued from the client and
   2.6 MB from Shoal, both blocked, the client asleep. Shoal stops reading
   while out.write() waits, the client doesn't read until it has sent
   everything. (A 100,000-request pipeline fits in the buffers and works.)

Also measured for the chapter: Shoal v1 built without optimization (no
CMAKE_BUILD_TYPE) did 258K SET/s against Valkey's 1.78M; with
RelWithDebInfo, 1.78M SET/s and 2.0M GET/s, the same as Valkey (both
client-bound). Shoal's CMakeLists.txt now defaults to RelWithDebInfo.
