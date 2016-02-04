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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include "net/api.hh"
#include <stdexcept>
#include <string>
#include <boost/any.hpp>
#include <boost/type.hpp>
#include <experimental/optional>

namespace seastar {
namespace rpc {

// used to tag a type for serializers
template<typename T>
using type = boost::type<T>;

struct stats {
    using counter_type = uint64_t;
    counter_type replied = 0;
    counter_type pending = 0;
    counter_type exception_received = 0;
    counter_type sent_messages = 0;
    counter_type wait_reply = 0;
    counter_type timeout = 0;
};


struct client_info {
    socket_address addr;
    std::unordered_map<sstring, boost::any> user_data;
    template <typename T>
    void attach_auxiliary(const sstring& key, T&& object) {
        user_data.emplace(key, boost::any(std::forward<T>(object)));
    }
    template <typename T>
    T& retrieve_auxiliary(const sstring& key) {
        auto it = user_data.find(key);
        assert(it != user_data.end());
        return boost::any_cast<T&>(it->second);
    }
    template <typename T>
    typename std::add_const<T>::type& retrieve_auxiliary(const sstring& key) const {
        return const_cast<client_info*>(this)->retrieve_auxiliary<typename std::add_const<T>::type>(key);
    }
};

class error : public std::runtime_error {
public:
    error(const std::string& msg) : std::runtime_error(msg) {}
};

class closed_error : public error {
public:
    closed_error() : error("connection is closed") {}
};

class timeout_error : public error {
public:
    timeout_error() : error("rpc call timed out") {}
};

class unknown_verb_error : public error {
public:
    uint64_t type;
    unknown_verb_error(uint64_t type_) : error("unknown verb"), type(type_) {}
};

class unknown_exception_error : public error {
public:
    unknown_exception_error() : error("unknown exception") {}
};

class rpc_protocol_error : public error {
public:
    rpc_protocol_error() : error("rpc protocol exception") {}
};

struct no_wait_type {};

// return this from a callback if client does not want to waiting for a reply
extern no_wait_type no_wait;

template <typename T>
class optional : public std::experimental::optional<T> {
public:
     using std::experimental::optional<T>::optional;
};

} // namespace rpc
} // namespace seastar
