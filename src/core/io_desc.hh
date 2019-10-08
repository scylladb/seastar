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
 * Copyright (C) 2019 ScyllaDB Ltd.
 */

#pragma once

#include <seastar/core/linux-aio.hh>
#include <exception>

namespace seastar {

class io_desc {
    promise<seastar::internal::linux_abi::io_event> _pr;
public:
    virtual ~io_desc() = default;
    virtual void set_exception(std::exception_ptr eptr) {
        _pr.set_exception(std::move(eptr));
    }

    virtual void set_value(seastar::internal::linux_abi::io_event& ev) {
        _pr.set_value(ev);
    }

    future<seastar::internal::linux_abi::io_event> get_future() {
        return _pr.get_future();
    }
};

}
