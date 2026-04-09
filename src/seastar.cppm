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
#include <seastar/core/fair_queue.hh>
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
#include <seastar/core/on_internal_error.hh>
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
// NOT: #include <seastar/core/shared_ptr_incomplete.hh>
// shared_ptr_incomplete.hh provides a split-header pattern for lw_shared_ptr
// with incomplete types.  It must NOT be in the module GMF because importing
// the module would make the template body visible too early, causing
// static_cast failures when lw_shared_ptr<T>::~lw_shared_ptr is
// instantiated while T is still incomplete.  Consumer TUs that rely on
// this pattern must textually #include it at the point where T is complete.
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

#include <seastar/util/alloc_failure_injector.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/bool_class.hh>
#include <seastar/util/closeable.hh>
#include <seastar/util/conversions.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/file.hh>
#include <seastar/util/later.hh>
#include <seastar/util/lazy.hh>
#include <seastar/util/log-cli.hh>
#include <seastar/util/log.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/program-options.hh>
#include <seastar/util/optimized_optional.hh>
#include <seastar/util/print_safe.hh>
#include <seastar/util/process.hh>
#include <seastar/util/read_first_line.hh>
#include <seastar/util/short_streams.hh>
#include <seastar/util/tmp_file.hh>
#include <seastar/util/memory-data-source.hh>
#include <seastar/util/memory-data-sink.hh>

#include <seastar/net/arp.hh>
#include <seastar/net/packet.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
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
#include <seastar/net/unix_address.hh>

#include <seastar/net/byteorder.hh>

#include <seastar/http/common.hh>
#include <seastar/http/client.hh>
#include <seastar/http/connection_factory.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/file_handler.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/json_path.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/http/request.hh>
#include <seastar/http/retry_strategy.hh>
#include <seastar/http/routes.hh>
#include <seastar/http/transformers.hh>
#include <seastar/http/url.hh>

#include <seastar/json/formatter.hh>
#include <seastar/json/json_elements.hh>

#include <seastar/rpc/rpc.hh>
#include <seastar/rpc/rpc_types.hh>
#include <seastar/rpc/lz4_compressor.hh>
#include <seastar/rpc/lz4_fragmented_compressor.hh>

#include <seastar/coroutine/all.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/coroutine/generator.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/coroutine/switch_to.hh>

export module seastar;

