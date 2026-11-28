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

// Tests the private socket that reactor threads use to talk to the system
// logger.  Each test stands up a system logger of its own: an AF_UNIX
// datagram socket that logger::do_log() is pointed at with
// internal::set_syslog_socket_path(), and whose receive buffer the test sizes
// to decide whether messages get through or are dropped.

#include <seastar/testing/test_case.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/log.hh>

#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

static logger test_log("syslog_test");

namespace {

// An AF_UNIX datagram socket standing in for /dev/log: it queues whatever the
// logger sends, and hands it to the test on demand.
class fake_system_logger {
    std::string _path;
    int _fd = -1;
    size_t _rcvbuf = 0;
public:
    // \param rcvbuf receive buffer to ask the kernel for; a small buffer fills
    //               up quickly, which is how the overload path is reached
    explicit fake_system_logger(size_t rcvbuf) {
        static unsigned seq = 0;
        // sockaddr_un::sun_path is short, so keep the name short too.
        _path = (std::filesystem::temp_directory_path()
                / fmt::format("sea-syslog-{}-{}", ::getpid(), seq++)).native();
        _fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        BOOST_REQUIRE_GE(_fd, 0);
        int buf = rcvbuf;
        BOOST_REQUIRE_EQUAL(::setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf)), 0);
        socklen_t buf_len = sizeof(buf);
        BOOST_REQUIRE_EQUAL(::getsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &buf, &buf_len), 0);
        _rcvbuf = buf;
        sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        BOOST_REQUIRE_LT(_path.size(), sizeof(addr.sun_path));
        std::memcpy(addr.sun_path, _path.c_str(), _path.size() + 1);
        std::filesystem::remove(_path);
        BOOST_REQUIRE_EQUAL(::bind(_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
    }
    fake_system_logger(const fake_system_logger&) = delete;
    ~fake_system_logger() {
        stop();
    }
    // Goes away, as a system logger being restarted would.
    void stop() noexcept {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
            std::filesystem::remove(_path);
        }
    }
    const std::string& path() const noexcept {
        return _path;
    }
    // The receive buffer the kernel actually gave us, which is not necessarily
    // the one we asked for.
    size_t rcvbuf() const noexcept {
        return _rcvbuf;
    }
    // Returns every message queued so far. sendmsg() queues the message before
    // it returns, so everything logged before this call is already here.
    std::vector<std::string> drain() {
        std::vector<std::string> msgs;
        // Big enough for the largest datagram the send buffer can hold.
        std::vector<char> buf(1u << 20);
        while (true) {
            auto n = ::recv(_fd, buf.data(), buf.size(), MSG_DONTWAIT);
            if (n < 0) {
                BOOST_REQUIRE_EQUAL(errno, EAGAIN);
                return msgs;
            }
            msgs.emplace_back(buf.data(), n);
        }
    }
};

// Sends log messages to \p logger, and nowhere else, for as long as it lives.
class syslog_only {
public:
    explicit syslog_only(const fake_system_logger& logger) {
        internal::set_syslog_socket_path(logger.path());
        seastar::logger::set_ostream_enabled(false);
        seastar::logger::set_syslog_enabled(true);
    }
    syslog_only(const syslog_only&) = delete;
    ~syslog_only() {
        seastar::logger::set_syslog_enabled(false);
        seastar::logger::set_ostream_enabled(true);
    }
};

// Matches an RFC 3164 record carrying \p body, as the system logger would see
// it: "<PRI>MMM dd hh:mm:ss tag[pid]: body".
void require_syslog_record(const std::string& msg, int priority, const std::string& body) {
    auto pattern = fmt::format("^<{}>[A-Z][a-z][a-z] [ 0-9][0-9] [0-9]{{2}}:[0-9]{{2}}:[0-9]{{2}} {}\\[{}\\]:{}$",
            priority, program_invocation_short_name, ::getpid(), body);
    BOOST_REQUIRE_MESSAGE(std::regex_match(msg, std::regex(pattern)),
            fmt::format("{} does not match {}", msg, pattern));
}

// The number of skipped messages reported by an overload notice, or
// std::nullopt if \p msg isn't one.
std::optional<uint64_t> skipped_count(const std::string& msg) {
    std::smatch match;
    if (!std::regex_search(msg, match, std::regex("([0-9]+) messages skipped due to syslog overload$"))) {
        return std::nullopt;
    }
    return std::stoull(match[1]);
}

constexpr int info_priority = LOG_USER | LOG_INFO;
constexpr int warning_priority = LOG_USER | LOG_WARNING;

// Small enough that a handful of messages fill it; the kernel rounds it up to
// a minimum of its own.
constexpr size_t tiny_rcvbuf = 1024;
// Large enough that nothing is dropped for want of buffer space.
constexpr size_t large_rcvbuf = 1u << 20;

} // anonymous namespace

