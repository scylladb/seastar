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
 * Copyright 2016 Cloudius Systems
 */

#include <arpa/nameser.h>
#include <chrono>

#include <ares.h>
#include <boost/lexical_cast.hpp>

#include <ostream>
#include <seastar/util/std-compat.hh>
#include <seastar/net/inet_address.hh>

namespace seastar {

// NOTE: Should be prior to <seastar/util/log.hh> include because
// logger::stringer_for<T> needs to see the corresponding `operator <<`
// declaration at the call site
//
// This doesn't need to be in the public API, so leave it there instead of placing into `inet_address.hh`
std::ostream& operator<<(std::ostream& os, const compat::optional<net::inet_address::family>& f) {
    if (f) {
        return os << *f;
    } else {
        return os << "ANY";
    }
}

}

#include <seastar/net/ip.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/print.hh>
#include <seastar/util/log.hh>

namespace seastar {

static logger dns_log("dns_resolver");

class ares_error_category : public std::error_category {
public:
    constexpr ares_error_category() noexcept : std::error_category{} {}
    const char * name() const noexcept {
        return "C-Ares";
    }
    std::string message(int error) const {
        switch (error) {
            /* Server error codes (ARES_ENODATA indicates no relevant answer) */
            case ARES_ENODATA: return "No data";
            case ARES_EFORMERR: return "Form error";
            case ARES_ESERVFAIL: return "Server failure";
            case ARES_ENOTFOUND: return "Not found";
            case ARES_ENOTIMP: return "Not implemented";
            case ARES_EREFUSED: return "Refused";

            /* Locally generated error codes */
            case ARES_EBADQUERY: return "Bad query";
            case ARES_EBADNAME: return "Bad name";
            case ARES_EBADFAMILY: return "Bad family";
            case ARES_EBADRESP: return "Bad response";
            case ARES_ECONNREFUSED :return "Connection refused";
            case ARES_ETIMEOUT: return "Timeout";
            case ARES_EOF: return "EOF";
            case ARES_EFILE: return "File error";
            case ARES_ENOMEM: return "No memory";
            case ARES_EDESTRUCTION: return "Destruction";
            case ARES_EBADSTR: return "Bad string";

            /* ares_getnameinfo error codes */
            case ARES_EBADFLAGS: return "Invalid flags";

            /* ares_getaddrinfo error codes */
            case ARES_ENONAME: return "No name";
            case ARES_EBADHINTS: return "Bad hints";

            /* Uninitialized library error code */
            case ARES_ENOTINITIALIZED: return "Not initialized";

            /* ares_library_init error codes */
            case ARES_ELOADIPHLPAPI: return "Load PHLPAPI";
            case ARES_EADDRGETNETWORKPARAMS: return "Get network parameters";

            /* More error codes */
            case ARES_ECANCELLED: return "Cancelled";
            default:
            return "Unknown error";
        }
    }
};

static const ares_error_category ares_errorc;

static void check_ares_error(int error) {
    if (error != ARES_SUCCESS) {
        throw std::system_error(error, ares_errorc);
    }
}

struct ares_initializer {
    ares_initializer() {
        check_ares_error(ares_library_init(0));
    }
    ~ares_initializer() {
        ares_library_cleanup();
    }
};

class net::dns_resolver::impl
    : public enable_shared_from_this<impl>
{
public:
    impl(network_stack& stack, const options& opts)
        : _stack(stack)
        , _timeout(opts.timeout ? *opts.timeout : std::chrono::milliseconds(5000) /* from ares private */)
        , _timer(std::bind(&impl::poll_sockets, this))
    {
        static const ares_initializer a_init;

        // this can "block" ever so slightly, because it will
        // look in resolv.conf etc for query setup. We could
        // do this ourselves, and instead set ares options
        // here, but it seems more error prone (me parsing
        // resolv.conf -> hah!)
        ares_options a_opts = { 0, };

        // For now, use the default "fb" query order
        // (set explicitly lest we forget).
        // We only do querying dns server really async.
        // Reading hosts files is doen by c-ares internally
        // and with normal fread calls. Thus they theorectically
        // block. This can potentially be an issue for some application
        // and if so, we need to revisit this. For now, assume
        // it won't block us in any measurable way.
        char buf[3] = "fb";
        a_opts.lookups = buf; // only net
        // Always set the timeout
        a_opts.timeout = _timeout.count();
        int flags = ARES_OPT_LOOKUPS|ARES_OPT_TIMEOUTMS;

        if (opts.use_tcp_query && *opts.use_tcp_query) {
            a_opts.flags = ARES_FLAG_USEVC | ARES_FLAG_PRIMARY;
            flags |= ARES_OPT_FLAGS;
        }
        std::vector<in_addr> addr_tmp;
        if (opts.servers) {
            std::transform(opts.servers->begin(), opts.servers->end(), std::back_inserter(addr_tmp), [](const inet_address& a) {
                if (a.in_family() != inet_address::family::INET) {
                    throw std::invalid_argument("Servers must be ipv4 addresses");
                }
                in_addr in = a;
                return in;
            });
            a_opts.servers = addr_tmp.data();
            a_opts.nservers = int(addr_tmp.size());
            flags |= ARES_OPT_SERVERS;
        }
        std::vector<const char *> dom_tmp;
        if (opts.domains) {
            std::transform(opts.domains->begin(), opts.domains->end(), std::back_inserter(dom_tmp), [](const sstring& s) {
                return s.data();
            });
            a_opts.domains = const_cast<char **>(dom_tmp.data());
            a_opts.ndomains = int(dom_tmp.size());
            flags |= ARES_OPT_DOMAINS;
        }
        if (opts.tcp_port) {
            a_opts.tcp_port = *opts.tcp_port;
            flags |= ARES_OPT_TCP_PORT;
        }
        if (opts.udp_port) {
            a_opts.udp_port = *opts.udp_port;
            flags |= ARES_OPT_UDP_PORT;
        }

        check_ares_error(ares_init_options(&_channel, &a_opts, flags));

        static auto get_impl = [](void * p) { return reinterpret_cast<impl *>(p); };
        static const ares_socket_functions callbacks = {
            [](int af, int type, int protocol, void * p) { return get_impl(p)->do_socket(af, type, protocol); },
            [](ares_socket_t s, void * p) { return get_impl(p)->do_close(s); },
            [](ares_socket_t s, const struct sockaddr * addr, socklen_t len, void * p) { return get_impl(p)->do_connect(s, addr, len); },
            [](ares_socket_t s, void * dst, size_t len, int flags, struct sockaddr * addr, socklen_t * alen, void * p) {
                return get_impl(p)->do_recvfrom(s, dst, len, flags, addr, alen);
            },
            [](ares_socket_t s, const struct iovec * vec, int len, void * p) {
                return get_impl(p)->do_sendv(s, vec, len);
            },
        };

        ares_set_socket_functions(_channel, &callbacks, this);

        // just in case you need printf-debug.
        // dns_log.set_level(log_level::trace);
    }
    ~impl() {
        _timer.cancel();
        if (_channel) {
            ares_destroy(_channel);
        }
    }

    future<inet_address> resolve_name(sstring name, opt_family family) {
        return get_host_by_name(std::move(name), family).then([](hostent h) {
            return make_ready_future<inet_address>(h.addr_list.front());
        });
    }

    future<hostent> get_host_by_name(sstring name, opt_family family)  {
        class promise_wrap : public promise<hostent> {
        public:
            promise_wrap(sstring s)
                : name(std::move(s))
            {}
            sstring name;
        };

        dns_log.debug("Query name {} ({})", name, family);

        if (!family) {
            ::in_addr in;
            ::in6_addr in6;
            if (::inet_pton(AF_INET, name.c_str(), &in)) {
                return make_ready_future<hostent>(hostent{ {name}, {net::inet_address(in)}});
            }
            if (::inet_pton(AF_INET6, name.c_str(), &in6)) {
                return make_ready_future<hostent>(hostent{ {name}, {net::inet_address(in6)}});
            }
        }

        auto p = new promise_wrap(std::move(name));
        auto f = p->get_future();

        dns_call call(*this);

        auto af = family ? int(*family) : AF_UNSPEC;

        ares_gethostbyname(_channel, p->name.c_str(), af, [](void* arg, int status, int timeouts, ::hostent* host) {
            // we do potentially allocating operations below, so wrap the pointer in a
            // unique here.
            std::unique_ptr<promise_wrap> p(reinterpret_cast<promise_wrap *>(arg));

            switch (status) {
            default:
                dns_log.debug("Query failed: {}", status);
                p->set_exception(std::system_error(status, ares_errorc, p->name));
                break;
            case ARES_SUCCESS:
                p->set_value(make_hostent(*host));
                break;
            }

        }, reinterpret_cast<void *>(p));


        poll_sockets();

        return f.finally([this] {
            end_call();
        });
    }

    future<hostent> get_host_by_addr(inet_address addr) {
        class promise_wrap : public promise<hostent> {
        public:
            promise_wrap(inet_address a)
                : addr(std::move(a))
            {}
            inet_address addr;
        };

        dns_log.debug("Query addr {}", addr);

        auto p = new promise_wrap(std::move(addr));
        auto f = p->get_future();

        dns_call call(*this);

        ares_gethostbyaddr(_channel, p->addr.data(), p->addr.size(), int(p->addr.in_family()), [](void* arg, int status, int timeouts, ::hostent* host) {
            // we do potentially allocating operations below, so wrap the pointer in a
            // unique here.
            std::unique_ptr<promise_wrap> p(reinterpret_cast<promise_wrap *>(arg));

            switch (status) {
            default:
                dns_log.debug("Query failed: {}", status);
                p->set_exception(std::system_error(status, ares_errorc, boost::lexical_cast<std::string>(p->addr)));
                break;
            case ARES_SUCCESS:
                p->set_value(make_hostent(*host));
                break;
            }

        }, reinterpret_cast<void *>(p));


        poll_sockets();

        return f.finally([this] {
            end_call();
        });
    }

    future<srv_records> get_srv_records(srv_proto proto,
                                        const sstring& service,
                                        const sstring& domain) {
        auto p = std::make_unique<promise<srv_records>>();
        auto f = p->get_future();

        const auto query = format("_{}._{}.{}",
                                  service,
                                  proto == srv_proto::tcp ? "tcp" : "udp",
                                  domain);

        dns_log.debug("Query srv {}", query);

        dns_call call(*this);

        ares_query(_channel, query.c_str(), ns_c_in, ns_t_srv,
                   [](void* arg, int status, int timeouts,
                      unsigned char* buf, int len) {
            auto p = std::unique_ptr<promise<srv_records>>(
                reinterpret_cast<promise<srv_records> *>(arg));
            if (status != ARES_SUCCESS) {
                dns_log.debug("Query failed: {}", status);
                p->set_exception(std::system_error(status, ares_errorc));
                return;
            }
            ares_srv_reply* start = nullptr;
            status = ares_parse_srv_reply(buf, len, &start);
            if (status != ARES_SUCCESS) {
                dns_log.debug("Parse failed: {}", status);
                p->set_exception(std::system_error(status, ares_errorc));
                return;
            }
            try {
                p->set_value(make_srv_records(start));
            } catch (...) {
                p->set_exception(std::current_exception());
            }
            ares_free_data(start);
        }, reinterpret_cast<void *>(p.release()));


        poll_sockets();

        return f.finally([this] {
            end_call();
        });
    }

    future<sstring> resolve_addr(inet_address addr) {
        return get_host_by_addr(addr).then([](hostent h) {
            return make_ready_future<sstring>(h.names.front());
        });
    }

    future<> close() {
        _closed = true;
        ares_cancel(_channel);
        dns_log.trace("Shutting down {} sockets", _sockets.size());
        for (auto & p : _sockets) {
            do_close(p.first);
        }
        dns_log.trace("Closing gate");
        return _gate.close();
    }
private:
    enum class type {
        none, tcp, udp
    };
    struct dns_call {
        dns_call(impl & i)
            : _i(i)
            , _c(++i._calls)
        {}
        ~dns_call() {
            // If a query does not immediately complete
            // it might never do so, unless data actually
            // comes back to us and a waiting recv promise
            // is fulfilled.
            // We need to add a timer to do polling at ~timeout
            // ms later, so the ares logic can detect this and
            // tell us we're over.
            if (_c == 1 && _i._calls != 0) {
                _i._timer.arm_periodic(_i._timeout);
            }
        }
        impl& _i;
        uint64_t _c;
    };

    void end_call() {
        if (--_calls == 0) {
            _timer.cancel();
        }
    }
    void poll_sockets() {
        fd_set readers, writers;
        int n = 0;

        dns_log.trace("Poll sockets");

        do {
            // Retrieve the set of file descriptors that the library wants us to monitor.
            FD_ZERO(&readers);
            FD_ZERO(&writers);

            n = ares_fds(_channel, &readers, &writers);

            dns_log.trace("ares_fds: {}", n);

            if (n == 0) {
                break;
            }

            n = 0;

            for (auto & p : _sockets) {
                auto & e = p.second;
                auto fd = p.first;
                auto r = FD_ISSET(p.first, &readers);
                auto w = FD_ISSET(p.first, &writers);
                auto ra = e.avail & POLLIN;
                auto wa = e.avail & POLLOUT;

                dns_log.trace("fd {} {}{}/{}{}", fd, (r ? "r" : ""),
                                (w ? "w" : ""), (ra ? "r" : ""),
                                (wa ? "w" : ""));

                if (!wa) {
                    FD_CLR(fd, &writers);
                }
                if (!ra) {
                    FD_CLR(fd, &readers);
                }
                if (FD_ISSET(fd, &writers) || FD_ISSET(fd, &readers)) {
                    ++n;
                }
            }

            ares_process(_channel, &readers, &writers);
        } while (n != 0);
    }

    static srv_records make_srv_records(ares_srv_reply* start) {
        srv_records records;
        for (auto reply = start; reply; reply = reply->next) {
            srv_record record = {reply->priority,
                                 reply->weight,
                                 reply->port,
                                 sstring{reply->host}};
            records.push_back(std::move(record));
        }
        return records;
    }

    static hostent make_hostent(const ::hostent& host) {
        hostent e;
        e.names.emplace_back(host.h_name);
        auto np = host.h_aliases;
        while (*np != 0) {
            e.names.emplace_back(*np++);
        }
        auto p = host.h_addr_list;
        while (*p != nullptr) {
            switch (host.h_addrtype) {
            case AF_INET:
                assert(size_t(host.h_length) >= sizeof(in_addr));
                e.addr_list.emplace_back(*reinterpret_cast<const in_addr*>(*p));
                break;
            case AF_INET6:
                assert(size_t(host.h_length) >= sizeof(in6_addr));
                e.addr_list.emplace_back(*reinterpret_cast<const in6_addr*>(*p));
                break;
            default:
                break;
            }
            ++p;
        }

        dns_log.debug("Query success: {}/{}", e.names.front(), e.addr_list.front());

        return e;
    }
    // We need to partially ref-count our socket entries
    // when we have pending reads/writes, so we don't erase the
    // entry to early.
    void use(ares_socket_t fd) {
        _gate.enter();
        auto& e = _sockets.at(fd);
        ++e.pending;
    }
    void release(ares_socket_t fd) {
        auto& e = _sockets.at(fd);
        dns_log.trace("Release socket {} -> {}", fd, e.pending -  1);
        if (--e.pending < 0) {
            _sockets.erase(fd);
            dns_log.trace("Released socket {}", fd);
        }
        _gate.leave();
    }
    ares_socket_t do_socket(int af, int type, int protocol) {
        if (_closed) {
            return -1;
        }
        int fd = next_fd();
        switch (type) {
        case SOCK_STREAM:
            _sockets.emplace(fd, connected_socket());
            dns_log.trace("Created tcp socket {}", fd);
            break;
        case SOCK_DGRAM:
            _sockets.emplace(fd, _stack.make_udp_channel());
            dns_log.trace("Created udp socket {}", fd);
            break;
        default: return -1;
        }
        return fd;
    }
    int do_close(ares_socket_t fd) {
        dns_log.trace("Close socket {}", fd);
        auto& e = _sockets.at(fd);

        // Mark as closed.
        if (std::exchange(e.closed, true)) {
            return 0;
        }

        _gate.enter(); // "leave" is done in release(fd)

        switch (e.typ) {
        case type::tcp:
        {
            dns_log.trace("Close tcp socket {}, {} pending", fd, e.pending);
            future<> f = make_ready_future();
            if (e.tcp.in) {
                e.tcp.socket.shutdown_input();
                dns_log.trace("Closed tcp socket {} input", fd);
            }
            if (e.tcp.out) {
                f = f.then([&e] {
                    return e.tcp.out->close();
                }).then([fd] {
                    dns_log.trace("Closed tcp socket {} output", fd);
                });
            }
            f = f.finally([me = shared_from_this(), fd] {
                me->release(fd);
            });
            break;
        }
        case type::udp:
            e.udp.channel.shutdown_input();
            e.udp.channel.shutdown_output();
            release(fd);
            break;
        default:
            // should not happen
            _gate.leave();
            break;
        }
        return 0;
    }
    socket_address sock_addr(const sockaddr * addr, socklen_t len) {
        if (addr->sa_family != AF_INET) {
            throw std::invalid_argument("No ipv6 yet");
        }
        auto in = reinterpret_cast<const sockaddr_in *>(addr);
        return *in;
    }
    int do_connect(ares_socket_t fd, const sockaddr * addr, socklen_t len) {
        if (_closed) {
            return -1;
        }
        try {
            auto& e = get_socket_entry(fd);
            auto sa = sock_addr(addr, len);

            dns_log.trace("Connect {}({})->{}", fd, int(e.typ), sa);

            assert(e.avail == 0);

            e.avail = POLLOUT|POLLIN; // until we know otherwise

            switch (e.typ) {
            case type::tcp: {
                auto f = _stack.connect(sa);
                if (!f.available()) {
                    dns_log.trace("Connection pending: {}", fd);
                    e.avail = 0;
                    use(fd);
                    f.then_wrapped([me = shared_from_this(), &e, fd](future<connected_socket> f) {
                        try {
                            e.tcp.socket = f.get0();
                            dns_log.trace("Connection complete: {}", fd);
                        } catch (...) {
                            dns_log.debug("Connect {} failed: {}", fd, std::current_exception());
                        }
                        e.avail = POLLOUT|POLLIN;
                        me->poll_sockets();
                        me->release(fd);
                    });
                    errno = EWOULDBLOCK;
                    return -1;
                }
                e.tcp.socket = f.get0();
                break;
            }
            case type::udp:
                // we do not have udp connect, so just keep
                // track of the destination
                e.udp.dst = sa;
                break;
            default:
                return -1;
            }
            return 0;
        } catch (...) {
            return -1;
        }
    }
    ssize_t do_recvfrom(ares_socket_t fd, void * dst, size_t len, int flags, struct sockaddr * from, socklen_t * from_len) {
        if (_closed) {
            return -1;
        }
        try {
            auto& e = get_socket_entry(fd);
            dns_log.trace("Read {}({})", fd, int(e.typ));
            // check if we're already reading.
            if (!(e.avail & POLLIN)) {
                dns_log.trace("Read already pending {}", fd);
                errno = EWOULDBLOCK;
                return -1;
            }
            for (;;) {
                switch (e.typ) {
                case type::tcp: {
                    auto & tcp = e.tcp;
                    if (!tcp.indata.empty()) {
                        dns_log.trace("Read {}. {} bytes available", fd, tcp.indata.size());
                        len = std::min(len, tcp.indata.size());
                        std::copy(tcp.indata.begin(), tcp.indata.begin() + len, reinterpret_cast<char *>(dst));
                        tcp.indata.trim_front(len);
                        return len;
                    }
                    if (!tcp.in) {
                        tcp.in = tcp.socket.input();
                    }
                    auto f = tcp.in->read_up_to(len);
                    if (!f.available()) {
                        dns_log.trace("Read {}: data unavailable", fd);
                        e.avail &= ~POLLIN;
                        use(fd);
                        f.then_wrapped([me = shared_from_this(), &e, fd](future<temporary_buffer<char>> f) {
                            try {
                                auto buf = f.get0();
                                dns_log.trace("Read {} -> {} bytes", fd, buf.size());
                                e.tcp.indata = std::move(buf);
                            } catch (...) {
                                dns_log.debug("Read {} failed: {}", fd, std::current_exception());
                            }
                            e.avail |= POLLIN; // always reset state
                            me->poll_sockets();
                            me->release(fd);
                        });
                        errno = EWOULDBLOCK;
                        return -1;
                    }

                    try {
                        tcp.indata = f.get0();
                        continue; // loop will take care of data
                    } catch (std::system_error& e) {
                        errno = e.code().value();
                        return -1;
                    } catch (...) {
                    }
                    return -1;

                }
                case type::udp: {
                    auto & udp = e.udp;
                    if (udp.in) {
                        auto & p = udp.in->get_data();

                        dns_log.trace("Read {}. {} bytes available from {}", fd, p.len(), udp.in->get_src());

                        if (from != nullptr) {
                            *from = socket_address(udp.in->get_src()).as_posix_sockaddr();
                            if (from_len != nullptr) {
                                // TODO: ipvv6
                                *from_len = sizeof(sockaddr_in);
                            }
                        }

                        len = std::min(len, size_t(p.len()));
                        size_t rem = len;
                        auto * out = reinterpret_cast<char *>(dst);
                        for (auto & f : p.fragments()) {
                            auto n = std::min(rem, f.size);
                            out = std::copy_n(f.base, n, out);
                            rem = rem - n;
                        }
                        if (p.len() == len) {
                            udp.in = {};
                        } else {
                            p.trim_front(len);
                        }
                        return len;
                    }
                    auto f = udp.channel.receive();
                    if (!f.available()) {
                        e.avail &= ~POLLIN;
                        use(fd);
                        dns_log.trace("Read {}: data unavailable", fd);
                        f.then_wrapped([me = shared_from_this(), &e, fd](future<net::udp_datagram> f) {
                            try {
                                auto d = f.get0();
                                dns_log.trace("Read {} -> {} bytes", fd, d.get_data().len());
                                e.udp.in = std::move(d);
                                e.avail |= POLLIN;
                            } catch (...) {
                                dns_log.debug("Read {} failed: {}", fd, std::current_exception());
                            }
                            me->poll_sockets();
                            me->release(fd);
                        });
                        errno = EWOULDBLOCK;
                        return -1;
                    }

                    try {
                        udp.in = f.get0();
                        continue; // loop will take care of data
                    } catch (std::system_error& e) {
                        errno = e.code().value();
                        return -1;
                    } catch (...) {
                    }
                    return -1;
                }
                default:
                    return -1;
                }
            }
        } catch (...) {
        }
        return -1;
    }
    ssize_t do_sendv(ares_socket_t fd, const iovec * vec, int len) {
        if (_closed) {
            return -1;
        }
        try {
            auto& e = _sockets.at(fd);
            dns_log.trace("Send {}({})", fd, int(e.typ));

            // Assume we will be able to send data eventually very soon
            // and just assume that unless we get immediate
            // failures, we'll be ok. If we're not, the
            // timeout logic will have to handle the problem.
            //
            // This saves us on two accounts:
            // 1.) c-ares does not handle EWOULDBLOCK for
            //     udp sockets. Must pretend to finish
            //     immediately there anyway
            // 2.) Doing so for tcp writes saves us having to
            //     match iovec->packet fragments. Downside is we
            //     have to copy the data, but we pretty much
            //     have to anyway, since we could otherwise
            //     get a query time out while we're sending
            //     with zero-copy and suddenly have freed
            //     memory in packets. Bad.


            for (;;) {
                // check if we're already writing.
                if (e.typ == type::tcp && !(e.avail & POLLOUT)) {
                    dns_log.trace("Send already pending {}", fd);
                    errno = EWOULDBLOCK;
                    return -1;
                }

                net::packet p;
                p.reserve(len);
                for (int i = 0; i < len; ++i) {
                    p = net::packet(std::move(p), net::fragment{reinterpret_cast<char *>(vec[i].iov_base), vec[i].iov_len});
                }

                auto bytes = p.len();
                auto f = make_ready_future();

                switch (e.typ) {
                case type::tcp:
                    if (!e.tcp.out) {
                        e.tcp.out = e.tcp.socket.output(0);
                    }
                    f = e.tcp.out->write(std::move(p));
                    break;
                case type::udp:
                    f = e.udp.channel.send(e.udp.dst, std::move(p));
                    break;
                default:
                    return -1;
                }

                if (!f.available()) {
                    dns_log.trace("Send {} unavailable.", fd);
                    e.avail &= ~POLLOUT;
                    use(fd);
                    f.then_wrapped([me = shared_from_this(), &e, bytes, fd](future<> f) {
                        try {
                            f.get();
                            dns_log.trace("Send {}. {} bytes sent.", fd, bytes);
                        } catch (...) {
                            dns_log.debug("Send {} failed: {}", fd, std::current_exception());
                        }
                        e.avail |= POLLOUT;
                        me->poll_sockets();
                        me->release(fd);
                    });
                    // c-ares does _not_ use non-blocking retry for udp sockets. We just pretend
                    // all is fine even though we have no idea. Barring stack/adapter failure it
                    // is close to the same guarantee a "normal" message send would have anyway.
                    // For tcp we also pretend we're done, to make sure we don't have to deal with
                    // matching sent data
                }
                if (f.failed()) {
                    try {
                        f.get();
                    } catch (std::system_error& e) {
                        errno = e.code().value();
                    } catch (...) {
                    }
                    return -1;
                }

                return len;
            }
        } catch (...) {
        }
        return -1;
    }

    // Note: cannot use to much here, because fd_sets only handle
    // ~1024 fd:s. Set to something below that in case you need to
    // debug (maybe)
    static constexpr ares_socket_t socket_offset = 1;

    ares_socket_t next_fd() {
        ares_socket_t fd = ares_socket_t(_sockets.size() + socket_offset);
        while (_sockets.count(fd)) {
            ++fd;
        }
        return fd;
    }
    struct tcp_entry {
        tcp_entry(connected_socket s)
                        : socket(std::move(s)) {
        }
        ;
        connected_socket socket;
        compat::optional<input_stream<char>> in;
        compat::optional<output_stream<char>> out;
        temporary_buffer<char> indata;
    };
    struct udp_entry {
        udp_entry(net::udp_channel c)
                        : channel(std::move(c)) {
        }
        net::udp_channel channel;
        compat::optional<net::udp_datagram> in;;
        socket_address dst;
    };
    struct sock_entry {
        union {
            tcp_entry tcp;
            udp_entry udp;
        };
        type typ;
        int avail = 0;
        int pending = 0;
        bool closed = false;

        sock_entry(sock_entry&& e)
            : typ(e.typ)
            , avail(e.avail)
        {
            e.typ = type::none;
            switch (typ) {
            case type::tcp:
                tcp = std::move(e.tcp);
                break;
            case type::udp:
                udp = std::move(e.udp);
                break;
            default:
                break;
            }
        }
        sock_entry(connected_socket s)
            : tcp(tcp_entry{std::move(s)})
            , typ(type::tcp)
        {}
        sock_entry(net::udp_channel c)
            : udp(udp_entry{std::move(c)})
            , typ(type::udp)
        {}
        ~sock_entry() {
            switch (typ) {
            case type::tcp: tcp.~tcp_entry(); break;
            case type::udp: udp.~udp_entry(); break;
            default: break;
            }
        }
    };

    sock_entry& get_socket_entry(ares_socket_t fd) {
        auto& e = _sockets.at(fd);
        if (e.closed) {
            throw std::runtime_error("Socket closed");
        }
        return e;
    }


    typedef std::unordered_map<ares_socket_t, sock_entry> socket_map;

    friend struct dns_call;

    socket_map _sockets;
    network_stack & _stack;

    ares_channel _channel = {};
    uint64_t _calls = 0;
    std::chrono::milliseconds _timeout;
    timer<> _timer;
    gate _gate;
    bool _closed = false;
};

net::dns_resolver::dns_resolver()
    : dns_resolver(options())
{}

net::dns_resolver::dns_resolver(const options& opts)
    : dns_resolver(engine().net(), opts)
{}

net::dns_resolver::dns_resolver(network_stack& stack, const options& opts)
    : _impl(make_shared<impl>(stack, opts))
{}

net::dns_resolver::~dns_resolver()
{}

net::dns_resolver::dns_resolver(dns_resolver&&) noexcept = default;
net::dns_resolver& net::dns_resolver::operator=(dns_resolver&&) noexcept = default;

future<net::hostent> net::dns_resolver::get_host_by_name(const sstring& name, opt_family family) {
    return _impl->get_host_by_name(name, family);
}

future<net::hostent> net::dns_resolver::get_host_by_addr(const inet_address& addr) {
    return _impl->get_host_by_addr(addr);
}

future<net::inet_address> net::dns_resolver::resolve_name(const sstring& name, opt_family family) {
    return _impl->resolve_name(name, family);
}

future<sstring> net::dns_resolver::resolve_addr(const inet_address& addr) {
    return _impl->resolve_addr(addr);
}

future<net::dns_resolver::srv_records> net::dns_resolver::get_srv_records(net::dns_resolver::srv_proto proto,
                                                                          const sstring& service,
                                                                          const sstring& domain) {
    return _impl->get_srv_records(proto, service, domain);
}

future<> net::dns_resolver::close() {
    return _impl->close();
}

static net::dns_resolver& resolver() {
    static thread_local net::dns_resolver resolver;
    return resolver;
}


future<net::hostent> net::dns::get_host_by_name(const sstring& name, opt_family family) {
    return resolver().get_host_by_name(name, family);
}

future<net::hostent> net::dns::get_host_by_addr(const inet_address& addr) {
    return resolver().get_host_by_addr(addr);
}

future<net::inet_address> net::dns::resolve_name(const sstring& name, opt_family family) {
    return resolver().resolve_name(name, family);
}

future<sstring> net::dns::resolve_addr(const inet_address& addr) {
    return resolver().resolve_addr(addr);
}

future<net::dns_resolver::srv_records> net::dns::get_srv_records(net::dns_resolver::srv_proto proto,
                                                                 const sstring& service,
                                                                 const sstring& domain) {
    return resolver().get_srv_records(proto, service, domain);
}

future<sstring> net::inet_address::hostname() const {
    return dns::resolve_addr(*this);
}

future<std::vector<sstring>> net::inet_address::aliases() const {
    return dns::get_host_by_addr(*this).then([](hostent e) {
        return make_ready_future<std::vector<sstring>>(std::move(e.names));
    });
}

future<net::inet_address> net::inet_address::find(
                const sstring& name) {
    return dns::resolve_name(name);
}

future<net::inet_address> net::inet_address::find(
                const sstring& name, family f) {
    return dns::resolve_name(name, f);
}

future<std::vector<net::inet_address>> net::inet_address::find_all(
                const sstring& name) {
    return dns::get_host_by_name(name).then([](hostent e) {
        return make_ready_future<std::vector<net::inet_address>>(std::move(e.addr_list));
    });
}

future<std::vector<net::inet_address>> net::inet_address::find_all(
                const sstring& name, family f) {
    return dns::get_host_by_name(name, f).then([](hostent e) {
        return make_ready_future<std::vector<net::inet_address>>(std::move(e.addr_list));
    });
}

}
