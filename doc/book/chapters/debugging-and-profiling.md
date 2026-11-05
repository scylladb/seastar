# Debugging and profiling {#ch:debugging-and-profiling}

The earlier chapters of this part found bugs with tests, with the
`debug` build ([chapter](#ch:testing)), and with LeakSanitizer and
repeat tests ([chapter](#ch:memory-leaks)). This chapter is about the
tools for a program that misbehaves anyway: reading the backtraces that
Seastar prints, looking inside a running program, or a dead one, with
gdb, teaching gdb about Seastar, and finding where the time goes, with
`perf` and flame graphs. Most of these tools aren't Seastar's own, but a
Seastar program uses them in its own way: its work is split among
shards, each a thread, and among tasks, which no ordinary backtrace
shows.

## Backtraces {index="backtrace"}

Seastar prints backtraces in several places that the earlier chapters
met: in a stall report ([chapter](#ch:stalls)), in an exception made by
`make_backtraced_exception_ptr()` ([chapter](#ch:errors)), in the
warning about an exceptional future that nobody looked at
([chapter](#ch:futures-and-promises)), and when the program crashes. All
are printed the same way, on one line. Here is a stall report of
[chapter](#ch:stalls)'s `gentle.cc`, which stalls on purpose, by
destroying a table of a million entries at once:

```
Reactor stalled for 66 ms on shard 0, in scheduling group main. Backtrace: 0x4e01fe
0x491530 0x491a5d 0x491d09 /lib64/libc.so.6+0x19faf 0x42bcc6 0x42c70d 0x49f966 ...
0x42aa84 (BuildId: 484cad2890ae53ffc170ff2fd2d0a7d84c1da728)
```

Each number is the address of a function call that was in progress,
innermost first: a plain address is in the program, and
`/lib64/libc.so.6+0x19faf` is an offset in a shared library. The
*BuildId* identifies the exact binary that printed it, since the same
addresses mean nothing in another build of the same program; `file`
prints a binary's BuildId, so a backtrace can be matched with the binary
that made it. Seastar prints addresses, which are cheap to collect, even
in a signal handler, rather than names, which the program would have to
look up in its debugging information.

[`seastar-addr2line`]{index="`seastar-addr2line`" .main}, in Seastar's
`scripts` directory, turns them into names. It reads text, a whole log
if need be, finds the backtraces in it, and decodes each with the
program given with `-e`:

```
$ seastar-addr2line -e gentle < stall.txt
Reactor stalled for 66 ms on shard 0, in scheduling group main. Backtrace:
[Backtrace #0]
void seastar::backtrace<...>(...) at include/seastar/util/backtrace.hh:86
seastar::backtrace_buffer::append_backtrace_oneline() at src/core/reactor.cc:861
...
seastar::internal::cpu_stall_detector::on_signal() at src/core/reactor.cc:1292
/lib64/libc.so.6: ELF 64-bit LSB shared object, x86-64, ...

?? at ??:0
std::__detail::_Hash_node<...>::_M_next() const at .../hashtable_policy.h:367
 (inlined by) std::__detail::_Hashtable_alloc<...>::_M_deallocate_nodes(...) at ...
 (inlined by) std::_Hashtable<...>::~_Hashtable() at .../hashtable.h:1902
 (inlined by) std::unordered_map<...>::~unordered_map() at .../unordered_map.h:112
 ...
 (inlined by) main::'lambda'()::operator()(...) (.actor) at gentle.cc:63
seastar::internal::coroutine_traits_base<void>::promise_type::run_and_dispose() ...
seastar::reactor::task_queue::run_tasks() at src/core/reactor.cc:2784
...
```

The first frames are the stall detector's own, which made the report;
then comes the signal handler, in the C library, which has no debugging
information here, so it's `?? at ??:0`; and then the code that was
running when the signal came: the destructor of the `unordered_map`,
called at line 63 of `gentle.cc`, which is `t.reset()`. Lines that say
"inlined by" are functions that the compiler inlined into the next one,
so they have no address of their own; `seastar-addr2line` finds them in
the debugging information. Without it, the decoding has only names, from
the symbol table: built without `-g`, the same program's frames were
`gentle.cc:0`, with no line numbers, and one of them `?? at ??:0`.
Seastar's `release` mode is built with debugging information
(`RelWithDebInfo`), so Seastar's own frames decode, and so does a
program built, as Shoal is, as `RelWithDebInfo` too. A shared library
needs its own debugging information, which distributions install
separately, such as Fedora's `-debuginfo` packages.

A backtrace shows how the code that ran got there, but in asynchronous
code that's only the reactor's loop and the current task: the code that
*waits* for the current task is not on the stack, but in memory, in the
chain of continuations. Seastar can record that too: built with
`configure.py --enable-task-backtrace`, which
[appendix](#app:building-seastar) listed, Seastar keeps, with each task,
a backtrace of where it was created, and its backtraces then show those
of the tasks waiting for the current one too. It slows every task down,
so it's for a build made to chase one bug.

## gdb {index="gdb"}

gdb works with Seastar programs, as with any other, but what it shows
needs some interpretation, and some of it needs help.

### Attaching to a running program {index="gdb!attaching"}

`gdb -p PID` attaches to a running process, and stops all its threads.
For a Seastar program, that stops every shard, as one long stall: clients
wait, and timers fire late, once gdb lets the program go on, with
`continue`, or with `detach`, which leaves it running without gdb. So
it's for a program that's already stuck, or for a test machine, but a
quick look at a production server, a few seconds long, is often worth
the pause. Linux lets a user attach to its own processes only if the
setting `kernel.yama.ptrace_scope` allows it: with 0, any of them; with 1,
the default of some distributions, only its children; with 2, only root;
and with 3, nobody. A program can also be started under gdb, as
`gdb --args shoal -c2`, which needs no permission, unless the setting
is 3.

The first thing to tell gdb is to ignore two signals. Seastar uses
signals for itself: `SIGRTMIN`, for the reactor's high-resolution
timers, and `SIGRTMIN + 1`, the stall detector's, which gdb shows as
`SIG34` and `SIG35`, and stops at every time one arrives:

```
handle SIG34 nostop noprint pass
handle SIG35 nostop noprint pass
```

These, and `set debuginfod enabled off`, which stops gdb from looking
for debugging information on the Internet, belong in a file of gdb
commands, which `gdb -x FILE` runs, or in `~/.gdbinit`.

### Threads {index="gdb!threads"}

A Seastar program has more threads than shards. Shoal, started with
`-c2`, waiting for clients:

```
(gdb) info threads
  Id   Target Id                                    Frame
* 1    Thread ... (LWP 2477290) "shoal"             ... in _io_uring_get_cqe ()
  2    Thread ... (LWP 2477298) "iou-wrk-2477290"   ... in ?? ()
  3    Thread ... (LWP 2477297) "syscall-1"         ... in __syscall_cancel_arch ()
  4    Thread ... (LWP 2477296) "syscall-0"         ... in __syscall_cancel_arch ()
  5    Thread ... (LWP 2477295) "reactor-1"         ... in _io_uring_get_cqe ()
  6    Thread ... (LWP 2477293) "shoal-ust"         ... in syscall ()
  7    Thread ... (LWP 2477292) "shoal-ust"         ... in syscall ()
```

Shard 0 runs on the program's main thread, which has the program's
name; the other shards each have a thread, `reactor-1` and up. Each
shard also has a thread, `syscall-0` and up, for system calls that may
block, and have no asynchronous version, such as those behind
`file_stat()`, `chmod()` and `file_system_space()`: the reactor hands
them to it, and waits for the answer as for any other. `iou-wrk-...` are the kernel's
workers for io_uring, with nothing for gdb to show; and `shoal-ust` are
LTTng's ([chapter](#ch:observability)). A shard that has nothing to do
sleeps in the reactor's backend, here io_uring's
`_io_uring_get_cqe()`, and its backtrace, with `thread apply all bt`,
shows the reactor's loop, `seastar::reactor::do_run()`, waiting for
events.

### Asynchronous code in gdb {index="gdb!coroutines"}

A breakpoint in a coroutine stops in it as in any function, but the
coroutine is several functions. A breakpoint on Shoal's
`pubsub::publish_here()` has three locations, among them the function
that the caller calls, which allocates the coroutine's frame and starts
it, and the coroutine's body, which GCC calls its *actor*, and which
runs again on every resume. The first stop is in the first:

```
(gdb) break pubsub::publish_here
Breakpoint 1 at 0x4ba8e0: pubsub::publish_here. (3 locations)
(gdb) continue
Thread 1 "shoal" hit Breakpoint 1.2, pubsub::publish_here (this=..., channel=...,
    data=...) at pubsub.cc:231
```

and the next one in the body, whose only argument is the frame:

```
Thread 1 "shoal" hit Breakpoint 1.1, pubsub::publish_here (frame_ptr=...)
    at pubsub.cc:231
(gdb) info locals
_Coro_resume_fn = 0x4ba8e0 <pubsub::publish_here()>
_Coro_promise = {<seastar::task> = {_vptr.task = ... <vtable for
    seastar::internal::coroutine_traits_base<unsigned long>::promise_type+16>, ...
_Coro_resume_index = 2
...
message = {u = {external = {str = 0x2 <error: Cannot access memory at address 0x2>,
cost = 0
subscribers = 511
```

The frame is a structure, which gdb shows as the body's local variables:
the function that resumes the coroutine, its promise, which is a
Seastar `task`, as [chapter](#ch:how-coroutines-work) explained, and
which suspension point it's at, and then the coroutine's own variables,
all of them, including those that the code hasn't reached yet, whose
values, such as `subscribers = 511`, are whatever the memory held. Once
the coroutine has suspended and resumed, its backtrace, with `bt`,
begins at the reactor, which resumed it as a task, and not at whoever
waits for its result: as with the backtraces above, that's in memory,
in the coroutine's promise, which points to the task waiting for it.

### Seastar's memory {index="gdb!memory"}

Every shard allocates from memory of its own, as [chapter](#ch:memory)
explained, and in a `release` or `dev` build, each shard's allocator
keeps its state in the thread-local variable `seastar::memory::cpu_mem`,
whose `memory` is where the shard's memory starts, and `nr_pages` how
many pages of 4 KB it has, and `nr_free_pages` how many of them are
free. Seastar places the shards' memory one after another, in one large
region, aligned so that a shard's number can be read from an address's
high bits: each shard's memory starts `1 << cpu_id_shift` bytes after
the previous one's, where Seastar computes `cpu_id_shift` from the
number of shards when it starts. In gdb, in Shoal on two shards:

```
(gdb) thread 1
(gdb) print 'seastar::memory::cpu_mem'.memory
$1 = 0x600000000000 ""
(gdb) print 'seastar::memory::cpu_id_shift'
$2 = 36
```

So an object's address tells which shard allocated it: shard
`(address - 0x600000000000) >> 36`, here, which is how Seastar itself
finds the shard that must free an object that another shard let go of.
That answers a common question when a program crashes on an object that
a shard used: is the object that shard's, or another's, which it
shouldn't have touched ([chapter](#ch:shared-nothing))?

## A gdb script of your own {index="gdb!scripts"}

gdb doesn't know Seastar's types, and shows them as they are laid out in
memory. A `sstring` is the clearest case: in the frame above, the
channel's name, "news", is somewhere in a union of two layouts, which
`print` shows both of:

```
channel = @0x...: {u = {external = {str = 0xa007377656e <error: Cannot
    access memory at address 0xa007377656e>, size = 0, pad = 0 '\000'},
    internal = {str = "news\000\n\000...", size = 4 '\004'}}}
```

gdb can be taught, with scripts in Python, which can print a type as it
should be read, and add commands. Seastar comes with none, but ScyllaDB
has a large one, `scylla-gdb.py`, in its source, with commands that
ScyllaDB's developers use every day to find their way in a core dump,
such as `scylla fiber`, which follows the chain of tasks that wait for
one another, and `scylla task_histogram`, which counts the objects in
memory by their type. Most of its commands are about ScyllaDB's own
data, some are about Seastar, and those depend on the version of
Seastar's internals that ScyllaDB uses; and it's under ScyllaDB's
license, not Seastar's. It's worth reading as an example of what such a
script can do, but a Seastar program is better served by a script of its
own, which can be small.

Here is one, `examples/debugging-and-profiling/seastar.py`. Its first
part is a *pretty printer*, which gdb calls to print a value of a type
that it was registered for:

```{.python include="examples/debugging-and-profiling/seastar.py" snippet="printer"}
```

With it, gdb prints `channel = "news"`. Its second part is a command,
which lists the task queues of every shard, one for each scheduling
group ([chapter](#ch:scheduling-groups)), with how many tasks are ready
to run in each:

```{.python include="examples/debugging-and-profiling/seastar.py" snippet="command"}
```

A command is a class derived from `gdb.Command`, whose `invoke()` gdb
calls; `gdb.parse_and_eval()` evaluates an expression in the program, as
`print` would, and returns a `gdb.Value`, whose fields are read by name.
Each shard's reactor is in the thread-local variable
`seastar::local_engine`, so the command switches to each thread in turn,
and reads that thread's. The names of the reactor's members, such as
`_task_queues`, are Seastar's internals, which a new version of Seastar
may change, so a script must be kept up to date with the Seastar that
the program uses; that's the price of every such script, ScyllaDB's
too. Attached to Shoal while `valkey-benchmark` keeps it busy:

```
(gdb) source seastar.py
(gdb) seastar-task-queues
shard 0 (gdb thread 1, "shoal"):
  "main": shares 1000, 23 tasks ready
  "atexit": shares 1000, 0 tasks ready
  "keys": shares 100, 0 tasks ready
  "rewrite": shares 100, 0 tasks ready
shard 1 (gdb thread 5, "reactor-1"):
  "main": shares 1000, 16 tasks ready
  "atexit": shares 1000, 0 tasks ready
  "keys": shares 100, 0 tasks ready
  "rewrite": shares 100, 0 tasks ready
```

The scheduling groups are Shoal's, `keys` and `rewrite`, of
[chapter](#ch:scheduling-groups), with Seastar's own `main` and
`atexit`; their names print as strings, thanks to the pretty printer.
A shard with hundreds of tasks ready, all the time, is overloaded, and a
queue that stays full while others are empty points to the work that
holds it up.

## Core dumps {index="core dump"}

A *core dump* is a process's memory, written to a file, which gdb can
read as if the process were still there, stopped. The kernel writes one
when a program crashes, if it's allowed to, and `gcore PID`, which comes
with gdb, writes one of a running process without stopping it for more
than the time it takes. On most distributions today, the kernel hands
the core dumps of crashed programs to `systemd-coredump`, which keeps
them, compressed, and `coredumpctl list` lists them, and
`coredumpctl gdb PID` starts gdb on one.

A Seastar program's core dump is as large as the memory that it was
given: Shoal, started with `-m1G`, made a core dump of 1.1 GB, though
only 27 MB of it was written to disk, since the memory that Shoal hadn't
touched yet was left out, as a hole in the file. A production server,
given most of a large machine's memory, makes a core dump of hundreds of
gigabytes, and `systemd-coredump` doesn't keep, by default, one larger
than 32 GB. So ScyllaDB's installation configures it: its
`scylla_coredump_setup` script raises the limits, `ProcessSizeMax` and
`ExternalSizeMax`, to 1024 GB, and the time that `systemd-coredump` may
take to write one.

Then gdb reads it with the program's binary, the same one:

```
$ gdb -x gdbinit build/shoal core.2479975
(gdb) source seastar.py
(gdb) seastar-task-queues
shard 0 (gdb thread 1):
  "main": shares 1000, 0 tasks ready
  ...
```

Everything above works in a core dump, except what needs the process to
run, such as breakpoints, and calling the program's functions; and a
core dump doesn't keep the threads' names, which is why the script
prints none here.

## Profiling with perf {index="perf"}

When a program is slow rather than wrong, the question is where its time
goes. Linux's `perf` answers it, by sampling: it interrupts the program,
hundreds of times a second, and records where each shard was, with the
calls that led there. A user can profile its own processes without
root, if the kernel's setting `kernel.perf_event_paranoid` allows it,
but then only what runs in user space, not the kernel's work for the
program, and `perf` must be told so, or it refuses to start:

```
$ perf record -e cycles:u --user-callchains -F 499 --call-graph dwarf,8192 \
      -p $(pidof shoal) -- sleep 5
```

`-F 499` takes 499 samples a second, and `--call-graph dwarf` records
each sample's stack, which `perf` turns into calls with the program's
debugging information. The other way, with frame pointers (`-g`), is
cheaper, but needs every function compiled to keep them, and Seastar
isn't, by default. It can be: add `-fno-omit-frame-pointer` to the
compiler's flags of Seastar (`configure.py --cflags`, or CMake's
`CMAKE_CXX_FLAGS`) and of the program; a stack then goes through the
system's libraries only if they were built with frame pointers too. We profiled Shoal 16 on two shards, while `valkey-benchmark` sent
it `SET`s and `GET`s of random keys, as in
[chapter](#ch:designing-a-sharded-application).

### Flame graphs {index="flame graph"}

A *flame graph*, Brendan Gregg's way of showing many stacks at once,
shows where the time goes at a glance. His scripts,
`stackcollapse-perf.pl` and `flamegraph.pl`, from
<https://github.com/brendangregg/FlameGraph>, turn `perf script`'s
output into a picture, in which each box is a function, as wide as its
share of the samples, above the function that called it:

```
$ perf script > perf.txt
$ stackcollapse-perf.pl perf.txt > folded.txt
$ flamegraph.pl folded.txt > shoal.svg
```

`folded.txt` has a line for each different stack, with its count. Drawn
as it is, a Seastar program's flame graph is hard to read. Each shard is
a thread, with a stack of its own, so the shards are drawn side by side,
though they do the same work. Most boxes are too narrow for their names,
which are long: a function of `sstring` is
`seastar::basic_sstring<char, unsigned int, 15u, true>::size`. And
between every two functions that matter are several that only pass the
call on, such as `std::__invoke_impl`, `futurize_invoke` and a lambda's
`operator()`. So we clean the stacks before drawing them, with a short
script, `examples/debugging-and-profiling/clean_stacks.py`:

```
$ clean_stacks.py < folded.txt > cleaned.txt
$ flamegraph.pl --colors aqua --bgcolors '#ffffff' cleaned.txt > shoal.svg
```

It merges the shards, by cutting every stack below the reactor's loop,
`seastar::reactor::do_run`, where each shard's thread runs its tasks,
and adding up the stacks that are then the same; it keeps of each name
only the function's own, without its class, template arguments,
parameters or namespace; it drops the frames of C++'s plumbing, and
shows the reactor's loop, and the machinery that resumes coroutines, as
one frame each. [Figure](#fig:flame-shoal) is the result.

![A flame graph of Shoal 16 serving `SET`s and `GET`s, on two shards,
merged, and cleaned by `clean_stacks.py`: each box is a function, as
wide as its share of the samples, above the function that called it.
Above the reactor's loop, from the left: requests that the other shard
sent this one to execute (`send`, `invoke_on`); the connection's loop
(`serve`), which executes requests (`execute_here`), with the key
table's lookups (`find`, `try_emplace`, `probe`), prefetches the keys of
the requests that follow (`prefetch_ahead`), and writes replies
(`write`); and the parser of requests (`parse`).](figures/flame-shoal.svg){#fig:flame-shoal sideways="true"}

About half of the time is the connection's loop, executing requests, a
sixth is parsing them, and a fifth is requests that one shard executes
for the other, with the messages between them: the cost of sharding
that [chapter](#ch:designing-a-sharded-application) measured. What
isn't there is as telling: the kernel's work, which a user's profile
can't see, and the waiting, since `perf` samples only the CPU's work. A
flame graph of a program that waits, for a disk or a lock, shows
little; for that, Seastar's metrics ([chapter](#ch:observability)) and
its tracing tell more.

### From a function to an instruction {index="perf!annotate"}

A flame graph shows the shape; `perf report` gives the numbers. With
`--no-children`, each function is charged only for its own work, not
its callees':

```
$ perf report --no-children --sort symbol -g none --stdio
    10.29%  [.] key_table<entry>::try_emplace(std::string_view)
     6.02%  [.] request_reader::parse(char const*, char const*)
     5.20%  [.] server::prefetch_ahead(...)
     5.11%  [.] std::_Hash_bytes(void const*, unsigned long, unsigned long)
     4.03%  [.] server::serve(...)
     3.86%  [.] operator new(unsigned long)
...
```

The busiest function is the table's `try_emplace()`, with a tenth of the
time, and `perf annotate` shows where in it, instruction by instruction:

```
$ perf annotate --stdio --no-source 'key_table<entry>::try_emplace(...)'
...
    1.18 :   4b237a:        cmpq    %r13, (%rax)
    0.78 :   4b237f:        movsbq  0x17(%r12), %rax
   71.99 :   4b2385:        movq    %r12, %rsi
    0.20 :   4b2388:        testb   %al, %al
...
```

Seven tenths of the function's time is on one instruction, a copy from
one register to another, which takes no time at all. The time is the
instruction before it, which the processor's sampling charges to the
next one: `movsbq 0x17(%r12)` reads the 24th byte of a key in the
table, the last byte of its `sstring`, which says whether the string is
kept inside the object. It's the first time that the lookup touches
this entry of the table, which, among a million keys, isn't in the
processor's cache: the miss that [chapter](#ch:testing)'s benchmark
measured, 74 ns for a lookup against 12 ns when the keys stay in the
cache. The busiest function of the server isn't slow code; it's one
memory access. That's the most common surprise of profiling a server
like this, and why Shoal prefetches the keys of the requests that come
next ([chapter](#ch:memory)).

## Exercises

1. Add to `seastar.py` a command, `seastar-memory`, that prints the free
   memory of every shard. Each shard's allocator keeps its state in the
   thread-local variable `seastar::memory::cpu_mem`, whose
   `nr_free_pages` counts its free pages, of 4 KB each. Try it on a
   running Shoal, and on a core dump.

2. Add a command, `seastar-shard-of`, that tells which shard allocated
   the object at a given address, as this chapter explained. Use it on
   the address of a `sstring` of a coroutine stopped at a breakpoint.

3. Profile Shoal with its log on (`--appendonly yes`), and draw a flame
   graph. Where does the time of the log go?