// A logged message reaches the system logger as a well-formed RFC 3164 record.
SEASTAR_TEST_CASE(test_syslog_message_format) {
    fake_system_logger syslog(large_rcvbuf);
    auto only_syslog = syslog_only(syslog);

    test_log.info("hello");
    test_log.error("goodbye");

    auto msgs = syslog.drain();
    BOOST_REQUIRE_EQUAL(msgs.size(), 2u);
    require_syslog_record(msgs[0], info_priority, " \\[shard 0:[a-z]+\\] syslog_test - hello");
    require_syslog_record(msgs[1], LOG_USER | LOG_ERR, " \\[shard 0:[a-z]+\\] syslog_test - goodbye");
    return make_ready_future<>();
}

// A system logger that cannot keep up doesn't stall the reactor: messages are
// dropped instead, and counted, and the count is reported once it recovers.
SEASTAR_TEST_CASE(test_syslog_overload) {
    fake_system_logger syslog(tiny_rcvbuf);
    auto only_syslog = syslog_only(syslog);

    // Far more messages than the receive buffer can hold, so that some of them
    // have to be dropped.
    constexpr unsigned count = 1000;
    for (unsigned i = 0; i < count; ++i) {
        test_log.info("message {}", i);
    }
    auto delivered = syslog.drain().size();
    BOOST_REQUIRE_GT(delivered, 0u);
    BOOST_REQUIRE_LT(delivered, count);

    // The receive buffer is empty again, but sending is held off for a while
    // after it was found full.
    co_await sleep(100ms);
    test_log.info("recovered");

    auto msgs = syslog.drain();
    BOOST_REQUIRE_EQUAL(msgs.size(), 2u);
    // The notice comes first, so that it precedes the messages that survived
    // the gap it describes.
    auto skipped = skipped_count(msgs[0]);
    BOOST_REQUIRE(skipped.has_value());
    BOOST_REQUIRE_EQUAL(*skipped, count - delivered);
    require_syslog_record(msgs[0], warning_priority,
            fmt::format(" \\[shard 0\\] syslog - WARN: {} messages skipped due to syslog overload", *skipped));
    require_syslog_record(msgs[1], info_priority, " \\[shard 0:[a-z]+\\] syslog_test - recovered");

    // Having reported the gap, we're back to reporting nothing.
    test_log.info("still here");
    msgs = syslog.drain();
    BOOST_REQUIRE_EQUAL(msgs.size(), 1u);
    require_syslog_record(msgs[0], info_priority, " \\[shard 0:[a-z]+\\] syslog_test - still here");
}

// A system logger that goes away and comes back gets a working connection
// again, without any help from the application.
SEASTAR_TEST_CASE(test_syslog_reconnect) {
    auto syslog = std::make_unique<fake_system_logger>(large_rcvbuf);
    auto only_syslog = syslog_only(*syslog);
    auto path = syslog->path();

    test_log.info("before");
    BOOST_REQUIRE_EQUAL(syslog->drain().size(), 1u);

    // The system logger restarts, losing whatever was logged while it was down.
    syslog->stop();
    test_log.info("during");
    syslog = std::make_unique<fake_system_logger>(large_rcvbuf);
    // Point the logger at the new socket. A real system logger would come back
    // at the same path; a test can't, because it doesn't own /dev/log.
    internal::set_syslog_socket_path(syslog->path());
    // Connecting is retried at a bounded rate, so give the socket time to come
    // back before expecting it to work.
    co_await sleep(1200ms);

    test_log.info("after");
    auto msgs = syslog->drain();
    BOOST_REQUIRE_EQUAL(msgs.size(), 1u);
    require_syslog_record(msgs[0], info_priority, " \\[shard 0:[a-z]+\\] syslog_test - after");
}

// A message too large for a single datagram is truncated rather than lost, and
// doesn't leave the socket unusable.
SEASTAR_TEST_CASE(test_syslog_oversized_message) {
    fake_system_logger syslog(large_rcvbuf);
    auto only_syslog = syslog_only(syslog);

    // A datagram is limited by the send buffer, which is a good deal larger
    // than the receive buffer we can ask for, so the truncated message may
    // still be too large to be queued. Either way, the message that follows it
    // has to get through.
    test_log.info("{}", std::string(2 * syslog.rcvbuf(), 'x'));
    for (auto& msg : syslog.drain()) {
        if (skipped_count(msg)) {
            continue;
        }
        BOOST_REQUIRE(msg.ends_with("..."));
    }

    co_await sleep(100ms);
    test_log.info("intact");
    auto msgs = syslog.drain();
    BOOST_REQUIRE(!msgs.empty());
    require_syslog_record(msgs.back(), info_priority, " \\[shard 0:[a-z]+\\] syslog_test - intact");
}
