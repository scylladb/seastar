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

#ifdef SEASTAR_MODULE
module;
#endif

#include <algorithm>
#include <iostream>
#include <memory>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/http/file_handler.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/app-template.hh>
#include <seastar/http/exception.hh>
#endif

namespace seastar {

namespace httpd {

directory_handler::directory_handler(const sstring& doc_root,
        file_transformer* transformer)
        : file_interaction_handler(transformer), doc_root(doc_root) {
}

future<std::unique_ptr<http::reply>> directory_handler::handle(const sstring& path,
        std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) {
    sstring full_path = doc_root + req->param.get_decoded_param("path");
    auto h = this;
    return engine().file_type(full_path).then(
            [h, full_path, req = std::move(req), rep = std::move(rep)](auto val) mutable {
                if (val) {
                    if (val.value() == directory_entry_type::directory) {
                        if (h->redirect_if_needed(*req.get(), *rep.get())) {
                            return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
                        }
                        full_path += "/index.html";
                    }
                    return h->read(full_path, std::move(req), std::move(rep));
                }
                rep->set_status(http::reply::status_type::not_found).done();
                return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));

            });
}

file_interaction_handler::~file_interaction_handler() {
    delete transformer;
}

sstring file_interaction_handler::get_extension(const sstring& file) {
    size_t last_slash_pos = file.find_last_of('/');
    size_t last_dot_pos = file.find_last_of('.');
    sstring extension;
    if (last_dot_pos != sstring::npos && last_dot_pos > last_slash_pos) {
        extension = file.substr(last_dot_pos + 1);
    }
    // normalize file extension for mime type
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    return extension;
}

output_stream<char> file_interaction_handler::get_stream(std::unique_ptr<http::request> req,
        const sstring& extension, output_stream<char>&& s) {
    if (transformer) {
        return transformer->transform(std::move(req), extension, std::move(s));
    }
    return std::move(s);
}

future<std::unique_ptr<http::reply>> file_interaction_handler::read(
        sstring file_name, std::unique_ptr<http::request> req,
        std::unique_ptr<http::reply> rep) {
    sstring extension = get_extension(file_name);
    rep->write_body(extension, [req = std::move(req), extension, file_name, this] (output_stream<char>&& s) mutable {
        return do_with(get_stream(std::move(req), extension, std::move(s)),
                [file_name] (output_stream<char>& os) {
            return open_file_dma(file_name, open_flags::ro).then([&os] (file f) {
                return do_with(make_file_input_stream(std::move(f)), [&os](input_stream<char>& is) {
                    return copy(is, os).finally([&os] {
                        return os.close();
                    }).finally([&is] {
                        return is.close();
                    });
                });
            });
        });
    });
    return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
}

bool file_interaction_handler::redirect_if_needed(const http::request& req,
        http::reply& rep) const {
    if (req._url.length() == 0 || req._url.back() != '/') {
        rep.set_status(http::reply::status_type::moved_permanently);
        rep._headers["Location"] = req.get_url() + "/";
        rep.done();
        return true;
    }
    return false;
}

future<std::unique_ptr<http::reply>> file_handler::handle(const sstring& path,
        std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) {
    if (force_path && redirect_if_needed(*req.get(), *rep.get())) {
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }
    return read(file, std::move(req), std::move(rep));
}

}

}
