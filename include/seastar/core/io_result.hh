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
 * Copyright 2022 Jinyong Ha (jyha200@gmail.com), Heewon Shin (shw096@snu.ac.kr)
 */

#pragma once

namespace seastar {

struct io_result {
  size_t res1; // written or read bytes (same as legacy write or read command)
  ssize_t res2; // assigned block address from ZNS SSD (only for append command, otherwise -1) 
};

}
