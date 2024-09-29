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

// put all headers not provided by this module into the global module fragment
// to prevent attachment to the module

#include <any>
#include <array>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cassert>
#include <chrono>
#include <compare>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <optional>
#include <queue>
#include <random>
#include <ranges>
#include <regex>
#include <source_location>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/functional/hash.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/parent_from_member.hpp>
#include <boost/intrusive/slist.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/mpl/for_each.hpp>
#include <boost/mpl/range_c.hpp>
#include <boost/next_prior.hpp>
#include <boost/program_options.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/irange.hpp>
#include <boost/thread/barrier.hpp>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <fmt/printf.h>
#include <gnutls/crypto.h>
#ifdef SEASTAR_HAVE_HWLOC
#include <hwloc.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif
#include <linux/fs.h>
#include <linux/perf_event.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <execinfo.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <ucontext.h>
#include <unistd.h>

export module seastar;

// include all declaration and definitions to be exported in the the module
// purview
#include <seastar/util/std-compat.hh>
#include <seastar/core/abortable_fifo.hh>
#include <seastar/core/abort_on_ebadf.hh>
#include <seastar/core/abort_on_expiry.hh>
#include <seastar/core/abort_source.hh>
#include <seastar/core/alien.hh>
#include <seastar/core/align.hh>
#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/array_map.hh>
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
#include <seastar/core/distributed.hh>
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
#include <seastar/core/systemwide_memory_barrier.hh>
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

module : private;

#include <seastar/core/internal/read_state.hh>
#include <seastar/core/internal/buffer_allocator.hh>
#include <seastar/core/internal/io_intent.hh>
#include <seastar/core/internal/stall_detector.hh>
#include <seastar/core/internal/uname.hh>

#include "core/cgroup.hh"
#include "core/file-impl.hh"
#include "core/prefault.hh"
#include "core/program_options.hh"
#include "core/reactor_backend.hh"
#include "core/syscall_result.hh"
#include "core/thread_pool.hh"
#include "core/scollectd-impl.hh"
#include "core/vla.hh"

#include <seastar/util/internal/iovec_utils.hh>
#include <seastar/util/internal/magic.hh>
#include <seastar/util/function_input_iterator.hh>
#include <seastar/util/shared_token_bucket.hh>
#include <seastar/util/transform_iterator.hh>

#include <seastar/net/dhcp.hh>
#include <seastar/net/native-stack.hh>
#include <seastar/net/proxy.hh>
#include <seastar/net/tcp-stack.hh>
#include <seastar/net/toeplitz.hh>
#include <seastar/net/virtio.hh>

#include "net/native-stack-impl.hh"

#include <seastar/http/url.hh>
#include <seastar/http/internal/content_source.hh>
