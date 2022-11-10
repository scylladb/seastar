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

#pragma once
#include <seastar/json/json_elements.hh>
#include <seastar/json/formatter.hh>
#include <seastar/http/routes.hh>
#include <seastar/http/transformers.hh>
#include <string>
#include <seastar/util/noncopyable_function.hh>

namespace seastar {

namespace httpd {

struct api_doc : public json::json_base {
    json::json_element<std::string> path;
    json::json_element<std::string> description;

    void register_params() {
        add(&path, "path");
        add(&description, "description");

    }
    api_doc() {
        register_params();
    }
    api_doc(const api_doc & e)
      : json::json_base()
    {
        register_params();
        path = e.path;
        description = e.description;
    }
    template<class T>
    api_doc& operator=(const T& e) {
        path = e.path;
        description = e.description;
        return *this;
    }
    api_doc& operator=(const api_doc& e) {
        path = e.path;
        description = e.description;
        return *this;
    }
};

struct api_docs : public json::json_base {
    json::json_element<std::string> apiVersion;
    json::json_element<std::string> swaggerVersion;
    json::json_list<api_doc> apis;

    void register_params() {
        add(&apiVersion, "apiVersion");
        add(&swaggerVersion, "swaggerVersion");
        add(&apis, "apis");

    }
    api_docs() {
        apiVersion = "0.0.1";
        swaggerVersion = "1.2";
        register_params();
    }
    api_docs(const api_docs & e)
      : json::json_base()
    {
        apiVersion = "0.0.1";
        swaggerVersion = "1.2";
        register_params();
    }
    template<class T>
    api_docs& operator=(const T& e) {
        apis = e.apis;
        return *this;
    }
    api_docs& operator=(const api_docs& e) {
        apis = e.apis;
        return *this;
    }
};

class api_registry_base : public handler_base {
protected:
    sstring _base_path;
    sstring _file_directory;
    routes& _routes;

public:
    api_registry_base(routes& routes, const sstring& file_directory,
            const sstring& base_path)
            : _base_path(base_path), _file_directory(file_directory), _routes(
                    routes) {
    }

    void set_route(handler_base* h) {
        _routes.put(GET, _base_path, h);
    }
    virtual ~api_registry_base() = default;
};

class api_registry : public api_registry_base {
    api_docs _docs;
public:
    api_registry(routes& routes, const sstring& file_directory,
            const sstring& base_path)
            : api_registry_base(routes, file_directory, base_path) {
        set_route(this);
    }

    future<std::unique_ptr<http::reply>> handle(const sstring& path,
            std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) override {
        rep->_content = json::formatter::to_json(_docs);
        rep->done("json");
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }

    void reg(const sstring& api, const sstring& description,
            const sstring& alternative_path = "") {
        api_doc doc;
        doc.description = description;
        doc.path = "/" + api;
        _docs.apis.push(doc);
        sstring path =
                (alternative_path == "") ?
                        _file_directory + api + ".json" : alternative_path;
        file_handler* index = new file_handler(path,
                new content_replace("json"));
        _routes.put(GET, _base_path + "/" + api, index);
    }
};

class api_registry_builder_base {
protected:
    sstring _file_directory;
    sstring _base_path;
    static const sstring DEFAULT_DIR;
    static const sstring DEFAULT_PATH;
public:
    api_registry_builder_base(const sstring& file_directory = DEFAULT_DIR,
            const sstring& base_path = DEFAULT_PATH)
            : _file_directory(file_directory), _base_path(base_path) {
    }
};

class api_registry_builder : public api_registry_builder_base {
public:
    api_registry_builder(const sstring& file_directory = DEFAULT_DIR,
            const sstring& base_path = DEFAULT_PATH)
            : api_registry_builder_base(file_directory, base_path) {
    }

    void set_api_doc(routes& r) {
        new api_registry(r, _file_directory, _base_path);
    }

