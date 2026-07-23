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
// individual <fmt/*.h> headers.  When SEASTAR_IMPORT_FMT is defined (see the
// Seastar_IMPORT_FMT option in CMakeLists.txt) this imports fmt as a C++20
// module; otherwise it is a plain textual include of every fmt header Seastar
// uses.  Consumers must pick one per translation unit -- mixing `import fmt;`
// with a textual <fmt/*.h> in the same TU is a redefinition error -- which is
// exactly why every Seastar fmt include is funnelled through here.

#ifdef SEASTAR_IMPORT_FMT

// fmt's preprocessor macros are not part of the module, so the ones that
// Seastar's public headers rely on must be provided out of band.  The only
// such macro left is FMT_VERSION, supplied by the build and derived from the
// fmt that was found (Seastar does not bundle fmt, so it cannot be hardcoded
// here).
#ifndef FMT_VERSION
#error "SEASTAR_IMPORT_FMT requires the build to define FMT_VERSION (see Seastar_IMPORT_FMT)"
#endif

import fmt;

#else

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/compile.h>
#include <fmt/core.h>

#endif
