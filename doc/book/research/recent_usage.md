# Seastar APIs in last year's ScyllaDB development

Produced by `recent_usage.py ~/seastar ~/scylla origin/master 2025-11-05`
(ScyllaDB origin/master of 2026-11-04; 4,804 non-merge commits touching C++
files). Full data: `recent_usage.tsv`.

Columns: *current* = occurrences in today's tree; *added*/*removed* = in lines
added/removed by last year's commits; *net* = added - removed; *ratio* = share
of last year's additions divided by share of today's tree (above 1: rising).

## Rising (new code uses more than its share)

| API | current | added | removed | net | ratio |
|---|---|---|---|---|---|
| `this_smp_shard_count()` (replaced `smp::count`: 404 removed, 4 left) | 952 | 987 | 35 | +952 | 2.33 |
| `(this auto` lambda coroutines | 34 | 63 | 33 | +30 | 4.16 |
| `with_scheduling_group(` | 36 | 56 | 51 | +5 | 3.49 |
| `gate::holder` | 63 | 88 | 65 | +23 | 3.14 |
| `coroutine::as_future` | 116 | 131 | 59 | +72 | 2.54 |
| `defer(` | 469 | 514 | 305 | +209 | 2.46 |
| `coroutine::try_future` (added to Seastar 2025-09) | 47 | 49 | 2 | +47 | 2.34 |
| `SEASTAR_FIXTURE_TEST_CASE` (added to Seastar 2025-08) | 155 | 149 | 5 | +144 | 2.16 |
| `max_concurrent_for_each(` | 36 | 29 | 20 | +9 | 1.81 |
| `coroutine::return_exception` | 195 | 147 | 77 | +70 | 1.69 |
| `co_await` | 9826 | 6547 | 3886 | +2661 | 1.50 |
| `abort_source` | 1306 | 870 | 490 | +380 | 1.50 |
| `.local()` (sharded) | 1943 | 1222 | 735 | +487 | 1.41 |
| `get_units(` | 168 | 102 | 54 | +48 | 1.36 |
| `lowres_clock` | 322 | 193 | 91 | +102 | 1.35 |

## Fading (common in the tree, rare in new code)

| API | current | added | removed | net | ratio |
|---|---|---|---|---|---|
| `repeat(` | 25 | 0 | 1 | -1 | 0.00 |
| `do_until(` | 56 | 3 | 2 | +1 | 0.12 |
| `manual_clock` | 144 | 17 | 0 | +17 | 0.27 |
| `json_return_type` | 478 | 83 | 62 | +21 | 0.39 |
| `seastar::async` | 557 | 99 | 54 | +45 | 0.40 |
| `deferred_close` (vs. `deferred_stop` at 1.00) | 293 | 53 | 35 | +18 | 0.41 |
| `named_semaphore` | 76 | 15 | 14 | +1 | 0.44 |
| `.handle_exception_type()` | 32 | 6 | 7 | -1 | 0.42 |
| `.then_wrapped()` | 112 | 24 | 72 | -48 | 0.48 |
| `.then()` | 1153 | 254 | 616 | -362 | 0.49 |
| `rpc::` | 675 | 147 | 87 | +60 | 0.49 |
| `rwlock` | 43 | 10 | 14 | -4 | 0.52 |
| `.discard_result()` | 300 | 86 | 227 | -141 | 0.64 |
| `seastar::thread` | 79 | 23 | 10 | +13 | 0.65 |
| `do_for_each(` | 26 | 8 | 17 | -9 | 0.69 |
| `do_with(` | 87 | 32 | 46 | -14 | 0.83 |

`SEASTAR_THREAD_TEST_CASE` (1.06) and `SEASTAR_TEST_CASE` (1.02) hold steady:
Seastar threads survive mainly in tests and startup code.

Caveats: counts are of text, so generic short names (`get`, `ptr`, `server`)
collide with ScyllaDB's own identifiers and are ignored here; moved code
counts as both added and removed, which is why *net* matters.
