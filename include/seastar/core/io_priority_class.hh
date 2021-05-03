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
 * Copyright 2021 ScyllaDB
 */

#pragma once

namespace seastar {

/// \cond internal
class io_queue;
using io_priority_class_id = unsigned;
class io_priority_class {
    io_priority_class_id _id;

    io_priority_class() = delete;
    explicit io_priority_class(io_priority_class_id id) noexcept
        : _id(id)
    { }

    bool rename_registered(sstring name);

public:
    io_priority_class_id id() const noexcept {
        return _id;
    }

    static io_priority_class register_one(sstring name, uint32_t shares);

    /// \brief Updates the current amount of shares for a given priority class
    ///
    /// \param pc the priority class handle
    /// \param shares the new shares value
    /// \return a future that is ready when the share update is applied
    future<> update_shares(uint32_t shares);

    /// Renames an io priority class
    ///
    /// Renames an `io_priority_class` previously created with register_one_priority_class().
    ///
    /// The operation is global and affects all shards.
    /// The operation affects the exported statistics labels.
    ///
    /// \param pc The io priority class to be renamed
    /// \param new_name The new name for the io priority class
    /// \return a future that is ready when the io priority class have been renamed
    future<> rename(sstring new_name) noexcept;

    unsigned get_shares() const;
    sstring get_name() const;

private:
    static constexpr unsigned _max_classes = 2048;
    static std::mutex _register_lock;
    static std::array<uint32_t, _max_classes> _registered_shares;
    static std::array<sstring, _max_classes> _registered_names;
};

const io_priority_class& default_priority_class();

} // namespace seastar
