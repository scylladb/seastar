/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2019 ScyllaDB
 */

// we could split the subsystems into multiple module partitions for a cleaner
// structure of the module, but the dependencies between Seastar subsystems
// form a cylic graph, if we split the sources at the boundary of the
// subdirectory the header files are located. for instance:
//   core/future => util/backtrace => core/sstring.
//
// one way to address this circular dependency problem by breaking some
// subsystems into smaller pieces at the expense of creasomg the complexity
// level of the module structure. as each partition has
// - its own source file
// - an entry in CMakeLists.txt
// - one or more cross partition import / export clause when it is used / exposed
//
// a simpler alternative is to put all headers into a the same purview of
// the "seastar" module. but this slows down the build speed of Seastar itself,
// as each time when we modify any of the header file, the whole module is
// recompiled. but this should fine at this moment, as the majority Seastar
// developers are not supposed to build Seastar as a C++ module, which is, in
// general, built for a single time to be consumed by Seastar applications.

module;

#include <seastar/util/std-compat.hh>
#include <seastar/core/abortable_fifo.hh>
#include <seastar/core/abort_on_ebadf.hh>
#include <seastar/core/abort_on_expiry.hh>
#include <seastar/core/abort_source.hh>
#include <seastar/core/alien.hh>
#include <seastar/core/align.hh>
#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/bitops.hh>
#include <seastar/core/bitset-iter.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/cacheline.hh>
#include <seastar/core/checked_ptr.hh>
#include <seastar/core/chunked_fifo.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/circular_buffer_fixed_capacity.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/deleter.hh>
#include <seastar/core/disk_params.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/enum.hh>
#include <seastar/core/exception_hacks.hh>
#include <seastar/core/execution_stage.hh>
#include <seastar/core/expiring_fifo.hh>
#include <seastar/core/file.hh>
#include <seastar/core/file-types.hh>
#include <seastar/core/fsnotify.hh>
#include <seastar/core/fsqual.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/future.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/idle_cpu_handler.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/iostream-impl.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/io_priority_class.hh>
#include <seastar/core/layered_file.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/make_task.hh>
#include <seastar/core/manual_clock.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/metrics_types.hh>
#include <seastar/core/pipe.hh>
#include <seastar/core/polymorphic_temporary_buffer.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/preempt.hh>
#include <seastar/core/prefetch.hh>
#include <seastar/core/print.hh>
// #include <seastar/core/prometheus.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/ragel.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/reactor_config.hh>
#include <seastar/core/relabel_config.hh>
#include <seastar/core/report_exception.hh>
#include <seastar/core/resource.hh>
#include <seastar/core/rwlock.hh>
#include <seastar/core/scattered_message.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/scheduling_specific.hh>
#include <seastar/core/scollectd.hh>
#include <seastar/core/scollectd_api.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/shared_mutex.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/shared_ptr_debug_helper.hh>
#include <seastar/core/shared_ptr_incomplete.hh>
#include <seastar/core/signal.hh>
#include <seastar/core/simple-stream.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/smp_options.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/stream.hh>
#include <seastar/core/stall_sampler.hh>
#include <seastar/core/task.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/timed_out_error.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/transfer.hh>
#include <seastar/core/unaligned.hh>
#include <seastar/core/units.hh>
#include <seastar/core/vector-data-sink.hh>
#include <seastar/core/weak_ptr.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/when_any.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/with_timeout.hh>

#include <seastar/coroutine/all.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/coroutine/generator.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/coroutine/switch_to.hh>

#include <seastar/util/alloc_failure_injector.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/conversions.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/file.hh>
#include <seastar/util/log-cli.hh>
#include <seastar/util/log.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/optimized_optional.hh>
#include <seastar/util/print_safe.hh>
#include <seastar/util/process.hh>
#include <seastar/util/read_first_line.hh>
#include <seastar/util/short_streams.hh>

#include <seastar/net/arp.hh>
#include <seastar/net/packet.hh>
#include <seastar/net/api.hh>
#include <seastar/net/ip_checksum.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/ip.hh>
#include <seastar/net/ipv4_address.hh>
#include <seastar/net/native-stack.hh>
#include <seastar/net/posix-stack.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/tcp.hh>
#include <seastar/net/udp.hh>
#include <seastar/net/tls.hh>

