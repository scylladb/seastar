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
 * Copyright 2019 ScyllaDB
 */

#pragma once

namespace seastar {

struct pollfn {
    virtual ~pollfn() {}
    // Returns true if work was done (false = idle)
    virtual bool poll() = 0;
    // Checks if work needs to be done, but without actually doing any
    // returns true if works needs to be done (false = idle)
    virtual bool pure_poll() = 0;
    // Tries to enter interrupt mode.
    //
    // If it returns true, then events from this poller will wake
    // a sleeping idle loop, and exit_interrupt_mode() must be called
    // to return to normal polling.
    //
    // If it returns false, the sleeping idle loop may not be entered.
    virtual bool try_enter_interrupt_mode() = 0;
    virtual void exit_interrupt_mode() = 0;
};

// The common case for poller -- do not make any difference between
// poll() and pure_poll(), always/never agree to go to sleep and do
// nothing on wakeup.
template <bool Passive>
struct simple_pollfn : public pollfn {
    virtual bool pure_poll() override final {
        return poll();
    }
    virtual bool try_enter_interrupt_mode() override final {
        return Passive;
    }
    virtual void exit_interrupt_mode() override final {
    }
};

}