export namespace seastar {

// Core types and utilities
using seastar::app_template;
using seastar::logger;
using seastar::log_level;
using seastar::future;
using seastar::promise;
using seastar::shared_future;
using seastar::shared_promise;
using seastar::make_ready_future;
using seastar::make_exception_future;
using seastar::current_exception_as_future;
using seastar::futurize;
using seastar::futurize_invoke;
using seastar::thread;
using seastar::thread_attributes;
using seastar::reactor;
using seastar::engine;
using seastar::local_engine;
using seastar::smp;
using seastar::shard_id;
using seastar::this_shard_id;

// Async
using seastar::async;

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
using seastar::weakly_referencable;
using seastar::allocate_aligned_buffer;

// Synchronization primitives
using seastar::basic_semaphore;
using seastar::semaphore;
using seastar::broken_semaphore;
using seastar::semaphore_timed_out;
using seastar::semaphore_aborted;
using seastar::named_semaphore_aborted;
using seastar::semaphore_units;
using seastar::named_semaphore;
using seastar::named_semaphore_timed_out;
using seastar::broken_named_semaphore;
using seastar::named_semaphore_exception_factory;
using seastar::get_units;
using seastar::consume_units;
using seastar::try_get_units;
using seastar::with_semaphore;
using seastar::gate;
using seastar::gate_closed_exception;
using seastar::named_gate;
using seastar::with_gate;
using seastar::try_with_gate;
using seastar::condition_variable;
using seastar::broken_condition_variable;
using seastar::condition_variable_timed_out;
using seastar::rwlock;
using seastar::basic_rwlock;
using seastar::shared_mutex;
using seastar::abort_source;
using seastar::abort_requested_exception;

// Containers
using seastar::circular_buffer;
using seastar::circular_buffer_fixed_capacity;
using seastar::chunked_fifo;
using seastar::queue;
using seastar::basic_sstring;
using seastar::sstring;
using seastar::pipe;
using seastar::pipe_reader;
using seastar::pipe_writer;
using seastar::checked_ptr;
using seastar::checked_ptr_is_null_exception;

// Scheduling
using seastar::scheduling_group;
using seastar::scheduling_supergroup;
using seastar::current_scheduling_group;
using seastar::default_scheduling_group;
using seastar::create_scheduling_group;
using seastar::destroy_scheduling_group;
using seastar::rename_scheduling_group;
using seastar::create_scheduling_supergroup;
using seastar::destroy_scheduling_supergroup;
using seastar::max_scheduling_groups;
using seastar::need_preempt;

// Idle CPU handler
using seastar::idle_cpu_handler;
using seastar::idle_cpu_handler_result;
using seastar::work_waiting_on_reactor;


// Timers and clocks
using seastar::timer;
using seastar::lowres_clock;
using seastar::lowres_system_clock;
using seastar::manual_clock;
using seastar::steady_clock_type;

// Sharding
using seastar::sharded;
using seastar::sharded_parameter;
using seastar::async_sharded_service;
using seastar::peering_sharded_service;
using seastar::no_sharded_instance_exception;

// I/O
using seastar::file;
using seastar::file_open_options;
using seastar::open_flags;
using seastar::access_flags;
using seastar::file_permissions;
using seastar::rename_flags;
using seastar::follow_symlink;
using seastar::directory_entry_type;
using seastar::directory_entry;
using seastar::list_directory_generator_type;
using seastar::file_handle;
using seastar::file_handle_impl;
using seastar::file_impl;
using seastar::file_input_stream_options;
using seastar::file_input_stream_history;
using seastar::file_output_stream_options;
using seastar::io_intent;
using seastar::input_stream;
using seastar::output_stream;
using seastar::output_stream_options;
using seastar::data_sink;
using seastar::data_source;
using seastar::data_source_impl;
using seastar::data_sink_impl;
using seastar::stat_data;
using seastar::open_file_dma;
using seastar::open_directory;
using seastar::check_direct_io_support;
using seastar::file_exists;
using seastar::remove_file;
using seastar::rename_file;
using seastar::sync_directory;
using seastar::touch_directory;
using seastar::recursive_touch_directory;
using seastar::recursive_remove_directory;
using seastar::file_stat;
using seastar::file_size;
using seastar::fs_avail;
using seastar::make_file_input_stream;
using seastar::make_file_output_stream;
using seastar::make_pipe_input_stream;
using seastar::make_pipe_output_stream;
using seastar::with_file;
using seastar::with_file_close_on_failure;
using seastar::copy;
using seastar::consumption_result;
using seastar::stop_consuming;
using seastar::skip_bytes;
using seastar::continue_consuming;

// Networking
using seastar::socket;
using seastar::server_socket;
using seastar::connected_socket;
using seastar::accept_result;
using seastar::socket_address;
using seastar::listen_options;
using seastar::ipv4_addr;
using seastar::ipv6_addr;
using seastar::transport;
using seastar::listen;
using seastar::connect;
using seastar::make_ipv4_address;

// Sleep
using seastar::sleep;
using seastar::sleep_abortable;
using seastar::sleep_aborted;

// Utility functions and types
using seastar::do_with;
using seastar::do_for_each;
using seastar::do_until;
using seastar::keep_doing;
using seastar::repeat;
using seastar::repeat_until_value;
using seastar::stop_iteration;
using seastar::parallel_for_each;
using seastar::max_concurrent_for_each;
using seastar::map_reduce;
using seastar::when_all;
using seastar::when_all_succeed;
using seastar::when_any;
using seastar::yield;
using seastar::to_sstring;
using seastar::visit;
using seastar::make_visitor;
using seastar::ref;
using seastar::cref;
using seastar::lazy_deref;
using seastar::lazy_eval;
using seastar::pretty_type_name;
using seastar::log2ceil;
using seastar::log2floor;
using seastar::align_up;
using seastar::align_down;
using seastar::enum_hash;

// Defer and RAII
using seastar::defer;
using seastar::deferred_close;
using seastar::deferred_stop;
using seastar::with_closeable;

// Bool class
using seastar::bool_class;
using seastar::optimized_optional;
using seastar::noncopyable_function;
using seastar::value_of;

// Byte-order / endian helpers
using seastar::cpu_to_be;
using seastar::be_to_cpu;
using seastar::cpu_to_le;
using seastar::le_to_cpu;
using seastar::read_le;
using seastar::read_be;
using seastar::write_le;
using seastar::write_be;

// Serialization streams
using seastar::simple_memory_input_stream;
using seastar::simple_input_stream;
using seastar::simple_memory_output_stream;
using seastar::simple_output_stream;
using seastar::measuring_output_stream;
using seastar::fragmented_memory_input_stream;

// Error handling
using seastar::on_internal_error;
using seastar::on_internal_error_noexcept;
using seastar::on_fatal_internal_error;
using seastar::set_abort_on_internal_error;
using seastar::set_abort_on_ebadf;
using seastar::current_backtrace;
using seastar::throw_with_backtrace;
using seastar::simple_backtrace;
using seastar::shared_backtrace;
using seastar::tasktrace;
using seastar::handle_signal;

// Exception types
using seastar::nested_exception;
using seastar::timed_out_error;
using seastar::cancelled_error;
using seastar::broken_promise;
using seastar::broken_pipe_exception;

// Resource management
using seastar::with_scheduling_group;
using seastar::with_timeout;

// Print utilities
using seastar::format;

// Logging
using seastar::logger_registry;
using seastar::global_logger_registry;
using seastar::logging_settings;
using seastar::apply_logging_settings;
using seastar::level_name;

// POSIX wrappers
using seastar::chown;

// Temporary files
using seastar::tmp_file;
using seastar::tmp_dir;
using seastar::make_tmp_file;

// Unix domain
using seastar::unix_domain_addr;

// Pointer casts
using seastar::static_pointer_cast;
using seastar::dynamic_pointer_cast;

// Additional
using seastar::uninitialized_string;
using seastar::shared_ptr_make_helper;
using seastar::shared_ptr_value_hash;
using seastar::shared_ptr_equal_by_value;
using seastar::indirect_hash;
using seastar::indirect_less;
using seastar::expiring_fifo;
using seastar::futurize_t;
using seastar::is_future;
using seastar::file_input_stream_options;
using seastar::free_deleter;
using seastar::count_leading_zeros;
using seastar::count_trailing_zeros;
using seastar::current_backtrace_tasklocal;
using seastar::saved_backtrace;
using seastar::make_backtraced_exception_ptr;
using seastar::lw_shared_ptr_deleter;
using seastar::make_deleter;
using seastar::make_sstring;
using seastar::subscription;
using seastar::smp_service_group;
using seastar::smp_service_group_config;
using seastar::default_smp_service_group;
using seastar::create_smp_service_group;
using seastar::destroy_smp_service_group;
using seastar::smp_submit_to_options;
using seastar::inheriting_concrete_execution_stage;
using seastar::scheduling_group_key;
using seastar::scheduling_group_key_config;
using seastar::scheduling_group_key_create;
using seastar::scheduling_group_get_specific;
using seastar::reduce_scheduling_group_specific;
using seastar::operator""_KiB;
using seastar::operator""_MiB;
using seastar::operator""_GiB;
using seastar::operator""_TiB;

// Byte-order helpers (seastar:: scope, from net/byteorder.hh)
using seastar::htonq;
using seastar::ntohq;

// consume_be / produce_be (from core/byteorder.hh)
using seastar::consume_be;
using seastar::produce_be;

// Additional symbols
using seastar::now;
using seastar::later;
using seastar::with_clock;
using seastar::memory_output_stream;
using seastar::abort_on_expiry;
using seastar::http_response_parser;
using seastar::semaphore_default_exception_factory;
using seastar::file_desc;
using seastar::make_socket;
using seastar::throw_pthread_error;
using seastar::deferred_action;

}

