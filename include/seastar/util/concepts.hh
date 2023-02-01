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
 * Copyright (C) 2020 ScyllaDB
 */
#pragma once

#if __has_include(<concepts>)
#include <concepts>
#endif

#if defined(__cpp_concepts) && __cpp_concepts >= 201907 && \
    defined(__cpp_lib_concepts) && __cpp_lib_concepts >= 201907

#define SEASTAR_CONCEPT(x...) x
#define SEASTAR_NO_CONCEPT(x...)

#else

#define SEASTAR_CONCEPT(x...)
#define SEASTAR_NO_CONCEPT(x...) x

#endif

