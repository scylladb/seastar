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
 * Copyright (C) 2019 Red Hat, Inc.
 */

#include <filesystem>

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/core/print.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/log.hh>
#include <seastar/util/std-compat.hh>

using namespace seastar;
using std::string;
using namespace std::string_literals;
using namespace std::chrono_literals;

static logger iplog("unix_domain");

class ud_server_client {
public:
    ud_server_client(string server_path, std::optional<string> client_path, int rounds) :
        ud_server_client(server_path, client_path, rounds, 0) {};

    ud_server_client(string server_path, std::optional<string> client_path, int rounds,
                     int abort_run) :
        server_addr{unix_domain_addr{server_path}}, client_path{client_path},
                    rounds{rounds},
                    rounds_left{rounds}, abort_after{abort_run} {}

    future<> run();
    ud_server_client(ud_server_client&&) = default;
    ud_server_client(const ud_server_client&) = delete;

private:
    const string test_message{"are you still the same?"s};
    future<> init_server();
    void client_round();
    const socket_address server_addr;

    const std::optional<string> client_path;
    server_socket server;
    const int rounds;
    int rounds_left;
    server_socket* lstn_sock;
    seastar::thread th;
    int abort_after; // if set - force the listening socket down after that number of rounds
    bool planned_abort{false}; // set when abort_accept() is called
};

future<> ud_server_client::init_server() {
    return do_with(seastar::listen(server_addr), [this](server_socket& lstn) mutable {

        lstn_sock = &lstn; // required when aborting (on some tests)

        //  start the clients here, where we know the server is listening

        th = seastar::thread([this]{
            for (int i=0; i<rounds; ++i) {
                if (abort_after) {
                    if (--abort_after == 0) {
                        planned_abort = true;
                        lstn_sock->abort_accept();
                        break;
                    }
                }
                client_round();
            }
        });

        return do_until([this](){return rounds_left<=0;}, [&lstn,this]() {
            return lstn.accept().then([this](accept_result from_accept) {
                connected_socket cn    = std::move(from_accept.connection);
                socket_address cn_addr = std::move(from_accept.remote_address);
                --rounds_left;
                //  verify the client address
                if (client_path) {
                    socket_address tmmp{unix_domain_addr{*client_path}};
                    BOOST_REQUIRE_EQUAL(cn_addr, socket_address{unix_domain_addr{*client_path}});
                }

                return do_with(cn.input(), cn.output(), [](auto& inp, auto& out) {

                    return inp.read().then([&out](auto bb) {
                        string ans = "-"s;
                        if (bb && bb.size()) {
                            ans = "+"s + string{bb.get(), bb.size()};
                        }
                        return out.write(ans).then([&out](){return out.flush();}).
                        then([&out](){return out.close();});
                    }).then([&inp]() { return inp.close(); }).
                    then([]() { return make_ready_future<>(); });

                }).then([]{ return make_ready_future<>();});
            });
        }).handle_exception([this](auto e) {
            // OK to get here only if the test was a "planned abort" one
            if (!planned_abort) {
                std::rethrow_exception(e);
            }
        }).finally([this]{
            return th.join();
        });
    });
}

/// Send a message to the server, and expect (almost) the same string back.
/// If 'client_path' is set, the client binds to the named path.
// Runs in a seastar::thread.
void ud_server_client::client_round() {
    auto cc = client_path ?
        engine().net().connect(server_addr, socket_address{unix_domain_addr{*client_path}}).get() :
        engine().net().connect(server_addr).get();

    auto inp = cc.input();
    auto out = cc.output();

    out.write(test_message).get();
    out.flush().get();
    auto bb = inp.read().get();
    BOOST_REQUIRE_EQUAL(std::string_view(bb.begin(), bb.size()), "+"s+test_message);
    inp.close().get();
    out.close().get();
}

future<> ud_server_client::run() {
    return async([this] {
        auto serverfut = init_server();
        (void)serverfut.get();
    });

}

void rm(std::string_view what) {
    auto res = system(fmt::format("rm -f {}", what).c_str());
    BOOST_REQUIRE_EQUAL(res, 0);
}

//  testing the various address types, both on the server and on the
//  client side

