/* This file is open source software, licensed to you under the terms
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
 * Copyright (C) 2023 ScyllaDB Ltd.
 */

#include <chrono>
#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>

#include <string_view>
#include <concepts>
#include <seastar/core/internal/cpu_profiler.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/smp.hh>

template <typename Func>
requires (std::is_invocable_v<Func>)
void check_function_allocation(const char *name, size_t expected_allocs, Func f) {
  auto before = seastar::memory::stats();
  f();
  auto after = seastar::memory::stats();

  BOOST_TEST_INFO("After function: " << name);
  BOOST_REQUIRE_EQUAL(expected_allocs, after.mallocs() - before.mallocs());
}

class cpu_profiler_test : public seastar::internal::cpu_profiler {
public:
  cpu_profiler_test(seastar::internal::cpu_profiler_config cfg)
      : seastar::internal::cpu_profiler(cfg) {}

  virtual ~cpu_profiler_test() override = default;
  virtual void arm_timer(std::chrono::nanoseconds) override {
    // noop
  }
  virtual void disarm_timer() override {
    // noop
  }
};

BOOST_AUTO_TEST_CASE(signal_handler_doesnt_alloc) {
  cpu_profiler_test profiler(seastar::internal::cpu_profiler_config{
      true, std::chrono::milliseconds(1)});
  profiler.start();
  check_function_allocation("cpu_profiler_on_signal", 0, [&profiler] {
    for (int i = 0; i < 1'000'000; i++) {
      profiler.on_signal();
    }
  });
}