    void register_function(routes& r, const sstring& api,
            const sstring& description, const sstring& alternative_path = "") {
        auto h = r.get_exact_match(GET, _base_path);
        if (h) {
            // if a handler is found, it was added there by the api_registry_builder
            // with the set_api_doc method, so we know it's the type
            static_cast<api_registry*>(h)->reg(api, description, alternative_path);
        };
    }
};

using doc_entry = noncopyable_function<future<>(output_stream<char>&)>;

/*!
 * \brief a helper function that creates a reader from a file
 */

doc_entry get_file_reader(sstring file_name);

/*!
 * \brief An api doc that support swagger version 2.0
 *
 * The result is a unified JSON file with the swagger definitions.
 *
 * The file content is a concatenation of the doc_entry by the order of
 * their entry.
 *
 * Definitions will be added under the definition section
 *
 * typical usage:
 *
 * First entry:
 *
  {
  "swagger": "2.0",
  "host": "localhost:10000",
  "basePath": "/v2",
  "paths": {

 * entry:
 "/config/{id}": {
      "get": {
        "description": "Return a config value",
        "operationId": "findConfigId",
        "produces": [
          "application/json"
        ],
        }
        }
 *
 * Closing the entries:
  },

  "definitions": {
  .....

  .....
  }
}
 *
 */
class api_docs_20 {
    std::vector<doc_entry> _apis;
    content_replace _transform;
    std::vector<doc_entry> _definitions;

public:
    future<> write(output_stream<char>&&, std::unique_ptr<http::request> req);

    void add_api(doc_entry&& f) {
        _apis.emplace_back(std::move(f));
    }

    void add_definition(doc_entry&& f) {
        _definitions.emplace_back(std::move(f));
    }
};

class api_registry_20 : public api_registry_base {
    api_docs_20 _docs;
public:
    api_registry_20(routes& routes, const sstring& file_directory,
            const sstring& base_path)
            : api_registry_base(routes, file_directory, base_path) {
        set_route(this);
    }

    future<std::unique_ptr<http::reply>> handle(const sstring& path,
            std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) override {
        rep->write_body("json", [this, req = std::move(req)] (output_stream<char>&& os) mutable {
            return _docs.write(std::move(os), std::move(req));
        });
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }

    virtual void reg(doc_entry&& f) {
        _docs.add_api(std::move(f));
    }

    virtual void add_definition(doc_entry&& f) {
        _docs.add_definition(std::move(f));
    }
};

class api_registry_builder20 : public api_registry_builder_base {
    api_registry_20* get_register_base(routes& r) {
        auto h = r.get_exact_match(GET, _base_path);
        if (h) {
            // if a handler is found, it was added there by the api_registry_builder
            // with the set_api_doc method, so we know it's the type
            return static_cast<api_registry_20*>(h);
        }
        return nullptr;
    }

public:
    api_registry_builder20(const sstring& file_directory = DEFAULT_DIR,
            const sstring& base_path = DEFAULT_PATH)
            : api_registry_builder_base(file_directory, base_path) {
    }

    void set_api_doc(routes& r) {
        new api_registry_20(r, _file_directory, _base_path);
    }

    /*!
     * \brief register a doc_entry
     * This doc_entry can be used to either take the definition from a file
     * or generate them dynamically.
     */
    void register_function(routes& r, doc_entry&& f) {
        auto h = get_register_base(r);
        if (h) {
            h->reg(std::move(f));
        }
    }
    /*!
     * \brief register an API
     */
    void register_api_file(routes& r, const sstring& api) {
        register_function(r, get_file_reader(_file_directory + "/" + api + ".json"));
    }


    /*!
     * Add a footer doc_entry
     */
    void add_definition(routes& r, doc_entry&& f) {
        auto h = get_register_base(r);
        if (h) {
            h->add_definition(std::move(f));
        }

    }

    /*!
     * Add a definition file
     */
    void add_definitions_file(routes& r, const sstring& file) {
        add_definition(r, get_file_reader(_file_directory + file + ".def.json" ));
    }

};

}

}
