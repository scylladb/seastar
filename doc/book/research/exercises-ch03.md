# Chapter 3 exercises, run on Nadav's laptop (2026-11-06)

Same machine and Seastar as chapter 2's exercises.

1. **Release and dev builds.** The library alone (99 files), without a
   compiler cache: release 88 s, dev 65 s (the memory limiter held the dev
   build back, so the true difference is probably larger). The default
   build compiles 148 more files (tests, apps, demos). `pong` built against
   each: 103 MB against release (static, with debug information), 181 KB
   against dev, because dev (and debug) build Seastar as a shared library
   (`configure.py` sets BUILD_SHARED_LIBS for those modes); the dev `pong`
   doesn't start until LD_LIBRARY_PATH names build/dev ("libseastar.so:
   cannot open shared object file"). Building `pong` took 2.9 s against
   dev, 3.5 s against release. Its speed couldn't be compared: one pong
   core already saturates the client (3.33M vs 3.63M requests/s), so the
   old exercise text about speed was replaced.
2. **Three months of commits.** 209 commits by 32 people;
   `git log -S'[[deprecated'` finds deprecations (`httpd: deprecate
   http_server_tester`, `rpc: Deprecate the per-verb scheduling group`,
   whose messages name the replacement) and removals of deprecated APIs
   (`coroutine: Remove deprecated exception helpers`). One month had no
   deprecation, so the exercise uses three months.
3. **--greeting.** Works on the command line and in SEASTAR_OPTIONS; with
   both, the command line wins. (An earlier draft wrongly hinted that
   SEASTAR_OPTIONS holds only Seastar's options.) Building it as a reader
   would revealed CMake warnings (C++ standard mismatch, policy CMP0167),
   fixed in shoal/v0/CMakeLists.txt.
4. **Log levels.** `shoal=trace` shows the same as `shoal=debug` (Shoal has
   no trace messages); `--default-log-level debug` adds 15 "seastar" and 3
   "io" lines, including "Assign CPU0 to NUMA0" lines: `-c2` chose CPUs 0
   and 4, `-c4` chose 0, 2, 4, 6, half of them slow cores on this hybrid
   machine. "CPU0 allocated 536870912 bytes" shows the memory per shard.
5. **Reactor backends.** `--reactor-backend` offers io_uring (default),
   asymmetric_io_uring, linux-aio, epoll. asymmetric_io_uring refuses to
   start without `--async-workers-cpuset` ("No CPUs specified for
   asymmetric_io_uring workers"), and starts with `--async-workers-cpuset 4`.
