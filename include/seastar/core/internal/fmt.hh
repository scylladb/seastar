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
 * Copyright (C) 2026 ScyllaDB
 */

#pragma once

// Single point through which Seastar pulls in {fmt}.
//
// All Seastar code that needs {fmt} includes this header instead of the
// individual <fmt/*.h> headers.  For now it is a plain textual include of
// every fmt header Seastar uses; routing everything through here lets a
// later change switch to `import fmt;` in one place.
//
// It also defines SEASTAR_FMT_VERSION, which the version-dependent parts of
// Seastar's public headers key off instead of fmt's own FMT_VERSION.  The
// build may define it (as an integer, MMmmpp, like FMT_VERSION) for setups
// where fmt does not supply it; otherwise it is taken from fmt.

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/compile.h>
#include <fmt/core.h>

#ifndef SEASTAR_FMT_VERSION
#define SEASTAR_FMT_VERSION FMT_VERSION
#endif
