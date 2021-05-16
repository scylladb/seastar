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
 * Copyright (C) 2021 ScyllaDB
 */
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/closeable.hh>
#include <yaml-cpp/yaml.h>

using namespace seastar;

int main(int ac, char** av) {
    namespace bpo = boost::program_options;

    app_template app;
    auto opt_add = app.add_options();
    opt_add
        ("directory", bpo::value<sstring>()->default_value("."), "directory to work on")
    ;

    return app.run(ac, av, [&] {
        return seastar::async([&] {
            auto& opts = app.configuration();
            auto& storage = opts["directory"].as<sstring>();

            YAML::Emitter out;
            out << YAML::BeginDoc;
            out << YAML::BeginMap;

            engine().open_file_dma(storage + "/tempfile", open_flags::rw | open_flags::create | open_flags::exclusive).then([&] (file f) {
                return with_closeable(std::move(f), [&out, &storage] (file& f) {
                    return remove_file(storage + "/tempfile").then([&out, &f] {
                        out << YAML::Key << "disk_read_max_length" << YAML::Value << f.disk_read_max_length();
                        out << YAML::Key << "disk_write_max_length" << YAML::Value << f.disk_write_max_length();
                    });
                });
            }).get();

            out << YAML::EndMap;
            out << YAML::EndDoc;
            std::cout << out.c_str();
        });
    });
}
