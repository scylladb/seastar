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

#ifndef SEASTAR_MODULE
#include <sys/inotify.h>

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/util/modules.hh>
#endif

namespace seastar::experimental {

SEASTAR_MODULE_EXPORT_BEGIN

/// \defgroup fsnotifier FileSystem Notifier
///
/// Seastar provides an API which can be used to monitor filesystem modifications.
///
/// \addtogroup fsnotifier
/// @{

/// \brief Filesystem modification notifier.
///
/// This is a thin wrapper around inotify(see http://man7.org/linux/man-pages/man7/inotify.7.html),
/// which is the de-facto light-weight filesystem modification watch
/// interface in Linux and can be used to log filesystem activities,
/// reload configurations, etc.
///
/// The wrapper provides a buffered read of events, and RAII handling
/// of watches themselves.
///
/// \note Note that this is an experimental feature, and thus any tweaks that
///       are backward incompatible can be made before finally freezing it.
///       Besides, the impl currently does not (yet) handle re-writing watches.
class fsnotifier {
    class impl;
    shared_ptr<impl> _impl;
public:
    class watch;
    friend class watch;

    /// \brief Flags of events supported by \ref fsnotifier.
    ///
    /// \note Note that the flags are bit-matched with inotify and they can
    ///       be calculated using the bitwise AND and bitwise OR operators
    ///       (including the assignment form) directly to take intersection
    ///       or union.
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

    /// \brief Token of a watch point.
    using watch_token = int32_t;
    /// \brief Unique sequence number of associating related events.
    ///
    /// \note The sequence number is used to connect related events.
    ///       Currently, it is used only for the rename events(i.e.,
    ///       move_from and move_to), and is 0 for other types.
    using sequence_no = uint32_t;

    /// \brief Simple RAII wrapper around a \ref fsnotifier::watch_token
    ///
    /// The events of the path will be unregistered automatically on
    /// destruction of the \ref watch.
    class watch {
    public:
        ~watch();
        watch(watch&&) noexcept;
        watch& operator=(watch&&) noexcept;

        /// Reset the watch point.
        ///
        /// \note Note that this operation won't unregister the event for
        ///       the path, but simply releases resources used internally.
        watch_token release();

        /// Cast this watch point to a watch token.
        operator watch_token() const {
            return _token;
        }

        /// Get the token of this watch point.
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

    /// \brief Monitor events specified in mask for the give path.
    ///
    /// \param path path of the file or directory to monitor for.
    /// \param mask events of interest.
    /// \return a future that becomes ready when the underlying
    ///         inotify_add_watch(2) call completes.
    future<watch> create_watch(const sstring& path, flags mask);

    /// \brief A wrapper around inotify_event.
    struct event {
        // matches source watch
        watch_token id;
        // event(s) generated
        flags mask;
        sequence_no seq; // event correlation -> move_from+move_to
        sstring name; // optional file name, in case of move_from/to
    };

    /// Wait for events.
    ///
    /// \return a future that becomes ready when registered events occur.
    future<std::vector<event>> wait() const;

    /// Shutdown the notifier and abort any waiting events.
    ///
    /// \note After shutdown, all watches are invalidated,
    ///       and no new ones can be created.
    void shutdown();

    /// Check if the notifier is activated.
    bool active() const;

    /// Equivalent to \ref active().
    operator bool() const {
        return active();
    }
};

/// Take the union of two events.
inline fsnotifier::flags operator|(fsnotifier::flags a, fsnotifier::flags b) {
    return fsnotifier::flags(std::underlying_type_t<fsnotifier::flags>(a) | std::underlying_type_t<fsnotifier::flags>(b));
}

/// Take the union of two events, assignment form.
inline void operator|=(fsnotifier::flags& a, fsnotifier::flags b) {
    a = (a | b);
}

/// Take the intersection of two events.
inline fsnotifier::flags operator&(fsnotifier::flags a, fsnotifier::flags b) {
    return fsnotifier::flags(std::underlying_type_t<fsnotifier::flags>(a) & std::underlying_type_t<fsnotifier::flags>(b));
}

/// Take the intersection of two events, assignment form.
inline void operator&=(fsnotifier::flags& a, fsnotifier::flags b) {
    a = (a & b);
}

/// @}

SEASTAR_MODULE_EXPORT_END

}
