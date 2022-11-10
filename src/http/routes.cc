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

#include <seastar/http/routes.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/request.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/json_path.hh>

namespace seastar {

namespace httpd {

using namespace std;

void verify_param(const http::request& req, const sstring& param) {
    if (req.get_query_param(param) == "") {
        throw missing_param_exception(param);
    }
}
routes::routes() : _general_handler([this](std::exception_ptr eptr) mutable {
    return exception_reply(eptr);
}) {}

routes::~routes() {
    for (int i = 0; i < NUM_OPERATION; i++) {
        for (auto kv : _map[i]) {
            delete kv.second;
        }
    }
    for (int i = 0; i < NUM_OPERATION; i++) {
        for (auto r : _rules[i]) {
            delete r.second;
        }
    }

}

std::unique_ptr<http::reply> routes::exception_reply(std::exception_ptr eptr) {
    auto rep = std::make_unique<http::reply>();
    try {
        // go over the register exception handler
        // if one of them handle the exception, return.
        for (auto e: _exceptions) {
            try {
                return e.second(eptr);
            } catch (...) {
                // this is needed if there are more then one register exception handler
                // so if the exception handler throw a new exception, they would
                // get the new exception and not the original one.
                eptr = std::current_exception();
            }
        }
        std::rethrow_exception(eptr);
    } catch (const base_exception& e) {
        rep->set_status(e.status(), json_exception(e).to_json());
    } catch (...) {
        rep->set_status(http::reply::status_type::internal_server_error,
                json_exception(std::current_exception()).to_json());
    }

    rep->done("json");
    return rep;
}

future<std::unique_ptr<http::reply> > routes::handle(const sstring& path, std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) {
    handler_base* handler = get_handler(str2type(req->_method),
            normalize_url(path), req->param);
    if (handler != nullptr) {
        try {
            for (auto& i : handler->_mandatory_param) {
                verify_param(*req.get(), i);
            }
            auto r =  handler->handle(path, std::move(req), std::move(rep));
            return r.handle_exception(_general_handler);
        } catch (const redirect_exception& _e) {
            rep.reset(new http::reply());
            rep->add_header("Location", _e.url).set_status(_e.status()).done(
                    "json");
        } catch (...) {
            rep = exception_reply(std::current_exception());
        }
    } else {
        rep.reset(new http::reply());
        json_exception ex(not_found_exception("Not found"));
        rep->set_status(http::reply::status_type::not_found, ex.to_json()).done(
                "json");
    }
    return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
}

sstring routes::normalize_url(const sstring& url) {
    if (url.length() < 2 || url.at(url.length() - 1) != '/') {
        return url;
    }
    return url.substr(0, url.length() - 1);
}

handler_base* routes::get_handler(operation_type type, const sstring& url,
        parameters& params) {
    handler_base* handler = get_exact_match(type, url);
    if (handler != nullptr) {
        return handler;
    }

    for (auto&& rule : _rules[type]) {
        handler = rule.second->get(url, params);
        if (handler != nullptr) {
            return handler;
        }
        params.clear();
    }
    return _default_handler;
}

routes& routes::add(operation_type type, const url& url,
        handler_base* handler) {
    match_rule* rule = new match_rule(handler);
    rule->add_str(url._path);
    if (url._param != "") {
        rule->add_param(url._param, true);
    }
    return add(rule, type);
}

routes& routes::add_default_handler(handler_base* handler) {
    _default_handler = handler;
    return *this;
}

template <typename Map, typename Key>
static auto delete_rule_from(operation_type type, Key& key, Map& map) {
    auto& bucket = map[type];
    auto ret = bucket.find(key);
    using ret_type = decltype(ret->second);
    if (ret != bucket.end()) {
        ret_type v = ret->second;
        bucket.erase(ret);
        return v;
    }
    return static_cast<ret_type>(nullptr);
}

handler_base* routes::drop(operation_type type, const sstring& url) {
    return delete_rule_from(type, url, _map);
}

routes& routes::put(operation_type type, const sstring& url, handler_base* handler) {
    auto it = _map[type].emplace(url, handler);
    if (it.second == false) {
        throw std::runtime_error(format("Handler for {} already exists.", url));
    }
    return *this;
}

match_rule* routes::del_cookie(rule_cookie cookie, operation_type type) {
    return delete_rule_from(type, cookie, _rules);
}

void routes::add_alias(const path_description& old_path, const path_description& new_path) {
    httpd::parameters p;
    stringstream path;
    path << old_path.path;
    for (const auto& p : old_path.params) {
        // the path_description path does not contains the path parameters
        // so just add a fake parameter to the path for each of the parameters,
        // and add the string for each fixed string part.
        if (p.type == path_description::url_component_type::FIXED_STRING) {
            path << p.name;
        } else {
            path << "/k";
        }

    }
    auto a = get_handler(old_path.operations.method, path.str(), p);
    if (!a) {
        throw std::runtime_error("routes::add_alias path_description not found: " + old_path.path);
    }
    // if a handler is found then it must be a function_handler
    new_path.set(*this, new function_handler(*static_cast<function_handler*>(a)));
}

rule_registration::rule_registration(routes& rs, match_rule& rule, operation_type op)
        : _routes(rs) , _op(op)
        , _cookie(_routes.add_cookie(&rule, _op)) {}

rule_registration::~rule_registration() {
    _routes.del_cookie(_cookie, _op);
}

handler_registration::handler_registration(routes& rs, handler_base& h, const sstring& url, operation_type op)
        : _routes(rs), _url(url), _op(op) {
    _routes.put(_op, _url, &h);
}

handler_registration::~handler_registration() {
    _routes.drop(_op, _url);
}

}

}
