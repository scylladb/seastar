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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <cstddef>
#endif
#include <seastar/util/modules.hh>

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

constexpr size_t KB = 1 << 10;
constexpr size_t MB = 1 << 20;
constexpr size_t GB = 1 << 30;

constexpr size_t operator""_KiB(unsigned long long n) { return n << 10; }
constexpr size_t operator""_MiB(unsigned long long n) { return n << 20; }
constexpr size_t operator""_GiB(unsigned long long n) { return n << 30; }
constexpr size_t operator""_TiB(unsigned long long n) { return n << 40; }

SEASTAR_MODULE_EXPORT_END
}
