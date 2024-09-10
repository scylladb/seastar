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

#ifndef SEASTAR_MODULE
#include <boost/program_options/variables_map.hpp>
#include <unordered_map>
#endif

#include <seastar/http/matchrules.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/common.hh>
#include <seastar/http/reply.hh>
#include <seastar/util/modules.hh>

namespace seastar {

namespace httpd {

SEASTAR_MODULE_EXPORT_BEGIN
/**
 * The url helps defining a route.
 */
class url {
public:
    /**
     * Move constructor
     */
    url(url&&) = default;

    /**
     * Construct with a url path as it's parameter
     * @param path the url path to be used
     */
    url(const sstring& path)
            : _path(path) {
    }

    /**
     * Adds a parameter that matches untill the end of the URL.
     * @param param the parmaeter name
     * @return the current url
     */
    url& remainder(const sstring& param) {
        this->_param = param;
        return *this;
    }

    sstring _path;
    sstring _param;
};

struct path_description;

/**
 * routes object do the request dispatching according to the url.
 * It uses two decision mechanism exact match, if a url matches exactly
 * (an optional leading slash is permitted) it is chosen
 * If not, the matching rules are used.
 * matching rules are evaluated by their insertion order
 */
class routes {
public:
    /**
     * The destructor deletes the match rules and handlers
     */
    ~routes();

    /**
     * adding a handler as an exact match
     * @param url the url to match (note that url should start with /)
     * @param handler the desire handler
     * @return itself
     * @attention If successful, this method takes ownership of the handler
        pointer. It will be automatically deleted when the routes instance is
        destroyed. However, if the method throws, the caller is responsible
        for deleting the handler pointer.
     */
    routes& put(operation_type type, const sstring& url, handler_base* handler);

    /**
     * removing a handler from exact match
     * @param url the url to match (note that url should start with /)
     * @return the current handler (to be removed by caller)
     */
    handler_base* drop(operation_type type, const sstring& url);

    /**
     * add a rule to be used.
     * rules are search only if an exact match was not found.
     * rules are search by the order they were added.
     * First in higher priority
     * @param rule a rule to add
     * @param type the operation type
     * @return itself
     * @attention This method takes ownership of the match_rule pointer. It will be automatically deleted when the
     * routes instance is destroyed.
     */
    routes& add(match_rule* rule, operation_type type = GET) {
        _rules[type][_rover++] = rule;
        return *this;
    }

    /**
     * Add a url match to a handler:
     * Example  routes.add(GET, url("/api").remainder("path"), handler);
     * @param type
     * @param url
     * @param handler
     * @return
     * @attention This method takes ownership of the handler's pointer.It will be automatically deleted when the routes
     * instance is destroyed.
     */
    routes& add(operation_type type, const url& url, handler_base* handler);

    /**
     * Add a default handler - handles any HTTP Method and Path (/\*) combination:
     * Example  routes.add_default_handler(handler);
     * @param handler
     * @return
     * @attention The caller must keep the @c handler active as long as the route instance is active.
     */
    routes& add_default_handler(handler_base* handler);

    /**
     * the main entry point.
     * the general handler calls this method with the request
     * the method takes the headers from the request and find the
     * right handler.
     * It then calls the handler with the parameters (if they exist) found in the url
     * @param path the url path found
     * @param req the http request
     * @param rep the http reply
     */
    future<std::unique_ptr<http::reply> > handle(const sstring& path, std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep);

    /**
     * Search and return an exact match
     * @param url the request url
     * @return the handler if exists or nullptr if it does not
     */
    handler_base* get_exact_match(operation_type type, const sstring& url) const {
        auto i = _map[type].find(url);
        return (i == _map[type].end()) ? nullptr : i->second;
    }

    /**
     * Search and return a handler by the operation type and url
     * @param type the http operation type
     * @param url the request url
     * @param params a parameter object that will be filled during the match
     * @return a handler based on the type/url match
     */
    handler_base* get_handler(operation_type type, const sstring& url,
            parameters& params);

private:
    /**
     * Normalize the url to remove the last / if exists
     * and get the parameter part
     * @param url the full url path
     * @return the url from the request without the last /
     */
    sstring normalize_url(const sstring& url);

    std::unordered_map<sstring, handler_base*> _map[NUM_OPERATION];
public:
    using rule_cookie = uint64_t;
private:
    rule_cookie _rover = 0;
    std::map<rule_cookie, match_rule*> _rules[NUM_OPERATION];
    //default Handler -- for any HTTP Method and Path (/*)
    handler_base* _default_handler = nullptr;
public:
    using exception_handler_fun = std::function<std::unique_ptr<http::reply>(std::exception_ptr eptr)>;
    using exception_handler_id = size_t;
private:
    std::map<exception_handler_id, exception_handler_fun> _exceptions;
    exception_handler_id _exception_id = 0;
    // for optimization reason, the lambda function
    // that calls the exception_reply of the current object
    // is stored
    exception_handler_fun _general_handler;
public:
    /**
     * The exception_handler_fun expect to call
     * std::rethrow_exception(eptr);
     * and catch only the exception it handles
     */
    exception_handler_id register_exeption_handler(exception_handler_fun fun) {
        auto current = _exception_id++;
        _exceptions[current] = fun;
        return current;
    }

    void remove_exception_handler(exception_handler_id id) {
        _exceptions.erase(id);
    }

    std::unique_ptr<http::reply> exception_reply(std::exception_ptr eptr);

    routes();

    /*!
     * \brief add an alias to an already registered path.
     * After registering a handler to a path, use this method
     * to add an alias to that handler.
     *
     */
    void add_alias(const path_description& old_path, const path_description& new_path);

    /**
     * Add a rule to be used.
     * @param rule a rule to add
     * @param type the operation type
     * @return a cookie using which the rule can be removed
     * @attention This method takes ownership of the match_rule pointer. It will be automatically deleted when the
     * routes instance is destroyed.
     */
    rule_cookie add_cookie(match_rule* rule, operation_type type) {
        auto pos = _rover++;
        _rules[type][pos] = rule;
        return pos;
    }

    /**
     * Del a rule by cookie
     * @param cookie a cookie returned previously by add_cookie
     * @param type the operation type
     * @return the pointer to the rule
     * @attention Ownership of the pointer is returned to the caller.
     */
    match_rule* del_cookie(rule_cookie cookie, operation_type type);
};

/**
 * The handler_registration object facilitates registration and auto
 * unregistration of an exact-match handler_base into \ref routes "routes"
 */
class handler_registration {
    routes& _routes;
    const sstring _url;
    operation_type _op;

public:
    /**
     * Registers the handler_base into routes with routes::put
     * @param rs the routes object reference
     * @param h the desire handler
     * @param url the url to match
     * @param op the operation type (`GET` by default)
     */
    handler_registration(routes& rs, handler_base& h, const sstring& url, operation_type op = GET);

    /**
     * Unregisters the handler from routes with routes::drop
     */
    ~handler_registration();
};

/**
 * The rule_registration object facilitates registration and auto
 * unregistration of a match_rule handler into \ref routes "routes"
 */
class rule_registration {
    routes& _routes;
    operation_type _op;
    routes::rule_cookie _cookie;

public:
    /**
     * Registers the match_rule into routes with routes::add_cookie
     * @param rs the routes object reference
     * @param rule a rule to add
     * @param op the operation type (`GET` by default)
     */
    rule_registration(routes& rs, match_rule& rule, operation_type op = GET);

    /**
     * Unregisters the rule from routes with routes::del_cookie
     */
    ~rule_registration();
};

SEASTAR_MODULE_EXPORT_END
}

}
