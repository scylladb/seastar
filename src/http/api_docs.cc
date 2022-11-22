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
 * Copyright 2015 Cloudius Systems
 */

#include <seastar/http/api_docs.hh>
#include <seastar/http/handlers.hh>
#include <seastar/json/formatter.hh>
#include <seastar/http/transformers.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/http/transformers.hh>
#include <seastar/core/loop.hh>

using namespace std;

namespace seastar {

namespace httpd {

const sstring api_registry_builder_base::DEFAULT_PATH = "/api-doc";
const sstring api_registry_builder_base::DEFAULT_DIR = ".";

doc_entry get_file_reader(sstring file_name) {
    return [file_name] (output_stream<char>& os) {
        return open_file_dma(file_name, open_flags::ro).then([&os] (file f) mutable {
            return do_with(input_stream<char>(make_file_input_stream(std::move(f))), [&os](input_stream<char>& is) {
                return copy(is, os).then([&is] {
                    return is.close();
                });
            });
        });
    };
}

future<> api_docs_20::write(output_stream<char>&& os, std::unique_ptr<http::request> req) {
    return do_with(output_stream<char>(_transform.transform(std::move(req), "", std::move(os))), [this] (output_stream<char>& os) {
        return do_for_each(_apis, [&os](doc_entry& api) {
            return api(os);
        }).then([&os] {
            return os.write("},\"definitions\": {");
        }).then([this, &os] {
            return do_for_each(_definitions, [&os](doc_entry& api) {
                return api(os);
            });
        }).then([&os] {
            return os.write("}}");
        }).then([&os] {
            return os.flush();
        }).finally([&os] {
            return os.close();
        });
    });
}

}

}
