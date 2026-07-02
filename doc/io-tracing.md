# IO Tracing

Seastar includes optional LTTng-UST tracepoints in the IO scheduler that
allow low-overhead recording of IO request lifecycle events.  The
tracepoints must be explicitly enabled at build time via
`-DSeastar_LTTNG=ON`, but have near-zero cost (~3 ns per tracepoint) when
tracing is not active.

## Prerequisites

Install the LTTng userspace tracer:

```bash
# Debian/Ubuntu
sudo apt install lttng-tools liblttng-ust-dev

# Fedora/RHEL
sudo dnf install lttng-tools lttng-ust-devel
```

Build Seastar with LTTng support enabled:

```bash
cmake -DSeastar_LTTNG=ON ...
```

Verify that the tracepoints are present:

```bash
readelf -n ./your_app | grep "Provider: seastar_io"
```

You should see entries for `io_queue_queued`, `io_queue_dispatched`,
`io_queue_completed`, and `io_queue_cancelled`.

## Available Tracepoints

All tracepoints belong to the `seastar_io` provider.

| Tracepoint            | Description                                  | Key Fields                              |
|-----------------------|----------------------------------------------|-----------------------------------------|
| `io_queue_queued`     | Request enqueued into the IO fair queue       | dev_id, req_id, direction, pclass, offset, length |
| `io_queue_dispatched` | Request dispatched to the IO backend          | dev_id, req_id                          |
| `io_queue_completed`  | Request completed successfully                | dev_id, req_id                          |
| `io_queue_cancelled`  | Request cancelled before dispatch             | dev_id, req_id                          |

### Field Descriptions

- **dev_id** — IO queue device identifier (corresponds to a mountpoint)
- **req_id** — Unique request identifier (pointer value), stable across
  queued → dispatched → completed lifecycle
- **direction** — `0` = write, `1` = read
- **pclass** — Priority/scheduling class identifier
- **offset** — File offset in bytes
- **length** — Request size in bytes

## Recording a Trace

### Using LTTng (recommended for production recording)

```bash
# Create a tracing session
lttng create io-session --output=/tmp/io-trace

# Enable all seastar IO tracepoints
lttng enable-event -u 'seastar_io:*'

# Start recording
lttng start

# ... run your Seastar application ...

# Stop recording
lttng stop

# View the trace as text
lttng view

# Or use babeltrace2 for more control
babeltrace2 /tmp/io-trace

# Destroy the session
lttng destroy io-session
```

### Attaching to a Running Application

LTTng-UST can attach to an **already running** Seastar application
without restarting it:

```bash
# Assuming your app is already running...
lttng create live-session
lttng enable-event -u 'seastar_io:*'
lttng start

# Events are now being recorded from the running app.
# No restart, no signal, no interruption.

lttng stop
lttng view
lttng destroy live-session
```

The only requirement is that `lttng-sessiond` must have been running
**before** the application started (so the lttng-ust library could
register the tracepoints at startup).  If `lttng-sessiond` was not
running when the app launched, the tracepoints will not be
discoverable.  In practice, ensure `lttng-sessiond --daemonize` is
part of your system startup.

### Example Output

```
[17:33:21.510589059] (+?.?????????) host seastar_io:io_queue_queued: { cpu_id = 0 }, { dev_id = 0, req_id = 0x601000175C80, direction = 0, pclass = 0, offset = 0, length = 4096 }
[17:33:21.510592245] (+0.000003186) host seastar_io:io_queue_dispatched: { cpu_id = 0 }, { dev_id = 0, req_id = 0x601000175C80 }
[17:33:21.518178549] (+0.007586304) host seastar_io:io_queue_completed: { cpu_id = 0 }, { dev_id = 0, req_id = 0x601000175C80 }
[17:33:21.521191175] (+0.003012626) host seastar_io:io_queue_queued: { cpu_id = 0 }, { dev_id = 0, req_id = 0x601000175C80, direction = 1, pclass = 0, offset = 0, length = 4096 }
[17:33:21.521194281] (+0.000003106) host seastar_io:io_queue_dispatched: { cpu_id = 0 }, { dev_id = 0, req_id = 0x601000175C80 }
[17:33:21.521265876] (+0.000071595) host seastar_io:io_queue_completed: { cpu_id = 0 }, { dev_id = 0, req_id = 0x601000175C80 }
```

Here you can see a write (`direction = 0`) at offset 0 taking 7.5 ms
(queued → completed), followed by a read (`direction = 1`) at the same
offset taking only 71 µs.  The delta in parentheses shows the time since
the previous event.

### Using perf (quick ad-hoc inspection)

The tracepoints are also exposed as SDT probes:

```bash
# List available probes
perf list sdt | grep seastar_io

# Record all IO events
perf record -e 'sdt_seastar_io:*' ./your_app

# View results
perf script
```

### Using bpftrace (live analysis / filtering)