#include <seastar/http/common.hh>
#include <seastar/http/client.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/file_handler.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/json_path.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/http/request.hh>
#include <seastar/http/routes.hh>
#include <seastar/http/transformers.hh>

#include <seastar/json/formatter.hh>
#include <seastar/json/json_elements.hh>

export module seastar;

export namespace seastar {

// Core types and utilities
using seastar::app_template;
using seastar::logger;
using seastar::log_level;
using seastar::future;
using seastar::promise;
using seastar::shared_future;
using seastar::make_ready_future;
using seastar::make_exception_future;
using seastar::thread;
using seastar::thread_attributes;
using seastar::reactor;
using seastar::engine;
using seastar::smp;
using seastar::shard_id;
using seastar::this_shard_id;

// Memory management
using seastar::temporary_buffer;
using seastar::deleter;
using seastar::shared_ptr;
using seastar::lw_shared_ptr;
using seastar::make_shared;
using seastar::make_lw_shared;
using seastar::weak_ptr;
using seastar::enable_shared_from_this;
using seastar::enable_lw_shared_from_this;
using seastar::foreign_ptr;
using seastar::make_foreign;

// Synchronization primitives
using seastar::semaphore;
using seastar::broken_semaphore;
using seastar::semaphore_timed_out;
using seastar::gate;
using seastar::gate_closed_exception;
using seastar::condition_variable;
using seastar::broken_condition_variable;
using seastar::rwlock;
using seastar::shared_mutex;
using seastar::abort_source;
using seastar::abort_requested_exception;

// Containers
using seastar::circular_buffer;
using seastar::circular_buffer_fixed_capacity;
using seastar::chunked_fifo;
using seastar::queue;
using seastar::sstring;

// Scheduling
using seastar::scheduling_group;
using seastar::task;

// Timers and clocks
using seastar::timer;
using seastar::lowres_clock;
using seastar::lowres_system_clock;
using seastar::manual_clock;
using seastar::steady_clock_type;

// Sharding
using seastar::sharded;

// I/O
using seastar::file;
using seastar::file_open_options;
using seastar::open_flags;
using seastar::directory_entry_type;
using seastar::file_handle;
using seastar::input_stream;
using seastar::output_stream;
using seastar::data_sink;
using seastar::data_source;

// Networking
using seastar::socket;
using seastar::server_socket;
using seastar::connected_socket;
using seastar::socket_address;
using seastar::ipv4_addr;
using seastar::ipv6_addr;
using seastar::transport;

// Metrics
using seastar::metrics::metric_groups;
using seastar::metrics::metric_group_definition;
using seastar::metrics::label;
using seastar::metrics::label_instance;

// Utility functions
using seastar::do_with;
using seastar::do_for_each;
using seastar::do_until;
using seastar::repeat;
using seastar::repeat_until_value;
using seastar::parallel_for_each;
using seastar::map_reduce;
using seastar::when_all;
using seastar::when_all_succeed;
using seastar::sleep;
using seastar::yield;

// Exception types
using seastar::nested_exception;
using seastar::timed_out_error;

// Resource management
using seastar::with_scheduling_group;
using seastar::with_timeout;

// Print utilities
using seastar::format;

}

export namespace seastar::net {

using seastar::net::packet;
using seastar::net::fragment;
using seastar::net::ethernet_address;
using seastar::net::ipv4_address;
using seastar::net::ipv6_address;
using seastar::net::inet_address;
using seastar::net::ipv4_tcp;
using seastar::net::ipv4_udp;
using seastar::net::udp_channel;
using seastar::net::datagram;
using seastar::net::get_impl;

}

export namespace seastar::http {

using seastar::http::reply;
using seastar::http::request;

}

export namespace std {

using std::hash;

// Expose hash specializations for seastar types that have them
template <>
struct hash<seastar::socket_address>;

template <>
struct hash<seastar::ipv4_addr>;

template <>
struct hash<seastar::net::inet_address>;

}
