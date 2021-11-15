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

#pragma once

namespace seastar {
namespace testing {

// Disables aborting in on_internal_error() for a scope.
//
// Intended for tests, which want to test error paths that invoke
// on_internal_error() without aborting, at the same time, having it enabled
// for other, indirectly affected code paths, that are not a direct target of
// the test.
class scoped_no_abort_on_internal_error {
    bool _prev;
public:
    scoped_no_abort_on_internal_error() noexcept;
    ~scoped_no_abort_on_internal_error();
};

}
}
