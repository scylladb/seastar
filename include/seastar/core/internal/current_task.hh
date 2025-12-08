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
 * Copyright 2025 ScyllaDB
 */

#pragma once

namespace seastar::internal {

// If a task wants to resume a different task instead of returning control to the reactor,
// it should set _current_task to the resumed task.
// In particular, this is mandatory if the task is going to die before control is returned
// to the reactor -- otherwise _current_task will be left dangling.
void set_current_task(task* t);

} // namespace seastar::internal