```bash
# Count IO requests per second
bpftrace -e 'usdt:./your_app:seastar_io:io_queue_queued { @ops = count(); }
             interval:s:1 { print(@ops); clear(@ops); }'

# Histogram of IO request sizes
bpftrace -e 'usdt:./your_app:seastar_io:io_queue_queued {
    @sizes = hist(arg6);
}'

# Measure queue latency (time from queued to dispatched)
bpftrace -e '
usdt:./your_app:seastar_io:io_queue_queued { @start[arg1] = nsecs; }
usdt:./your_app:seastar_io:io_queue_dispatched /@start[arg1]/ {
    @queue_lat = hist(nsecs - @start[arg1]);
    delete(@start[arg1]);
}'
```

## Per-Shard Event Filtering

Seastar runs one shard per thread, and LTTng-UST records events into
per-thread ring buffers.  Each event carries the virtual thread ID
(`vtid`) in its context, which maps 1:1 to a shard.  Seastar names
its reactor threads `reactor-N` where `N` is the shard number, so
resolving the mapping is straightforward.

### Adding Thread Context

To include thread identifiers in the trace output:

```bash
lttng add-context -u -t vtid -t procname
```

After this, every event will carry the OS thread ID and the thread
name (e.g., `reactor-0`, `reactor-3`).  Example output:

```
[17:33:21.510589059] host seastar_io:io_queue_queued: { cpu_id = 0 }, { vtid = 12345, procname = "reactor-0" }, { dev_id = 0, req_id = 0x601000175C80, direction = 0, ... }
[17:33:21.510592245] host seastar_io:io_queue_dispatched: { cpu_id = 1 }, { vtid = 12346, procname = "reactor-1" }, { dev_id = 0, req_id = 0x601000180A00 }
```

### Filtering Events by Shard

With `babeltrace2`, filter to a specific shard by thread name:

```bash
# Show only shard 0 events
babeltrace2 /tmp/io-trace --filter 'procname == "reactor-0"'

# Show only shard 2 events (using vtid if known)
babeltrace2 /tmp/io-trace --filter 'vtid == 12347'
```

With `lttng enable-event` you can restrict recording to specific threads
from the start (reduces trace volume in production):

```bash
# Only trace shard 0 (by thread name, requires lttng 2.13+)
lttng enable-event -u 'seastar_io:*' --filter 'procname == "reactor-0"'
```

### Resolving Shard ↔ Thread Mapping

If you didn't add `procname` context, the mapping can be reconstructed:

```bash
# While the Seastar app is running, dump thread names:
ls /proc/<pid>/task/*/comm
# Output: reactor-0, reactor-1, ...

# Or from within the trace itself: vtid values are consistent, so
# a single lookup at capture time is enough.
```

No explicit `shard_id` field is emitted in tracepoint payloads — this
avoids bloating every event with redundant data since the thread→shard
mapping is stable for the lifetime of the process.

> **Note:** Shard 0 retains the original process name (possibly truncated
> to 15 characters), while shards 1 and above are named `reactor-1`,
> `reactor-2`, etc.  When filtering, account for this by using `vtid`
> rather than `procname` for shard 0, or simply note that the "non-reactor"
> thread name is shard 0.

## Analyzing Traces

### With babeltrace2

```bash
# Print all events with timestamps (same format as lttng view)
babeltrace2 /tmp/io-trace

# Filter for a specific device
babeltrace2 /tmp/io-trace --filter 'dev_id == 0'

# Convert to JSON for scripting
babeltrace2 --output-format=json /tmp/io-trace > trace.json
```

Example `babeltrace2` output (same format as `lttng view`):

```
[17:33:21.521875455] (+0.000609579) host seastar_io:io_queue_queued: { cpu_id = 0 }, { dev_id = 0, req_id = 0x601000175C80, direction = 0, pclass = 0, offset = 0, length = 4096 }
[17:33:21.521879041] (+0.000003586) host seastar_io:io_queue_dispatched: { cpu_id = 0 }, { dev_id = 0, req_id = 0x601000175C80 }
[17:33:21.521960435] (+0.000081394) host seastar_io:io_queue_completed: { cpu_id = 0 }, { dev_id = 0, req_id = 0x601000175C80 }
```

### Computing IO Latencies

Since LTTng records precise timestamps for each event, latencies are
computed by matching `req_id` values:

- **Queue latency** = `dispatched.timestamp - queued.timestamp`
- **Execution latency** = `completed.timestamp - dispatched.timestamp`
- **Total latency** = `completed.timestamp - queued.timestamp`

### With Trace Compass (GUI)

[Trace Compass](https://www.eclipse.org/tracecompass/) can open CTF
traces produced by LTTng and provides timeline visualization, statistics,
and custom analysis views.

## Overhead

- **When not tracing**: ~3 ns per tracepoint (single branch, predicted not-taken)
- **When actively recording**: ~100–300 ns per event (userspace ring buffer
  write, no kernel transition)
- At 100K IOPS with 3 events per IO: ~30–90 ms of CPU per second (<1% of one core)

## Building Without LTTng

By default (`-DSeastar_LTTNG=OFF`), Seastar builds without tracepoints
(zero overhead).  To enable tracing, pass `-DSeastar_LTTNG=ON` — this
requires `lttng-ust-devel` to be installed and will fail at configure
time if the library is not found.  The `SEASTAR_HAVE_LTTNG_UST`
preprocessor macro indicates whether tracing is compiled in.
