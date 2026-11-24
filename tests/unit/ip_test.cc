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
#include <seastar/core/coroutine.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/ip.hh>
#include <seastar/testing/test_case.hh>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

using namespace seastar;
using namespace seastar::net;

namespace {

class echo_queue final : public qp {
public:
    promise<packet> reply;

    future<> send(packet p) override {
        reply.set_value(std::move(p));
        return make_ready_future<>();
    }
};

class echo_device final : public device {
public:
    echo_queue queue;

    echo_device() {
        _queues[this_shard_id()] = &queue;
    }

    ethernet_address hw_address() override {
        return {0x02, 0, 0, 0, 0, 1};
    }

    net::hw_features hw_features() override {
        return {};
    }

    std::unique_ptr<qp> init_local_queue(const program_options::option_group&, uint16_t) override {
        return {};
    }
};

}

SEASTAR_TEST_CASE(test_checksum_empty_fragments) {
    std::array<char, 17> bytes;
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = char(i + 1);
    }
    auto expected = ip_checksum(bytes.data(), bytes.size());
    for (size_t split = 0; split <= bytes.size(); ++split) {
        checksummer csum;
        csum.sum(bytes.data(), split);
        auto before = csum.get();
        auto before_sum = csum.csum;
        auto odd = csum.odd;
        csum.sum(nullptr, 0);
        BOOST_REQUIRE_EQUAL(csum.get(), before);
        BOOST_REQUIRE(csum.csum == before_sum);
        BOOST_REQUIRE_EQUAL(csum.odd, odd);
        csum.sum(bytes.data() + split, bytes.size() - split);
        BOOST_REQUIRE_EQUAL(csum.get(), expected);

        packet p(std::vector<fragment>{{bytes.data(), split}, {nullptr, 0},
                {bytes.data() + split, bytes.size() - split}, {nullptr, 0}}, {});
        checksummer packet_csum;
        packet_csum.sum(p);
        BOOST_REQUIRE_EQUAL(packet_csum.get(), expected);
    }
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_icmp_fragmented_echo_reply) {
    for (size_t split : {sizeof(icmp_hdr), sizeof(icmp_hdr) + 1}) {
        for (bool empty_fragment : {false, true}) {
            auto dev = std::make_shared<echo_device>();
            interface netif(dev);
            ipv4 ip(&netif);
            ipv4_address sender(0x0a000002);
            ipv4_address local(0x0a000001);
            ip.set_host_address(local);
            ip.learn({0x02, 0, 0, 0, 0, 2}, sender);
            icmp::inet_type inet(ip);
            icmp echo(inet);

            std::array<char, 25> bytes{};
            auto hdr = reinterpret_cast<icmp_hdr*>(bytes.data());
            hdr->type = icmp_hdr::msg_type::echo_request;
            hdr->rest = 0x12345678;
            for (size_t i = sizeof(icmp_hdr); i < bytes.size(); ++i) {
                bytes[i] = char(i);
            }
            hdr->csum = ip_checksum(bytes.data(), bytes.size());

            // Keep padding in the first allocation so the old contiguous read
            // produces a deterministic wrong checksum instead of relying on a fault.
            std::array<char, bytes.size()> first{};
            std::copy_n(bytes.begin(), split, first.begin());
            std::vector<fragment> fragments{{first.data(), split}};
            if (empty_fragment) {
                fragments.push_back({nullptr, 0});
            }
            fragments.push_back({bytes.data() + split, bytes.size() - split});
            packet request(std::move(fragments), {});
            auto sent = dev->queue.reply.get_future();
            echo.received(std::move(request), sender, local);
            auto reply = co_await with_timeout(std::chrono::steady_clock::now() + std::chrono::seconds(5), std::move(sent));
            reply.trim_front(sizeof(eth_hdr) + sizeof(ip_hdr));
            BOOST_REQUIRE_EQUAL(reply.len(), bytes.size());
            auto result = reply.get_header<icmp_hdr>();
            BOOST_REQUIRE(result);
            BOOST_REQUIRE(result->type == icmp_hdr::msg_type::echo_reply);
            BOOST_REQUIRE_EQUAL(result->code, 0);
            BOOST_REQUIRE_EQUAL(uint32_t(result->rest), uint32_t(hdr->rest));
            auto contiguous = reply.get_header(0, reply.len());
            BOOST_REQUIRE_EQUAL(ip_checksum(contiguous, reply.len()), 0);
            BOOST_REQUIRE(std::equal(bytes.begin() + sizeof(icmp_hdr), bytes.end(), contiguous + sizeof(icmp_hdr)));
        }
    }
}