export namespace seastar::coroutine {

using seastar::coroutine::lambda;
using seastar::coroutine::maybe_yield;
using seastar::coroutine::as_future;
using seastar::coroutine::return_exception_ptr;
using seastar::coroutine::exception;
using seastar::coroutine::parallel_for_each;

}

export namespace seastar::coroutine::experimental {

using seastar::coroutine::experimental::generator;

}

export namespace seastar::metrics {

using seastar::metrics::metric_groups;
using seastar::metrics::metric_group_definition;
using seastar::metrics::label;
using seastar::metrics::label_instance;
using seastar::metrics::description;
using seastar::metrics::shard_label;
using seastar::metrics::make_counter;
using seastar::metrics::make_gauge;
using seastar::metrics::make_histogram;
using seastar::metrics::make_summary;
using seastar::metrics::make_total_bytes;
using seastar::metrics::make_current_bytes;
using seastar::metrics::make_queue_length;
using seastar::metrics::make_total_operations;
using seastar::metrics::histogram;
using seastar::metrics::histogram_bucket;
using seastar::metrics::native_histogram_info;
using seastar::metrics::metric_type_def;
using seastar::metrics::relabel_config;
using seastar::metrics::metric_family_config;
using seastar::metrics::relabel_config_action;

}

export namespace seastar::metrics::impl {

using seastar::metrics::impl::get_values;
using seastar::metrics::impl::get_value_map;
using seastar::metrics::impl::value_vector;
using seastar::metrics::impl::escaped_string;
using seastar::metrics::impl::labels_type;

}

export namespace seastar::memory {

using seastar::memory::stats;
using seastar::memory::scoped_large_allocation_warning_threshold;
using seastar::memory::scoped_large_allocation_warning_disable;
using seastar::memory::get_large_allocation_warning_threshold;
using seastar::memory::scoped_critical_alloc_section;
using seastar::memory::reclaimer;
using seastar::memory::reclaiming_result;
using seastar::memory::local_failure_injector;
using seastar::memory::with_allocation_failures;
using seastar::memory::set_min_free_pages;
using seastar::memory::set_abort_on_allocation_failure;
using seastar::memory::is_abort_on_allocation_failure;
using seastar::memory::allocation_site;
using seastar::memory::sampled_memory_profile;
using seastar::memory::set_heap_profiling_sampling_rate;
using seastar::memory::page_size;

}

