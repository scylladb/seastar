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
 * Copyright (C) 2018 Scylladb, Ltd.
 */

#include <cstddef>

extern "C" {
void __sanitizer_start_switch_fiber(void**, const void*, size_t);
void __sanitizer_finish_switch_fiber(void*, const void**, size_t*);
}

int main() {
  __sanitizer_start_switch_fiber(nullptr, nullptr, 0);
  __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
}
