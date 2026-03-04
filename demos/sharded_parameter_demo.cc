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
 * Copyright 2020 ScyllaDB
 */


// Demonstration of seastar::sharded_parameter

#include <seastar/core/sharded.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/closeable.hh>

// This is some service that we wish to run on all shards.
class service_one {
    int _capacity = 7;
public:
    // Pretend that this int is some important resource.
    int get_capacity() const { return _capacity; }
};

// Another service that we run on all shards, that depends on service_one.
class service_two {
    int _resource_allocation;
public:
    service_two(service_one& s1, int resource_allocation) : _resource_allocation(resource_allocation) {}
    int get_resource_allocation() const { return _resource_allocation; }
};

int main(int ac, char** av) {
    seastar::app_template app;
    return app.run(ac, av, [&] {
        // sharded<> setup code is typically run in a seastar::thread
        return seastar::async([&] {

            // Launch service_one
            seastar::sharded<service_one> s1;
            s1.start().get();
            auto stop_s1 = seastar::deferred_stop(s1);

            auto calculate_half_capacity = [] (service_one& s1) {
                return s1.get_capacity() / 2;
            };

            // Launch service_two, passing it per-shard dependencies from s1
            seastar::sharded<service_two> s2;
            // Start s2, passing two parameters to service_two's constructor
            s2.start(
                    // Each service_two instance will get a reference to a service_one instance on the same shard
                    std::ref(s1),
                    // This calculation will be performed on each shard
                    seastar::sharded_parameter(calculate_half_capacity, std::ref(s1))
            ).get();
            auto stop_s2 = seastar::deferred_stop(s2);

            s2.invoke_on_all([] (service_two& s2) {
                SEASTAR_ASSERT(s2.get_resource_allocation() == 3);
            }).get();
        });
    });
}
