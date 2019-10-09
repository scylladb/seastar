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
 *  Copyright (C) 2018 Scylladb, Ltd.
 */

#include <seastar/testing/thread_test_case.hh>

#include <seastar/core/sharded.hh>

using namespace seastar;

namespace {
class invoke_on_during_stop final : public peering_sharded_service<invoke_on_during_stop> {
    bool flag = false;

public:
    future<> stop() {
        return container().invoke_on(0, [] (invoke_on_during_stop& instance) {
            instance.flag = true;
        });
    }

    ~invoke_on_during_stop() {
        if (engine().cpu_id() == 0) {
            assert(flag);
        }
    }
};
}

SEASTAR_THREAD_TEST_CASE(invoke_on_during_stop_test) {
    sharded<invoke_on_during_stop> s;
    s.start().get();
    s.stop().get();
}

class mydata {
public:
    int x = 1;
    future<> stop() {
        return make_ready_future<>();
    }
};

SEASTAR_THREAD_TEST_CASE(invoke_map_returns_non_future_value) {
    seastar::sharded<mydata> s;
    s.start().get();
    s.map([] (mydata& m) {
        return m.x;
    }).then([] (std::vector<int> results) {
        for (auto& x : results) {
            assert(x == 1);
        }
    }).get();
    s.stop().get();
};

SEASTAR_THREAD_TEST_CASE(invoke_map_returns_future_value) {
    seastar::sharded<mydata> s;
    s.start().get();
    s.map([] (mydata& m) {
        return make_ready_future<int>(m.x);
    }).then([] (std::vector<int> results) {
        for (auto& x : results) {
            assert(x == 1);
        }
    }).get();
    s.stop().get();
}

SEASTAR_THREAD_TEST_CASE(invoke_map_returns_future_value_from_thread) {
    seastar::sharded<mydata> s;
    s.start().get();
    s.map([] (mydata& m) {
        return seastar::async([&m] {
            return m.x;
        });
    }).then([] (std::vector<int> results) {
        for (auto& x : results) {
            assert(x == 1);
        }
    }).get();
    s.stop().get();
}
