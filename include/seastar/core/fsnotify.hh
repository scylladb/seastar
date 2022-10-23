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
 * Copyright 2020 ScyllaDB Ltd.
 */

#pragma once

#include <memory>
#include <sys/inotify.h>

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/shared_ptr.hh>

namespace seastar {

/**
 * Thin wrapper around inotify. (See http://man7.org/linux/man-pages/man7/inotify.7.html)
 * De-facto light-weight filesystem modification watch interface. 
 * Allows adding watches to files or directories for various modification 
 * events (see fsnotifier::flags). 
 * 
 * Presents a c++ wrapped buffered read of events, and
 * raii-handling of watches themselves. 
 * 
 * All definition are bit-matched with inotify, 
 * but watch points are raii guarded. 
 * 
 * Note that this impl does not (yet) handle
 * re-writing watches (adding to mask).
 * 
 */
class fsnotifier {
    class impl;
    shared_ptr<impl> _impl;
public:
    class watch;
    friend class watch;

    enum class flags : uint32_t {
        access = IN_ACCESS,             // File was accessed (e.g., read(2), execve(2)).
        attrib = IN_ATTRIB,             // Metadata changed—for example, permissions, timestamps, extended attributes
        close_write = IN_CLOSE_WRITE,   // File opened for writing was closed.
        close_nowrite = IN_CLOSE_NOWRITE,// File or directory not opened for writing was closed.
        create_child = IN_CREATE,       // File/directory created in watched directory 
        delete_child = IN_DELETE,       // File/directory deleted from watched directory.
        delete_self = IN_DELETE_SELF,   // Watched file/directory was itself deleted.  (This event
                                        // also occurs if an object is moved to another filesystem)
        modify = IN_MODIFY,             // File was modified (e.g., write(2), truncate(2)).
        move_self = IN_MOVE_SELF,       // Watched file/directory was itself moved.
        move_from = IN_MOVED_FROM,      // Generated for the directory containing the old filename
                                        // when a file is renamed.
        move_to = IN_MOVED_TO,          // Generated for the directory containing the new filename
                                        // when a file is renamed.
        open = IN_OPEN,                 // File was opened
        close = IN_CLOSE,               // close_write|close_nowrite
        move = IN_MOVE,                 // move_from|move_to
        oneshot = IN_ONESHOT,           // listen for only a single notification, after which the 
                                        // token will be invalid
        ignored = IN_IGNORED,           // generated when a token or the file being watched is deleted 
        onlydir = IN_ONLYDIR,           // Watch pathname only if it is a directory; the error ENOT‐
                                        // DIR results if pathname is not a directory.  Using this
                                        // flag provides an application with a race-free way of
                                        // ensuring that the monitored object is a directory.
    };

    using watch_token = int32_t;
    using sequence_no = uint32_t;

    /**
     * Simple raii-wrapper around a watch token
     * - i.e. watch identifier
     */
    class watch {
    public:
        ~watch();
        watch(watch&&) noexcept;
        watch& operator=(watch&&) noexcept;

        watch_token release();
        operator watch_token() const {
            return _token;
        }
        watch_token token() const {
            return _token;
        }
    private:
        friend class fsnotifier;
        watch(shared_ptr<impl>, watch_token);
        watch_token _token;
        shared_ptr<impl> _impl;
    };

    fsnotifier();
    ~fsnotifier();

    fsnotifier(fsnotifier&&);
    fsnotifier& operator=(fsnotifier&&);

    // create a watch point for given path, checking/producing 
    // events specified in mask 
    future<watch> create_watch(const sstring& path, flags mask);

    // a watch event. 
    struct event {
        // matches source watch
        watch_token id;
        // event(s) generated
        flags mask;
        sequence_no seq; // event correlation -> move_from+move_to
        sstring name; // optional file name, in case of move_from/to
    };

    // wait for events
    future<std::vector<event>> wait() const;

    // shutdown notifier and abort any event wait.
    // all watches are invalidated, and no new ones can be
    // created.
    void shutdown();

    bool active() const;

    operator bool() const {
        return active();
    }
};

inline fsnotifier::flags operator|(fsnotifier::flags a, fsnotifier::flags b) {
    return fsnotifier::flags(std::underlying_type_t<fsnotifier::flags>(a) | std::underlying_type_t<fsnotifier::flags>(b));
}

inline void operator|=(fsnotifier::flags& a, fsnotifier::flags b) {
    a = (a | b);
}

inline fsnotifier::flags operator&(fsnotifier::flags a, fsnotifier::flags b) {
    return fsnotifier::flags(std::underlying_type_t<fsnotifier::flags>(a) & std::underlying_type_t<fsnotifier::flags>(b));
}

inline void operator&=(fsnotifier::flags& a, fsnotifier::flags b) {
    a = (a & b);
}

}