export namespace seastar::tls {

using seastar::tls::certificate_credentials;
using seastar::tls::server_credentials;
using seastar::tls::credentials_builder;
using seastar::tls::x509_crt_format;
using seastar::tls::listen;
using seastar::tls::connect;
using seastar::tls::session_resume_mode;
using seastar::tls::tls_options;
using seastar::tls::dh_params;
using seastar::tls::client_auth;
using seastar::tls::subject_alt_name;
using seastar::tls::subject_alt_name_type;
using seastar::tls::ERROR_UNKNOWN_COMPRESSION_ALGORITHM;
using seastar::tls::ERROR_UNKNOWN_CIPHER_TYPE;
using seastar::tls::ERROR_INVALID_SESSION;
using seastar::tls::ERROR_UNEXPECTED_HANDSHAKE_PACKET;
using seastar::tls::ERROR_UNKNOWN_CIPHER_SUITE;
using seastar::tls::ERROR_UNKNOWN_ALGORITHM;
using seastar::tls::ERROR_UNSUPPORTED_SIGNATURE_ALGORITHM;
using seastar::tls::ERROR_SAFE_RENEGOTIATION_FAILED;
using seastar::tls::ERROR_UNSAFE_RENEGOTIATION_DENIED;
using seastar::tls::ERROR_UNKNOWN_SRP_USERNAME;
using seastar::tls::ERROR_PREMATURE_TERMINATION;
using seastar::tls::ERROR_PUSH;
using seastar::tls::ERROR_PULL;
using seastar::tls::ERROR_UNEXPECTED_PACKET;
using seastar::tls::ERROR_UNSUPPORTED_VERSION;
using seastar::tls::ERROR_NO_CIPHER_SUITES;
using seastar::tls::ERROR_DECRYPTION_FAILED;
using seastar::tls::ERROR_MAC_VERIFY_FAILED;

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
using seastar::net::hostent;
using seastar::net::hton;
using seastar::net::ntoh;
using seastar::net::tcp_keepalive_params;

}

export namespace seastar::net::dns {

using seastar::net::dns::resolve_name;
using seastar::net::dns::get_host_by_name;

}

export namespace seastar::http {

using seastar::http::reply;
using seastar::http::request;

}

export namespace seastar::http {

using seastar::http::client;
using seastar::http::connection_factory;
using seastar::http::retry_strategy;
using seastar::http::no_retry_strategy;

}

export namespace seastar::httpd {

using seastar::httpd::unexpected_status_error;
using seastar::httpd::base_exception;
using seastar::httpd::http_server;
using seastar::httpd::http_server_tester;
using seastar::httpd::routes;
using seastar::httpd::handler_base;
using seastar::httpd::function_handler;
using seastar::httpd::url;
using seastar::httpd::operation_type;
using seastar::httpd::json_request_function;
using seastar::httpd::future_json_function;

}

export namespace seastar::rpc {

using seastar::rpc::closed_error;
using seastar::rpc::timeout_error;
using seastar::rpc::stream_closed;
using seastar::rpc::remote_verb_error;
using seastar::rpc::unknown_verb_error;
using seastar::rpc::cancellable;
using seastar::rpc::sink;
using seastar::rpc::source;
using seastar::rpc::opt_time_point;
using seastar::rpc::rpc_clock_type;
using seastar::rpc::compressor;
using seastar::rpc::rcv_buf;
using seastar::rpc::snd_buf;
using seastar::rpc::connection_id;
using seastar::rpc::streaming_domain_type;
using seastar::rpc::protocol;
using seastar::rpc::lz4_compressor;
using seastar::rpc::lz4_fragmented_compressor;

}

export namespace seastar::json {

using seastar::json::jsonable;
using seastar::json::json_void;
using seastar::json::json_return_type;
using seastar::json::formatter;
using seastar::json::stream_range_as_array;

}

export namespace seastar::util {

using seastar::util::basic_memory_data_source;
using seastar::util::temporary_buffer_data_source;
using seastar::util::basic_memory_data_sink;
using seastar::util::read_entire_stream_contiguous;
using seastar::util::as_input_stream;
using seastar::util::read_entire_file_contiguous;

}

export namespace seastar::alien {

using seastar::alien::run_on;

}

export namespace seastar::log_cli {

using seastar::log_cli::options;

}

export namespace seastar::program_options {

using seastar::program_options::string_map;
using seastar::program_options::value;
using seastar::program_options::option_group;

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

template <>
struct hash<seastar::rpc::connection_id>;

}