SEASTAR_TEST_CASE(unixdomain_server) {
    rm("/tmp/ry");
    ud_server_client uds("/tmp/ry", std::nullopt, 3);
    return do_with(std::move(uds), [](auto& uds){
        return uds.run();
    });
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(unixdomain_abs) {
    char sv_name[]{'\0', '1', '1', '1'};
    //ud_server_client uds(string{"\0111",4}, string{"\0112",4}, 1);
    ud_server_client uds(string{sv_name,4}, std::nullopt, 4);
    return do_with(std::move(uds), [](auto& uds){
        return uds.run();
    });
    //return make_ready_future<>();
}

SEASTAR_TEST_CASE(unixdomain_abs_bind) {
    char sv_name[]{'\0', '1', '1', '1'};
    char cl_name[]{'\0', '1', '1', '2'};
    ud_server_client uds(string{sv_name,4}, string{cl_name,4}, 1);
    return do_with(std::move(uds), [](auto& uds){
        return uds.run();
    });
}

SEASTAR_TEST_CASE(unixdomain_abs_bind_2) {
    char sv_name[]{'\0', '1', '\0', '\12', '1'};
    char cl_name[]{'\0', '1', '\0', '\12', '2'};
    ud_server_client uds(string{sv_name,5}, string{cl_name,5}, 2);
    return do_with(std::move(uds), [](auto& uds){
        return uds.run();
    });
}

SEASTAR_TEST_CASE(unixdomain_text) {
    socket_address addr1{unix_domain_addr{"abc"}};
    BOOST_REQUIRE_EQUAL(format("{}", addr1), "abc");
    socket_address addr2{unix_domain_addr{""}};
    BOOST_REQUIRE_EQUAL(format("{}", addr2), "{unnamed}");
    socket_address addr3{unix_domain_addr{std::string("\0abc", 5)}};
    BOOST_REQUIRE_EQUAL(format("{}", addr3), "@abc_");
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(unixdomain_bind) {
    rm("111 112");
    ud_server_client uds("111"s, "112"s, 1);
    return do_with(std::move(uds), [](auto& uds){
        return uds.run();
    });
}

SEASTAR_TEST_CASE(unixdomain_short) {
    rm("3");
    ud_server_client uds("3"s, std::nullopt, 10);
    return do_with(std::move(uds), [](auto& uds){
        return uds.run();
    });
}

//  test our ability to abort the accept()'ing on a socket.
//  The test covers a specific bug in the handling of abort_accept()
SEASTAR_TEST_CASE(unixdomain_abort) {
    std::string sockname{"7"s}; // note: no portable & warnings-free option
    std::ignore = ::unlink(sockname.c_str());
    ud_server_client uds(sockname, std::nullopt, 10, 4);
    return do_with(std::move(uds), [sockname](auto& uds){
        return uds.run().finally([sockname](){
            std::ignore = ::unlink(sockname.c_str());
            return seastar::make_ready_future<>();
        });
    });
}

// From man 7 unix:
//  If a bind(2) call specifies addrlen as sizeof(sa_family_t), or
//  the SO_PASSCRED socket option was specified for a socket that was
//  not explicitly bound to an address, then the socket is autobound
//  to an abstract address.
static socket_address autobind() {
    socket_address addr;
    addr.addr_length = offsetof(sockaddr_un, sun_path);
    addr.u.sa.sa_family = AF_UNIX;
    return addr;
}

static string to_string(net::packet p) {
    p.linearize();
    const auto& f = p.frag(0);
    return std::string(f.base, f.size);
}

SEASTAR_THREAD_TEST_CASE(unixdomain_datagram_autobind) {
    auto chan1 = make_unbound_datagram_channel(AF_UNIX);
    auto chan2 = make_bound_datagram_channel(autobind());

    chan1.send(chan2.local_address(), "hello").get();
    net::datagram dgram = chan2.receive().get();

    string received = to_string(std::move(dgram.get_data()));
    BOOST_REQUIRE_EQUAL(received, "hello");
}

SEASTAR_THREAD_TEST_CASE(unixdomain_datagram_named_bound) {
    // Create a temporary directory for the socket file using mkdtemp.
    char tmpdir[] = "/tmp/seastar-test-XXXXXX";
    char* tmpdir_ptr = mkdtemp(tmpdir);
    if (tmpdir_ptr == nullptr) {
        throw std::runtime_error("mkdtemp failed");
    }

    // Create a socket file in the temporary directory.
    std::string socket_path = format("{}/socket", tmpdir_ptr);
    auto named_receiver = make_bound_datagram_channel(socket_address{unix_domain_addr{socket_path}});
    // Verify that a socket file was created.
    BOOST_REQUIRE(std::filesystem::exists(socket_path));

    // Send a message to the named socket using an unbound socket.
    auto sender = make_unbound_datagram_channel(AF_UNIX);
    sender.send(socket_address{unix_domain_addr{socket_path}}, "hihi").get();

    net::udp_datagram dgram = named_receiver.receive().get();
    string received = to_string(std::move(dgram.get_data()));
    BOOST_REQUIRE_EQUAL(received, "hihi");

    // Try to be nice and remove the temporary directory.
    std::filesystem::remove_all(tmpdir_ptr);
}
