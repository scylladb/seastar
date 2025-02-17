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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */
#ifdef SEASTAR_HAVE_DPDK

#ifdef SEASTAR_MODULE
module;
#endif

#include <cinttypes>
#include <atomic>
#include <iostream>
#include <vector>
#include <queue>
#include <getopt.h>
#include <malloc.h>

#include <cinttypes>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_pci.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_memzone.h>
#include <rte_vfio.h>

#include <boost/preprocessor.hpp>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/posix.hh>
#include <seastar/core/reactor.hh>
#include <seastar/net/virtio-interface.hh>
#include <seastar/core/stream.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/align.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/internal/poll.hh>
#include <seastar/core/units.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/function_input_iterator.hh>
#include <seastar/util/transform_iterator.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/net/ip.hh>
#include <seastar/net/const.hh>
#include <seastar/core/dpdk_rte.hh>
#include <seastar/net/dpdk.hh>
#include <seastar/net/toeplitz.hh>
#include <seastar/net/native-stack.hh>
#include "core/vla.hh"
#endif

#if RTE_VERSION <= RTE_VERSION_NUM(2,0,0,16)

static
inline
char*
rte_mbuf_to_baddr(rte_mbuf* mbuf) {
    return reinterpret_cast<char*>(RTE_MBUF_TO_BADDR(mbuf));
}

void* as_cookie(struct rte_pktmbuf_pool_private& p) {
    return reinterpret_cast<void*>(uint64_t(p.mbuf_data_room_size));
};

#else

void* as_cookie(struct rte_pktmbuf_pool_private& p) {
    return &p;
};

#endif

#ifndef MARKER
typedef void    *MARKER[0];   /**< generic marker for a point in a structure */
#endif

// Calculate maximum amount of memory required to store given number of objects
static size_t
get_mempool_xmem_size(uint32_t elt_num, size_t total_elt_sz, uint32_t pg_shift)
{
    size_t obj_per_page, pg_num, pg_sz;

    if (total_elt_sz == 0) {
        return 0;
    }

    if (pg_shift == 0) {
        return total_elt_sz * elt_num;
    }

    pg_sz = (size_t)1 << pg_shift;
    obj_per_page = pg_sz / total_elt_sz;
    if (obj_per_page == 0) {
        return RTE_ALIGN_CEIL(total_elt_sz, pg_sz) * elt_num;
    }

    pg_num = (elt_num + obj_per_page - 1) / obj_per_page;
    return pg_num << pg_shift;
}

using namespace seastar::net;

namespace seastar {

namespace dpdk {

/******************* Net device related constatns *****************************/
static constexpr uint16_t default_ring_size      = 512;

//
// We need 2 times the ring size of buffers because of the way PMDs
// refill the ring.
//
static constexpr uint16_t mbufs_per_queue_rx     = 2 * default_ring_size;
static constexpr uint16_t rx_gc_thresh           = 64;

//
// No need to keep more descriptors in the air than can be sent in a single
// rte_eth_tx_burst() call.
//
static constexpr uint16_t mbufs_per_queue_tx     = 2 * default_ring_size;

static constexpr uint16_t mbuf_cache_size        = 512;
static constexpr uint16_t mbuf_overhead          =
                                 sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM;
//
// We'll allocate 2K data buffers for an inline case because this would require
// a single page per mbuf. If we used 4K data buffers here it would require 2
// pages for a single buffer (due to "mbuf_overhead") and this is a much more
// demanding memory constraint.
//
static constexpr size_t   inline_mbuf_data_size  = 2048;

//
// Size of the data buffer in the non-inline case.
//
// We may want to change (increase) this value in future, while the
// inline_mbuf_data_size value will unlikely change due to reasons described
// above.
//
static constexpr size_t   mbuf_data_size         = 2048;

// (INLINE_MBUF_DATA_SIZE(2K)*32 = 64K = Max TSO/LRO size) + 1 mbuf for headers
static constexpr uint8_t  max_frags              = 32 + 1;

//
// Intel's 40G NIC HW limit for a number of fragments in an xmit segment.
//
// See Chapter 8.4.1 "Transmit Packet in System Memory" of the xl710 devices
// spec. for more details.
//
static constexpr uint8_t  i40e_max_xmit_segment_frags = 8;

//
// VMWare's virtual NIC limit for a number of fragments in an xmit segment.
//
// see drivers/net/vmxnet3/base/vmxnet3_defs.h VMXNET3_MAX_TXD_PER_PKT
//
static constexpr uint8_t vmxnet3_max_xmit_segment_frags = 16;

static constexpr uint16_t inline_mbuf_size       =
                                inline_mbuf_data_size + mbuf_overhead;

uint32_t qp_mempool_obj_size(bool hugetlbfs_membackend)
{
    uint32_t mp_size = 0;
    struct rte_mempool_objsz mp_obj_sz = {};

    //
    // We will align each size to huge page size because DPDK allocates
    // physically contiguous memory region for each pool object.
    //

    // Rx
    if (hugetlbfs_membackend) {
        mp_size +=
            align_up(rte_mempool_calc_obj_size(mbuf_overhead, 0, &mp_obj_sz)+
                                        sizeof(struct rte_pktmbuf_pool_private),
                                               memory::huge_page_size);
    } else {
        mp_size +=
            align_up(rte_mempool_calc_obj_size(inline_mbuf_size, 0, &mp_obj_sz)+
                                        sizeof(struct rte_pktmbuf_pool_private),
                                               memory::huge_page_size);
    }
    //Tx
    std::memset(&mp_obj_sz, 0, sizeof(mp_obj_sz));
    mp_size += align_up(rte_mempool_calc_obj_size(inline_mbuf_size, 0,
                                                  &mp_obj_sz)+
                                        sizeof(struct rte_pktmbuf_pool_private),
                                                  memory::huge_page_size);
    return mp_size;
}

static constexpr const char* pktmbuf_pool_name   = "dpdk_pktmbuf_pool";

/*
 * When doing reads from the NIC queues, use this batch size
 */
static constexpr uint8_t packet_read_size        = 32;
/******************************************************************************/

struct port_stats {
    port_stats() : rx{}, tx{} {}

    struct {
        struct {
            uint64_t mcast;        // number of received multicast packets
            uint64_t pause_xon;    // number of received PAUSE XON frames
            uint64_t pause_xoff;   // number of received PAUSE XOFF frames
        } good;

        struct {
            uint64_t dropped;      // missed packets (e.g. full FIFO)
            uint64_t crc;          // packets with CRC error
            uint64_t len;          // packets with a bad length
            uint64_t total;        // total number of erroneous received packets
        } bad;
    } rx;

    struct {
        struct {
            uint64_t pause_xon;   // number of sent PAUSE XON frames
            uint64_t pause_xoff;  // number of sent PAUSE XOFF frames
        } good;

        struct {
            uint64_t total;   // total number of failed transmitted packets
        } bad;
    } tx;
};

#define XSTATS_ID_LIST \
        (rx_multicast_packets) \
        (rx_xon_packets) \
        (rx_xoff_packets) \
        (rx_crc_errors) \
        (rx_length_errors) \
        (rx_undersize_errors) \
        (rx_oversize_errors) \
        (tx_xon_packets) \
        (tx_xoff_packets)

class dpdk_xstats {
public:
    dpdk_xstats(uint16_t port_id)
        : _port_id(port_id)
    {
    }

    ~dpdk_xstats()
    {
        if (_xstats)
            delete[] _xstats;
        if (_xstat_names)
            delete[] _xstat_names;
    }

    enum xstat_id {
        BOOST_PP_SEQ_ENUM(XSTATS_ID_LIST)
    };


    void start() {
        _len = rte_eth_xstats_get_names(_port_id, NULL, 0);
        _xstats = new rte_eth_xstat[_len];
        _xstat_names = new rte_eth_xstat_name[_len];
        update_xstats();
        update_xstat_names();
        update_offsets();
    }

    void update_xstats() {
        auto len = rte_eth_xstats_get(_port_id, _xstats, _len);
        SEASTAR_ASSERT(len == _len);
    }

    uint64_t get_value(const xstat_id id) {
        auto off = _offsets[static_cast<int>(id)];
        if (off == -1) {
            return 0;
        }
        return _xstats[off].value;
    }

private:
    uint16_t _port_id;
    int _len;
    struct rte_eth_xstat *_xstats = nullptr;
    struct rte_eth_xstat_name *_xstat_names = nullptr;
    int _offsets[BOOST_PP_SEQ_SIZE(XSTATS_ID_LIST)];

    static const sstring id_to_str(const xstat_id id) {
#define ENUM_TO_STR(r, data, elem) \
        if (id == elem) \
            return BOOST_PP_STRINGIZE(elem);

        BOOST_PP_SEQ_FOR_EACH(ENUM_TO_STR, ~, XSTATS_ID_LIST)
        return "";
    }

    int get_offset_by_name(const xstat_id id, const int len) {
        for (int i = 0; i < len; i++) {
            if (id_to_str(id) == _xstat_names[i].name)
                return i;
        }
        return -1;
    }

    void update_xstat_names() {
        auto len = rte_eth_xstats_get_names(_port_id, _xstat_names, _len);
        SEASTAR_ASSERT(len == _len);
    }

    void update_offsets() {
#define FIND_OFFSET(r, data, elem) \
        _offsets[static_cast<int>(elem)] = \
            get_offset_by_name(elem, _len);

        BOOST_PP_SEQ_FOR_EACH(FIND_OFFSET, ~, XSTATS_ID_LIST)
    }
};

class dpdk_device : public device {
    uint16_t _port_idx;
    uint16_t _num_queues;
    net::hw_features _hw_features;
    uint16_t _queues_ready = 0;
    unsigned _home_cpu;
    bool _use_lro;
    bool _enable_fc;
    std::vector<uint8_t> _redir_table;
    rss_key_type _rss_key;
    port_stats _stats;
    timer<> _stats_collector;
    const std::string _stats_plugin_name;
    const std::string _stats_plugin_inst;
    seastar::metrics::metric_groups _metrics;
    bool _is_i40e_device = false;
    bool _is_vmxnet3_device = false;
    dpdk_xstats _xstats;

public:
    rte_eth_dev_info _dev_info = {};
    promise<> _link_ready_promise;

private:
    /**
     * Port initialization consists of 3 main stages:
     * 1) General port initialization which ends with a call to
     *    rte_eth_dev_configure() where we request the needed number of Rx and
     *    Tx queues.
     * 2) Individual queues initialization. This is done in the constructor of
     *    dpdk_qp class. In particular the memory pools for queues are allocated
     *    in this stage.
     * 3) The final stage of the initialization which starts with the call of
     *    rte_eth_dev_start() after which the port becomes fully functional. We
     *    will also wait for a link to get up in this stage.
     */


    /**
     * First stage of the port initialization.
     *
     * @return 0 in case of success and an appropriate error code in case of an
     *         error.
     */
    int init_port_start();

    /**
     * The final stage of a port initialization.
     * @note Must be called *after* all queues from stage (2) have been
     *       initialized.
     */
    void init_port_fini();

    /**
     * Check the link status of out port in up to 9s, and print them finally.
     */
    void check_port_link_status();

    /**
     * Configures the HW Flow Control
     */
    void set_hw_flow_control();

public:
    dpdk_device(uint16_t port_idx, uint16_t num_queues, bool use_lro,
                bool enable_fc)
        : _port_idx(port_idx)
        , _num_queues(num_queues)
        , _home_cpu(this_shard_id())
        , _use_lro(use_lro)
        , _enable_fc(enable_fc)
        , _stats_plugin_name("network")
        , _stats_plugin_inst(std::string("port") + std::to_string(_port_idx))
        , _xstats(port_idx)
    {

        /* now initialise the port we will use */
        int ret = init_port_start();
        if (ret != 0) {
            rte_exit(EXIT_FAILURE, "Cannot initialise port %u\n", _port_idx);
        }

        // Register port statistics pollers
        namespace sm = seastar::metrics;
        _metrics.add_group(_stats_plugin_name, {
            // Rx Good
            sm::make_counter("rx_multicast", _stats.rx.good.mcast,
                            sm::description("Counts a number of received multicast packets."), {sm::shard_label(_stats_plugin_inst)}),
            // Rx Errors
            sm::make_counter("rx_crc_errors", _stats.rx.bad.crc,
                            sm::description("Counts a number of received packets with a bad CRC value. "
                                            "A non-zero value of this metric usually indicates a HW problem, e.g. a bad cable."), {sm::shard_label(_stats_plugin_inst)}),

            sm::make_counter("rx_dropped", _stats.rx.bad.dropped,
                            sm::description("Counts a number of dropped received packets. "
                                            "A non-zero value of this counter indicated the overflow of ingress HW buffers. "
                                            "This usually happens because of a rate of a sender on the other side of the link is higher than we can process as a receiver."), {sm::shard_label(_stats_plugin_inst)}),

            sm::make_counter("rx_bad_length_errors", _stats.rx.bad.len,
                            sm::description("Counts a number of received packets with a bad length value. "
                                            "A non-zero value of this metric usually indicates a HW issue: e.g. bad cable."), {sm::shard_label(_stats_plugin_inst)}),
            // Coupled counters:
            // Good
            sm::make_counter("rx_pause_xon", _stats.rx.good.pause_xon,
                            sm::description("Counts a number of received PAUSE XON frames (PAUSE frame with a quanta of zero). "
                                            "When PAUSE XON frame is received our port may resume sending L2 frames. "
                                            "PAUSE XON frames are sent to resume sending that was previously paused with a PAUSE XOFF frame. If ingress "
                                            "buffer falls below the low watermark threshold before the timeout configured in the original PAUSE XOFF frame the receiver may decide to send PAUSE XON frame. "
                                            "A non-zero value of this metric may mean that our sender is bursty and that the spikes overwhelm the receiver on the other side of the link."), {sm::shard_label(_stats_plugin_inst)}),

            sm::make_counter("tx_pause_xon", _stats.tx.good.pause_xon,
                            sm::description("Counts a number of sent PAUSE XON frames (L2 flow control frames). "
                                            "A non-zero value of this metric indicates that our ingress path doesn't keep up with the rate of a sender on the other side of the link. "
                                            "Note that if a sender port respects PAUSE frames this will prevent it from sending from ALL its egress queues because L2 flow control is defined "
                                            "on a per-link resolution."), {sm::shard_label(_stats_plugin_inst)}),

            sm::make_counter("rx_pause_xoff", _stats.rx.good.pause_xoff,
                            sm::description("Counts a number of received PAUSE XOFF frames. "
                                            "A non-zero value of this metric indicates that our egress overwhelms the receiver on the other side of the link and it has to send PAUSE frames to make us stop sending. "
                                            "Note that if our port respects PAUSE frames a reception of a PAUSE XOFF frame will cause ALL egress queues of this port to stop sending."), {sm::shard_label(_stats_plugin_inst)}),

            sm::make_counter("tx_pause_xoff", _stats.tx.good.pause_xoff,
                            sm::description("Counts a number of sent PAUSE XOFF frames. "
                                            "A non-zero value of this metric indicates that our ingress path (SW and HW) doesn't keep up with the rate of a sender on the other side of the link and as a result "
                                            "our ingress HW buffers overflow."), {sm::shard_label(_stats_plugin_inst)}),
            // Errors
            sm::make_counter("rx_errors", _stats.rx.bad.total,
                            sm::description("Counts the total number of ingress errors: CRC errors, bad length errors, etc."), {sm::shard_label(_stats_plugin_inst)}),

            sm::make_counter("tx_errors", _stats.tx.bad.total,
                            sm::description("Counts a total number of egress errors. A non-zero value usually indicated a problem with a HW or a SW driver."), {sm::shard_label(_stats_plugin_inst)}),
        });
    }

    ~dpdk_device() {
        _stats_collector.cancel();
    }

    ethernet_address hw_address() override {
        struct rte_ether_addr mac;
        rte_eth_macaddr_get(_port_idx, &mac);

        return mac.addr_bytes;
    }
    net::hw_features hw_features() override {
        return _hw_features;
    }

    net::hw_features& hw_features_ref() { return _hw_features; }

    const rte_eth_rxconf* def_rx_conf() const {
        return &_dev_info.default_rxconf;
    }

    const rte_eth_txconf* def_tx_conf() const {
        return &_dev_info.default_txconf;
    }

    /**
     *  Set the RSS table in the device and store it in the internal vector.
     */
    void set_rss_table();

    virtual uint16_t hw_queues_count() override { return _num_queues; }
    virtual future<> link_ready() override { return _link_ready_promise.get_future(); }
    virtual std::unique_ptr<qp> init_local_queue(const program_options::option_group& opts, uint16_t qid) override;
    virtual unsigned hash2qid(uint32_t hash) override {
        SEASTAR_ASSERT(_redir_table.size());
        return _redir_table[hash & (_redir_table.size() - 1)];
    }
    uint16_t port_idx() { return _port_idx; }
    bool is_i40e_device() const {
        return _is_i40e_device;
    }
    bool is_vmxnet3_device() const {
        return _is_vmxnet3_device;
    }

    virtual rss_key_type rss_key() const override { return _rss_key; }
};

template <bool HugetlbfsMemBackend>
class dpdk_qp : public net::qp {
    class tx_buf_factory;

    class tx_buf {
    friend class dpdk_qp;
    public:
        static tx_buf* me(rte_mbuf* mbuf) {
            return reinterpret_cast<tx_buf*>(mbuf);
        }

    private:
        /**
         * Checks if the original packet of a given cluster should be linearized
         * due to HW limitations.
         *
         * @param head head of a cluster to check
         *
         * @return TRUE if a packet should be linearized.
         */
        static bool i40e_should_linearize(rte_mbuf *head) {
            bool is_tso = head->ol_flags & RTE_MBUF_F_TX_TCP_SEG;

            // For a non-TSO case: number of fragments should not exceed 8
            if (!is_tso){
                return head->nb_segs > i40e_max_xmit_segment_frags;
            }

            //
            // For a TSO case each MSS window should not include more than 8
            // fragments including headers.
            //

            // Calculate the number of frags containing headers.
            //
            // Note: we support neither VLAN nor tunneling thus headers size
            // accounting is super simple.
            //
            size_t headers_size = head->l2_len + head->l3_len + head->l4_len;
            unsigned hdr_frags = 0;
            size_t cur_payload_len = 0;
            rte_mbuf *cur_seg = head;

            while (cur_seg && cur_payload_len < headers_size) {
                cur_payload_len += cur_seg->data_len;
                cur_seg = cur_seg->next;
                hdr_frags++;
            }

            //
            // Header fragments will be used for each TSO segment, thus the
            // maximum number of data segments will be 8 minus the number of
            // header fragments.
            //
            // It's unclear from the spec how the first TSO segment is treated
            // if the last fragment with headers contains some data bytes:
            // whether this fragment will be accounted as a single fragment or
            // as two separate fragments. We prefer to play it safe and assume
            // that this fragment will be accounted as two separate fragments.
            //
            size_t max_win_size = i40e_max_xmit_segment_frags - hdr_frags;

            if (head->nb_segs <= max_win_size) {
                return false;
            }

            // Get the data (without headers) part of the first data fragment
            size_t prev_frag_data = cur_payload_len - headers_size;
            auto mss = head->tso_segsz;

            while (cur_seg) {
                unsigned frags_in_seg = 0;
                size_t cur_seg_size = 0;

                if (prev_frag_data) {
                    cur_seg_size = prev_frag_data;
                    frags_in_seg++;
                    prev_frag_data = 0;
                }

                while (cur_seg_size < mss && cur_seg) {
                    cur_seg_size += cur_seg->data_len;
                    cur_seg = cur_seg->next;
                    frags_in_seg++;

                    if (frags_in_seg > max_win_size) {
                        return true;
                    }
                }

                if (cur_seg_size > mss) {
                    prev_frag_data = cur_seg_size - mss;
                }
            }

            return false;
        }

        /**
         * Sets the offload info in the head buffer of an rte_mbufs cluster.
         *
         * @param p an original packet the cluster is built for
         * @param qp QP handle
         * @param head a head of an rte_mbufs cluster
         */
        static void set_cluster_offload_info(const packet& p, const dpdk_qp& qp, rte_mbuf* head) {
            // Handle TCP checksum offload
            auto oi = p.get_offload_info();
            if (oi.needs_ip_csum) {
                head->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
                // TODO: Take a VLAN header into an account here
                head->l2_len = sizeof(struct rte_ether_hdr);
                head->l3_len = oi.ip_hdr_len;
            }
            if (qp.port().hw_features().tx_csum_l4_offload) {
                if (oi.protocol == ip_protocol_num::tcp) {
                    head->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
                    // TODO: Take a VLAN header into an account here
                    head->l2_len = sizeof(struct rte_ether_hdr);
                    head->l3_len = oi.ip_hdr_len;

                    if (oi.tso_seg_size) {
                        SEASTAR_ASSERT(oi.needs_ip_csum);
                        head->ol_flags |= RTE_MBUF_F_TX_TCP_SEG;
                        head->l4_len = oi.tcp_hdr_len;
                        head->tso_segsz = oi.tso_seg_size;
                    }
                } else if (oi.protocol == ip_protocol_num::udp) {
                    head->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
                    // TODO: Take a VLAN header into an account here
                    head->l2_len = sizeof(struct rte_ether_hdr);
                    head->l3_len = oi.ip_hdr_len;
                }
            }
        }

        /**
         * Creates a tx_buf cluster representing a given packet in a "zero-copy"
         * way.
         *
         * @param p packet to translate
         * @param qp dpdk_qp handle
         *
         * @return the HEAD tx_buf of the cluster or nullptr in case of a
         *         failure
         */
        static tx_buf* from_packet_zc(packet&& p, dpdk_qp& qp) {

            // Too fragmented - linearize
            if (p.nr_frags() > max_frags) {
                p.linearize();
                ++qp._stats.tx.linearized;
            }

build_mbuf_cluster:
            rte_mbuf *head = nullptr, *last_seg = nullptr;
            unsigned nsegs = 0;

            // Create a HEAD of the fragmented packet
            if (!translate_one_frag(qp, p.frag(0), head, last_seg, nsegs)) {
                return nullptr;
            }

            unsigned total_nsegs = nsegs;

            for (unsigned i = 1; i < p.nr_frags(); i++) {
                rte_mbuf *h = nullptr, *new_last_seg = nullptr;
                if (!translate_one_frag(qp, p.frag(i), h, new_last_seg, nsegs)) {
                    me(head)->recycle();
                    return nullptr;
                }

                total_nsegs += nsegs;

                // Attach a new buffers' chain to the packet chain
                last_seg->next = h;
                last_seg = new_last_seg;
            }

            // Update the HEAD buffer with the packet info
            head->pkt_len = p.len();
            head->nb_segs = total_nsegs;

            set_cluster_offload_info(p, qp, head);

            //
            // If a packet hasn't been linearized already and the resulting
            // cluster requires the linearisation due to HW limitation:
            //
            //    - Recycle the cluster.
            //    - Linearize the packet.
            //    - Build the cluster once again
            //
            if (head->nb_segs > max_frags ||
                (p.nr_frags() > 1 && qp.port().is_i40e_device() && i40e_should_linearize(head)) ||
                (p.nr_frags() > vmxnet3_max_xmit_segment_frags && qp.port().is_vmxnet3_device())) {
                me(head)->recycle();
                p.linearize();
                ++qp._stats.tx.linearized;

                goto build_mbuf_cluster;
            }

            me(last_seg)->set_packet(std::move(p));

            return me(head);
        }

        /**
         * Copy the contents of the "packet" into the given cluster of
         * rte_mbuf's.
         *
         * @note Size of the cluster has to be big enough to accommodate all the
         *       contents of the given packet.
         *
         * @param p packet to copy
         * @param head head of the rte_mbuf's cluster
         */
        static void copy_packet_to_cluster(const packet& p, rte_mbuf* head) {
            rte_mbuf* cur_seg = head;
            size_t cur_seg_offset = 0;
            unsigned cur_frag_idx = 0;
            size_t cur_frag_offset = 0;

            while (true) {
                size_t to_copy = std::min(p.frag(cur_frag_idx).size - cur_frag_offset,
                                          inline_mbuf_data_size - cur_seg_offset);

                memcpy(rte_pktmbuf_mtod_offset(cur_seg, void*, cur_seg_offset),
                       p.frag(cur_frag_idx).base + cur_frag_offset, to_copy);

                cur_frag_offset += to_copy;
                cur_seg_offset += to_copy;

                if (cur_frag_offset >= p.frag(cur_frag_idx).size) {
                    ++cur_frag_idx;
                    if (cur_frag_idx >= p.nr_frags()) {
                        //
                        // We are done - set the data size of the last segment
                        // of the cluster.
                        //
                        cur_seg->data_len = cur_seg_offset;
                        break;
                    }

                    cur_frag_offset = 0;
                }

                if (cur_seg_offset >= inline_mbuf_data_size) {
                    cur_seg->data_len = inline_mbuf_data_size;
                    cur_seg = cur_seg->next;
                    cur_seg_offset = 0;

                    // FIXME: assert in a fast-path - remove!!!
                    SEASTAR_ASSERT(cur_seg);
                }
            }
        }

        /**
         * Creates a tx_buf cluster representing a given packet in a "copy" way.
         *
         * @param p packet to translate
         * @param qp dpdk_qp handle
         *
         * @return the HEAD tx_buf of the cluster or nullptr in case of a
         *         failure
         */
        static tx_buf* from_packet_copy(packet&& p, dpdk_qp& qp) {
            // sanity
            if (!p.len()) {
                return nullptr;
            }

            /*
             * Here we are going to use the fact that the inline data size is a
             * power of two.
             *
             * We will first try to allocate the cluster and only if we are
             * successful - we will go and copy the data.
             */
            auto aligned_len = align_up((size_t)p.len(), inline_mbuf_data_size);
            unsigned nsegs = aligned_len / inline_mbuf_data_size;
            rte_mbuf *head = nullptr, *last_seg = nullptr;

            tx_buf* buf = qp.get_tx_buf();
            if (!buf) {
                return nullptr;
            }

            head = buf->rte_mbuf_p();
            last_seg = head;
            for (unsigned i = 1; i < nsegs; i++) {
                buf = qp.get_tx_buf();
                if (!buf) {
                    me(head)->recycle();
                    return nullptr;
                }

                last_seg->next = buf->rte_mbuf_p();
                last_seg = last_seg->next;
            }

            //
            // If we've got here means that we have succeeded already!
            // We only need to copy the data and set the head buffer with the
            // relevant info.
            //
            head->pkt_len = p.len();
            head->nb_segs = nsegs;

            copy_packet_to_cluster(p, head);
            set_cluster_offload_info(p, qp, head);

            return me(head);
        }

        /**
         * Zero-copy handling of a single net::fragment.
         *
         * @param do_one_buf Functor responsible for a single rte_mbuf
         *                   handling
         * @param qp dpdk_qp handle (in)
         * @param frag Fragment to copy (in)
         * @param head Head of the cluster (out)
         * @param last_seg Last segment of the cluster (out)
         * @param nsegs Number of segments in the cluster (out)
         *
         * @return TRUE in case of success
         */
        template <class DoOneBufFunc>
        static bool do_one_frag(DoOneBufFunc do_one_buf, dpdk_qp& qp,
                                fragment& frag, rte_mbuf*& head,
                                rte_mbuf*& last_seg, unsigned& nsegs) {
            size_t len, left_to_set = frag.size;
            char* base = frag.base;

            rte_mbuf* m;

            // TODO: SEASTAR_ASSERT() in a fast path! Remove me ASAP!
            SEASTAR_ASSERT(frag.size);

            // Create a HEAD of mbufs' cluster and set the first bytes into it
            len = do_one_buf(qp, head, base, left_to_set);
            if (!len) {
                return false;
            }

            left_to_set -= len;
            base += len;
            nsegs = 1;

            //
            // Set the rest of the data into the new mbufs and chain them to
            // the cluster.
            //
            rte_mbuf* prev_seg = head;
            while (left_to_set) {
                len = do_one_buf(qp, m, base, left_to_set);
                if (!len) {
                    me(head)->recycle();
                    return false;
                }

                left_to_set -= len;
                base += len;
                nsegs++;

                prev_seg->next = m;
                prev_seg = m;
            }

            // Return the last mbuf in the cluster
            last_seg = prev_seg;

            return true;
        }

        /**
         * Zero-copy handling of a single net::fragment.
         *
         * @param qp dpdk_qp handle (in)
         * @param frag Fragment to copy (in)
         * @param head Head of the cluster (out)
         * @param last_seg Last segment of the cluster (out)
         * @param nsegs Number of segments in the cluster (out)
         *
         * @return TRUE in case of success
         */
        static bool translate_one_frag(dpdk_qp& qp, fragment& frag,
                                       rte_mbuf*& head, rte_mbuf*& last_seg,
                                       unsigned& nsegs) {
            return do_one_frag(set_one_data_buf, qp, frag, head,
                               last_seg, nsegs);
        }

        /**
         * Copies one net::fragment into the cluster of rte_mbuf's.
         *
         * @param qp dpdk_qp handle (in)
         * @param frag Fragment to copy (in)
         * @param head Head of the cluster (out)
         * @param last_seg Last segment of the cluster (out)
         * @param nsegs Number of segments in the cluster (out)
         *
         * We return the "last_seg" to avoid traversing the cluster in order to get
         * it.
         *
         * @return TRUE in case of success
         */
        static bool copy_one_frag(dpdk_qp& qp, fragment& frag,
                                  rte_mbuf*& head, rte_mbuf*& last_seg,
                                  unsigned& nsegs) {
            return do_one_frag(copy_one_data_buf, qp, frag, head,
                               last_seg, nsegs);
        }

        /**
         * Allocates a single rte_mbuf and sets it to point to a given data
         * buffer.
         *
         * @param qp dpdk_qp handle (in)
         * @param m New allocated rte_mbuf (out)
         * @param va virtual address of a data buffer (in)
         * @param buf_len length of the data to copy (in)
         *
         * @return The actual number of bytes that has been set in the mbuf
         */
        static size_t set_one_data_buf(
            dpdk_qp& qp, rte_mbuf*& m, char* va, size_t buf_len) {
            static constexpr size_t max_frag_len = 15_KiB;

            //
            // Currently we break a buffer on a 15K boundary because 82599
            // devices have a 15.5K limitation on a maximum single fragment
            // size.
            //
            rte_iova_t iova = rte_mem_virt2iova(va);

            if (iova == RTE_BAD_IOVA) {
                return copy_one_data_buf(qp, m, va, buf_len);
            }

            tx_buf* buf = qp.get_tx_buf();
            if (!buf) {
                return 0;
            }

            size_t len = std::min(buf_len, max_frag_len);

            buf->set_zc_info(va, iova, len);
            m = buf->rte_mbuf_p();

            return len;
        }

        /**
         *  Allocates a single rte_mbuf and copies a given data into it.
         *
         * @param qp dpdk_qp handle (in)
         * @param m New allocated rte_mbuf (out)
         * @param data Data to copy from (in)
         * @param buf_len length of the data to copy (in)
         *
         * @return The actual number of bytes that has been copied
         */
        static size_t copy_one_data_buf(
            dpdk_qp& qp, rte_mbuf*& m, char* data, size_t buf_len)
        {
            tx_buf* buf = qp.get_tx_buf();
            if (!buf) {
                return 0;
            }

            size_t len = std::min(buf_len, inline_mbuf_data_size);

            m = buf->rte_mbuf_p();

            // mbuf_put()
            m->data_len = len;
            m->pkt_len  = len;

            qp._stats.tx.good.update_copy_stats(1, len);

            memcpy(rte_pktmbuf_mtod(m, void*), data, len);

            return len;
        }

    public:
        tx_buf(tx_buf_factory& fc) : _fc(fc) {

            _buf_iova     = _mbuf.buf_iova;
            _data_off     = _mbuf.data_off;
        }

        rte_mbuf* rte_mbuf_p() { return &_mbuf; }

        void set_zc_info(void* va, rte_iova_t iova, size_t len) {
            // mbuf_put()
            _mbuf.data_len           = len;
            _mbuf.pkt_len            = len;

            // Set the mbuf to point to our data
            _mbuf.buf_addr           = va;
            _mbuf.buf_iova           = iova;
            _mbuf.data_off           = 0;
            _is_zc                   = true;
        }

        void reset_zc() {
            //
            // If this mbuf was the last in a cluster and contains an
            // original packet object then call the destructor of the
            // original packet object.
            //
            if (_p) {
                //
                // Reset the std::optional. This in particular is going
                // to call the "packet"'s destructor and reset the
                // "optional" state to "nonengaged".
                //
                _p = std::nullopt;

            } else if (!_is_zc) {
                return;
            }

            // Restore the rte_mbuf fields we trashed in set_zc_info()
            _mbuf.buf_iova     = _buf_iova;
            _mbuf.buf_addr     = rte_mbuf_to_baddr(&_mbuf);
            _mbuf.data_off     = _data_off;

            _is_zc             = false;
        }

        void recycle() {
            struct rte_mbuf *m = &_mbuf, *m_next;

            while (m != nullptr) {
                m_next = m->next;
                rte_pktmbuf_reset(m);
                _fc.put(me(m));
                m = m_next;
            }
        }

        void set_packet(packet&& p) {
            _p = std::move(p);
        }

    private:
        struct rte_mbuf _mbuf;
        MARKER private_start;
        std::optional<packet> _p;
        rte_iova_t _buf_iova;
        uint16_t _data_off;
        // TRUE if underlying mbuf has been used in the zero-copy flow
        bool _is_zc = false;
        // buffers' factory the buffer came from
        tx_buf_factory& _fc;
        MARKER private_end;
    };

    class tx_buf_factory {
        //
        // Number of buffers to free in each GC iteration:
        // We want the buffers to be allocated from the mempool as many as
        // possible.
        //
        // On the other hand if there is no Tx for some time we want the
        // completions to be eventually handled. Thus we choose the smallest
        // possible packets count number here.
        //
        static constexpr int gc_count = 1;
    public:
        tx_buf_factory(uint16_t qid) {
            using namespace memory;

            sstring name = sstring(pktmbuf_pool_name) + to_sstring(qid) + "_tx";
            printf("Creating Tx mbuf pool '%s' [%u mbufs] ...\n",
                   name.c_str(), mbufs_per_queue_tx);

            if (HugetlbfsMemBackend) {
                size_t xmem_size;

                _xmem.reset(dpdk_qp::alloc_mempool_xmem(mbufs_per_queue_tx,
                                                        inline_mbuf_size,
                                                        xmem_size));
                if (!_xmem.get()) {
                    printf("Can't allocate a memory for Tx buffers\n");
                    exit(1);
                }

                //
                // We are going to push the buffers from the mempool into
                // the circular_buffer and then poll them from there anyway, so
                // we prefer to make a mempool non-atomic in this case.
                //
                _pool =
                    rte_mempool_create_empty(name.c_str(),
                                             mbufs_per_queue_tx,
                                             inline_mbuf_size,
                                             mbuf_cache_size,
                                             sizeof(struct rte_pktmbuf_pool_private),
                                             rte_socket_id(), 0);
                if (_pool) {
                    rte_pktmbuf_pool_init(_pool, nullptr);

                    if (rte_mempool_populate_virt(_pool, (char*)(_xmem.get()),
                                                  xmem_size, page_size,
                                                  nullptr, nullptr) <= 0) {
                        printf("Failed to populate mempool for Tx\n");
                        exit(1);
                    }

                    rte_mempool_obj_iter(_pool, rte_pktmbuf_init, nullptr);
                }

            } else {
                _pool =
                    rte_mempool_create(name.c_str(),
                                       mbufs_per_queue_tx, inline_mbuf_size,
                                       mbuf_cache_size,
                                       sizeof(struct rte_pktmbuf_pool_private),
                                       rte_pktmbuf_pool_init, nullptr,
                                       rte_pktmbuf_init, nullptr,
                                       rte_socket_id(), 0);
            }

            if (!_pool) {
                printf("Failed to create mempool for Tx\n");
                exit(1);
            }

            //
            // Fill the factory with the buffers from the mempool allocated
            // above.
            //
            init_factory();
        }

        /**
         * @note Should not be called if there are no free tx_buf's
         *
         * @return a free tx_buf object
         */
        tx_buf* get() {
            // Take completed from the HW first
            tx_buf *pkt = get_one_completed();
            if (pkt) {
                if (HugetlbfsMemBackend) {
                    pkt->reset_zc();
                }

                return pkt;
            }

            //
            // If there are no completed at the moment - take from the
            // factory's cache.
            //
            if (_ring.empty()) {
                return nullptr;
            }

            pkt = _ring.back();
            _ring.pop_back();

            return pkt;
        }

        void put(tx_buf* buf) {
            if (HugetlbfsMemBackend) {
                buf->reset_zc();
            }
            _ring.push_back(buf);
        }

        bool gc() {
            for (int cnt = 0; cnt < gc_count; ++cnt) {
                auto tx_buf_p = get_one_completed();
                if (!tx_buf_p) {
                    return false;
                }

                put(tx_buf_p);
            }

            return true;
        }
    private:
        /**
         * Fill the mbufs circular buffer: after this the _pool will become
         * empty. We will use it to catch the completed buffers:
         *
         * - Underlying PMD drivers will "free" the mbufs once they are
         *   completed.
         * - We will poll the _pktmbuf_pool_tx till it's empty and release
         *   all the buffers from the freed mbufs.
         */
        void init_factory() {
            while (rte_mbuf* mbuf = rte_pktmbuf_alloc(_pool)) {
                _ring.push_back(new(tx_buf::me(mbuf)) tx_buf{*this});
            }
        }

        /**
         * PMD puts the completed buffers back into the mempool they have
         * originally come from.
         *
         * @note rte_pktmbuf_alloc() resets the mbuf so there is no need to call
         *       rte_pktmbuf_reset() here again.
         *
         * @return a single tx_buf that has been completed by HW.
         */
        tx_buf* get_one_completed() {
            return tx_buf::me(rte_pktmbuf_alloc(_pool));
        }

    private:
        std::vector<tx_buf*> _ring;
        rte_mempool* _pool = nullptr;
        std::unique_ptr<void, free_deleter> _xmem;
    };

public:
    explicit dpdk_qp(dpdk_device* dev, uint16_t qid,
                     const std::string stats_plugin_name);

    virtual void rx_start() override;
    virtual future<> send(packet p) override {
        abort();
    }
    virtual ~dpdk_qp() {}

    virtual uint32_t send(circular_buffer<packet>& pb) override {
        if (HugetlbfsMemBackend) {
            // Zero-copy send
            return _send(pb, [&] (packet&& p) {
                return tx_buf::from_packet_zc(std::move(p), *this);
            });
        } else {
            // "Copy"-send
            return _send(pb, [&](packet&& p) {
                return tx_buf::from_packet_copy(std::move(p), *this);
            });
        }
    }

    dpdk_device& port() const { return *_dev; }
    tx_buf* get_tx_buf() { return _tx_buf_factory.get(); }
private:

    template <class Func>
    uint32_t _send(circular_buffer<packet>& pb, Func packet_to_tx_buf_p) {
        if (_tx_burst.size() == 0) {
            for (auto&& p : pb) {
                // TODO: SEASTAR_ASSERT() in a fast path! Remove me ASAP!
                SEASTAR_ASSERT(p.len());

                tx_buf* buf = packet_to_tx_buf_p(std::move(p));
                if (!buf) {
                    break;
                }

                _tx_burst.push_back(buf->rte_mbuf_p());
            }
        }

        uint16_t sent = rte_eth_tx_burst(_dev->port_idx(), _qid,
                                         _tx_burst.data() + _tx_burst_idx,
                                         _tx_burst.size() - _tx_burst_idx);

        uint64_t nr_frags = 0, bytes = 0;

        for (int i = 0; i < sent; i++) {
            rte_mbuf* m = _tx_burst[_tx_burst_idx + i];
            bytes    += m->pkt_len;
            nr_frags += m->nb_segs;
            pb.pop_front();
        }

        _stats.tx.good.update_frags_stats(nr_frags, bytes);

        _tx_burst_idx += sent;

        if (_tx_burst_idx == _tx_burst.size()) {
            _tx_burst_idx = 0;
            _tx_burst.clear();
        }

        return sent;
    }

    /**
     * Allocate a new data buffer and set the mbuf to point to it.
     *
     * Do some DPDK hacks to work on PMD: it assumes that the buf_addr
     * points to the private data of RTE_PKTMBUF_HEADROOM before the actual
     * data buffer.
     *
     * @param m mbuf to update
     */
    static bool refill_rx_mbuf(rte_mbuf* m, size_t size = mbuf_data_size) {
        char* data;

        if (posix_memalign((void**)&data, size, size)) {
            return false;
        }

        rte_iova_t iova = rte_mem_virt2iova(data);

        //
        // Set the mbuf to point to our data.
        //
        // Do some DPDK hacks to work on PMD: it assumes that the buf_addr
        // points to the private data of RTE_PKTMBUF_HEADROOM before the
        // actual data buffer.
        //
        m->buf_addr      = data - RTE_PKTMBUF_HEADROOM;
        m->buf_iova      = iova - RTE_PKTMBUF_HEADROOM;
        return true;
    }

    static bool init_noninline_rx_mbuf(rte_mbuf* m,
                                       size_t size = mbuf_data_size) {
        if (!refill_rx_mbuf(m, size)) {
            return false;
        }
        // The below fields stay constant during the execution.
        m->buf_len       = size + RTE_PKTMBUF_HEADROOM;
        m->data_off      = RTE_PKTMBUF_HEADROOM;
        return true;
    }

    bool init_rx_mbuf_pool();
    bool map_dma();
    bool rx_gc();
    bool refill_one_cluster(rte_mbuf* head);

    /**
     * Allocates a memory chunk to accommodate the given number of buffers of
     * the given size and fills a vector with underlying physical pages.
     *
     * The chunk is going to be used as an external memory buffer of the DPDK
     * memory pool.
     *
     * The chunk size if calculated using get_mempool_xmem_size() function.
     *
     * @param num_bufs  Number of buffers (in)
     * @param buf_sz    Size of each buffer (in)
     * @param xmem_size Size of allocated memory chunk (out)
     *
     * @return a virtual address of the allocated memory chunk or nullptr in
     *         case of a failure.
     */
    static void* alloc_mempool_xmem(uint16_t num_bufs, uint16_t buf_sz,
                                    size_t& xmem_size);

    /**
     * Polls for a burst of incoming packets. This function will not block and
     * will immediately return after processing all available packets.
     *
     */
    bool poll_rx_once();

    /**
     * Translates an rte_mbuf's into net::packet and feeds them to _rx_stream.
     *
     * @param bufs An array of received rte_mbuf's
     * @param count Number of buffers in the bufs[]
     */
    void process_packets(struct rte_mbuf **bufs, uint16_t count);

    /**
     * Translate rte_mbuf into the "packet".
     * @param m mbuf to translate
     *
     * @return a "optional" object representing the newly received data if in an
     *         "engaged" state or an error if in a "disengaged" state.
     */
    std::optional<packet> from_mbuf(rte_mbuf* m);

    /**
     * Transform an LRO rte_mbuf cluster into the "packet" object.
     * @param m HEAD of the mbufs' cluster to transform
     *
     * @return a "optional" object representing the newly received LRO packet if
     *         in an "engaged" state or an error if in a "disengaged" state.
     */
    std::optional<packet> from_mbuf_lro(rte_mbuf* m);

private:
    dpdk_device* _dev;
    uint16_t _qid;
    rte_mempool *_pktmbuf_pool_rx;
    std::vector<rte_mbuf*> _rx_free_pkts;
    std::vector<rte_mbuf*> _rx_free_bufs;
    std::vector<fragment> _frags;
    std::vector<char*> _bufs;
    size_t _num_rx_free_segs = 0;
    internal::poller _rx_gc_poller;
    std::unique_ptr<void, free_deleter> _rx_xmem;
    tx_buf_factory _tx_buf_factory;
    std::optional<reactor::poller> _rx_poller;
    internal::poller _tx_gc_poller;
    std::vector<rte_mbuf*> _tx_burst;
    uint16_t _tx_burst_idx = 0;
    static constexpr phys_addr_t page_mask = ~(memory::page_size - 1);
};

int dpdk_device::init_port_start()
{
    SEASTAR_ASSERT(_port_idx < rte_eth_dev_count_avail());

    rte_eth_dev_info_get(_port_idx, &_dev_info);

    //
    // This is a workaround for a missing handling of a HW limitation in the
    // DPDK i40e driver. This and all related to _is_i40e_device code should be
    // removed once this handling is added.
    //
    if (sstring("rte_i40evf_pmd") == _dev_info.driver_name ||
        sstring("rte_i40e_pmd") == _dev_info.driver_name) {
        printf("Device is an Intel's 40G NIC. Enabling 8 fragments hack!\n");
        _is_i40e_device = true;
    }

    if (std::string("rte_vmxnet3_pmd") == _dev_info.driver_name) {
      printf("Device is a VMWare Virtual NIC. Enabling 16 fragments hack!\n");
      _is_vmxnet3_device = true;
    }

    //
    // Another workaround: this time for a lack of number of RSS bits.
    // ixgbe PF NICs support up to 16 RSS queues.
    // ixgbe VF NICs support up to 4 RSS queues.
    // i40e PF NICs support up to 64 RSS queues.
    // i40e VF NICs support up to 16 RSS queues.
    //
    if (sstring("rte_ixgbe_pmd") == _dev_info.driver_name) {
        _dev_info.max_rx_queues = std::min(_dev_info.max_rx_queues, (uint16_t)16);
    } else if (sstring("rte_ixgbevf_pmd") == _dev_info.driver_name) {
        _dev_info.max_rx_queues = std::min(_dev_info.max_rx_queues, (uint16_t)4);
    } else if (sstring("rte_i40e_pmd") == _dev_info.driver_name) {
        _dev_info.max_rx_queues = std::min(_dev_info.max_rx_queues, (uint16_t)64);
    } else if (sstring("rte_i40evf_pmd") == _dev_info.driver_name) {
        _dev_info.max_rx_queues = std::min(_dev_info.max_rx_queues, (uint16_t)16);
    }

    // Hardware offload capabilities
    // https://github.com/DPDK/dpdk/blob/v19.05/lib/librte_ethdev/rte_ethdev.h#L993-L1074

    // We want to support all available offload features
    // TODO: below features are implemented in 17.05, should support new ones
    const uint64_t tx_offloads_wanted =
        RTE_ETH_TX_OFFLOAD_VLAN_INSERT      |
        RTE_ETH_TX_OFFLOAD_IPV4_CKSUM       |
        RTE_ETH_TX_OFFLOAD_UDP_CKSUM        |
        RTE_ETH_TX_OFFLOAD_TCP_CKSUM        |
        RTE_ETH_TX_OFFLOAD_SCTP_CKSUM       |
        RTE_ETH_TX_OFFLOAD_TCP_TSO          |
        RTE_ETH_TX_OFFLOAD_UDP_TSO          |
        RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM |
        RTE_ETH_TX_OFFLOAD_QINQ_INSERT      |
        RTE_ETH_TX_OFFLOAD_VXLAN_TNL_TSO    |
        RTE_ETH_TX_OFFLOAD_GRE_TNL_TSO      |
        RTE_ETH_TX_OFFLOAD_IPIP_TNL_TSO     |
        RTE_ETH_TX_OFFLOAD_GENEVE_TNL_TSO   |
        RTE_ETH_TX_OFFLOAD_MACSEC_INSERT;

    _dev_info.default_txconf.offloads =
        _dev_info.tx_offload_capa & tx_offloads_wanted;

    /* for port configuration all features are off by default */
    rte_eth_conf port_conf = { 0 };

    /* setting tx offloads for port */
    port_conf.txmode.offloads = _dev_info.default_txconf.offloads;

    printf("Port %d: max_rx_queues %d max_tx_queues %d\n",
           _port_idx, _dev_info.max_rx_queues, _dev_info.max_tx_queues);

    _num_queues = std::min({_num_queues, _dev_info.max_rx_queues, _dev_info.max_tx_queues});

    printf("Port %d: using %d %s\n", _port_idx, _num_queues,
           (_num_queues > 1) ? "queues" : "queue");

    // Set RSS mode: enable RSS if seastar is configured with more than 1 CPU.
    // Even if port has a single queue we still want the RSS feature to be
    // available in order to make HW calculate RSS hash for us.
    if (smp::count > 1) {
        if (_dev_info.hash_key_size == 40) {
            _rss_key = default_rsskey_40bytes;
        } else if (_dev_info.hash_key_size == 52) {
            _rss_key = default_rsskey_52bytes;
        } else if (_dev_info.hash_key_size != 0) {
            // WTF?!!
            rte_exit(EXIT_FAILURE,
                "Port %d: We support only 40 or 52 bytes RSS hash keys, %d bytes key requested",
                _port_idx, _dev_info.hash_key_size);
        } else {
            _rss_key = default_rsskey_40bytes;
            _dev_info.hash_key_size = 40;
        }

        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        /* enable all supported rss offloads */
        port_conf.rx_adv_conf.rss_conf.rss_hf = _dev_info.flow_type_rss_offloads;
        if (_dev_info.hash_key_size) {
            port_conf.rx_adv_conf.rss_conf.rss_key = const_cast<uint8_t *>(_rss_key.data());
            port_conf.rx_adv_conf.rss_conf.rss_key_len = _dev_info.hash_key_size;
        }
    } else {
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    }

    if (_num_queues > 1) {
        if (_dev_info.reta_size) {
            // RETA size should be a power of 2
            SEASTAR_ASSERT((_dev_info.reta_size & (_dev_info.reta_size - 1)) == 0);

            // Set the RSS table to the correct size
            _redir_table.resize(_dev_info.reta_size);
            _rss_table_bits = std::lround(std::log2(_dev_info.reta_size));
            printf("Port %d: RSS table size is %d\n",
                   _port_idx, _dev_info.reta_size);
        } else {
            _rss_table_bits = std::lround(std::log2(_dev_info.max_rx_queues));
        }
    } else {
        _redir_table.push_back(0);
    }

    // Set Rx VLAN stripping
    if (_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_VLAN_STRIP) {
        port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_VLAN_STRIP;
    }

#ifdef RTE_ETHDEV_HAS_LRO_SUPPORT
    // Enable LRO
    if (_use_lro && (_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TCP_LRO)) {
        printf("LRO is on\n");
        port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TCP_LRO;
        _hw_features.rx_lro = true;
    } else
#endif
        printf("LRO is off\n");

    // Check that all CSUM features are either all set all together or not set
    // all together. If this assumption breaks we need to rework the below logic
    // by splitting the csum offload feature bit into separate bits for IPv4,
    // TCP and UDP.
    SEASTAR_ASSERT(((_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM) &&
            (_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_UDP_CKSUM) &&
            (_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TCP_CKSUM)) ||
           (!(_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM) &&
            !(_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_UDP_CKSUM) &&
            !(_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TCP_CKSUM)));

    // Set Rx checksum checking
    if (  (_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM) &&
          (_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_UDP_CKSUM) &&
          (_dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TCP_CKSUM)) {
        printf("RX checksum offload supported\n");
        port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_CHECKSUM;
        _hw_features.rx_csum_offload = 1;
    }

    if ((_dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM)) {
        printf("TX ip checksum offload supported\n");
        _hw_features.tx_csum_ip_offload = 1;
    }

    // TSO is supported starting from DPDK v1.8
    if (_dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_TCP_TSO) {
        printf("TSO is supported\n");
        _hw_features.tx_tso = 1;
    }

    // There is no UFO support in the PMDs yet.
#if 0
    if (_dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_TSO) {
        printf("UFO is supported\n");
        _hw_features.tx_ufo = 1;
    }
#endif

    // Check that Tx TCP and UDP CSUM features are either all set all together
    // or not set all together. If this assumption breaks we need to rework the
    // below logic by splitting the csum offload feature bit into separate bits
    // for TCP and UDP.
    SEASTAR_ASSERT(((_dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) &&
            (_dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_TCP_CKSUM)) ||
           (!(_dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) &&
            !(_dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_TCP_CKSUM)));

    if (  (_dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) &&
          (_dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_TCP_CKSUM)) {
        printf("TX TCP&UDP checksum offload supported\n");
        _hw_features.tx_csum_l4_offload = 1;
    }

    int retval;

    printf("Port %u init ... ", _port_idx);
    fflush(stdout);

    /*
     * Standard DPDK port initialisation - config port, then set up
     * rx and tx rings.
      */
    if ((retval = rte_eth_dev_configure(_port_idx, _num_queues, _num_queues,
                                        &port_conf)) != 0) {
        return retval;
    }

    //rte_eth_promiscuous_enable(port_num);
    printf("done: \n");

    return 0;
}

void dpdk_device::set_hw_flow_control()
{
    // Read the port's current/default flow control settings
    struct rte_eth_fc_conf fc_conf;
    auto ret = rte_eth_dev_flow_ctrl_get(_port_idx, &fc_conf);

    if (ret == -ENOTSUP) {
        goto not_supported;
    }

    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Port %u: failed to get hardware flow control settings: (error %d)\n", _port_idx, ret);
    }

    if (_enable_fc) {
        fc_conf.mode = RTE_ETH_FC_FULL;
    } else {
        fc_conf.mode = RTE_ETH_FC_NONE;
    }

    ret = rte_eth_dev_flow_ctrl_set(_port_idx, &fc_conf);
    if (ret == -ENOTSUP) {
        goto not_supported;
    }

    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Port %u: failed to set hardware flow control (error %d)\n", _port_idx, ret);
    }

    printf("Port %u: %s HW FC\n", _port_idx,
                                  (_enable_fc ? "Enabling" : "Disabling"));
    return;

not_supported:
    printf("Port %u: Changing HW FC settings is not supported\n", _port_idx);
}

void dpdk_device::init_port_fini()
{
    // Changing FC requires HW reset, so set it before the port is initialized.
    set_hw_flow_control();

    if (rte_eth_dev_start(_port_idx) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot start port %d\n", _port_idx);
    }

    /* need to defer initialize xstats since NIC specific xstat entries
       show up only after port initization */
    _xstats.start();

    _stats_collector.set_callback([&] {
        rte_eth_stats rte_stats = {};
        int rc = rte_eth_stats_get(_port_idx, &rte_stats);

        if (rc) {
            printf("Failed to get port statistics: %s\n", strerror(rc));
        }

        _stats.rx.good.mcast      =
            _xstats.get_value(dpdk_xstats::xstat_id::rx_multicast_packets);
        _stats.rx.good.pause_xon  =
            _xstats.get_value(dpdk_xstats::xstat_id::rx_xon_packets);
        _stats.rx.good.pause_xoff =
            _xstats.get_value(dpdk_xstats::xstat_id::rx_xoff_packets);

        _stats.rx.bad.crc        =
            _xstats.get_value(dpdk_xstats::xstat_id::rx_crc_errors);
        _stats.rx.bad.len         =
            _xstats.get_value(dpdk_xstats::xstat_id::rx_length_errors) +
            _xstats.get_value(dpdk_xstats::xstat_id::rx_undersize_errors) +
            _xstats.get_value(dpdk_xstats::xstat_id::rx_oversize_errors);
        _stats.rx.bad.total       = rte_stats.ierrors;

        _stats.tx.good.pause_xon  =
            _xstats.get_value(dpdk_xstats::xstat_id::tx_xon_packets);
        _stats.tx.good.pause_xoff =
            _xstats.get_value(dpdk_xstats::xstat_id::tx_xoff_packets);

        _stats.tx.bad.total       = rte_stats.oerrors;
    });

    // TODO: replace deprecated filter api with generic flow api
    if (_num_queues > 1) {
        set_rss_table();
    }

    // Wait for a link
    check_port_link_status();

    printf("Created DPDK device\n");
}

template <bool HugetlbfsMemBackend>
void* dpdk_qp<HugetlbfsMemBackend>::alloc_mempool_xmem(
    uint16_t num_bufs, uint16_t buf_sz, size_t& xmem_size)
{
    using namespace memory;
    char* xmem;
    struct rte_mempool_objsz mp_obj_sz = {};

    rte_mempool_calc_obj_size(buf_sz, 0, &mp_obj_sz);

    xmem_size =
        get_mempool_xmem_size(num_bufs,
                              mp_obj_sz.elt_size + mp_obj_sz.header_size +
                                                   mp_obj_sz.trailer_size,
                              page_bits);

    // Aligning to 2M causes the further failure in small allocations.
    // TODO: Check why - and fix.
    if (posix_memalign((void**)&xmem, page_size, xmem_size)) {
        printf("Can't allocate %ld bytes aligned to %ld\n",
               xmem_size, page_size);
        return nullptr;
    }

    return xmem;
}

template <bool HugetlbfsMemBackend>
bool dpdk_qp<HugetlbfsMemBackend>::init_rx_mbuf_pool()
{
    using namespace memory;
    sstring name = sstring(pktmbuf_pool_name) + to_sstring(_qid) + "_rx";

    printf("Creating Rx mbuf pool '%s' [%u mbufs] ...\n",
           name.c_str(), mbufs_per_queue_rx);

    //
    // If we have a hugetlbfs memory backend we may perform a virt2phys
    // translation and memory is "pinned". Therefore we may provide an external
    // memory for DPDK pools and this way significantly reduce the memory needed
    // for the DPDK in this case.
    //
    if (HugetlbfsMemBackend) {
        size_t xmem_size;

        _rx_xmem.reset(alloc_mempool_xmem(mbufs_per_queue_rx, mbuf_overhead,
                                          xmem_size));
        if (!_rx_xmem.get()) {
            printf("Can't allocate a memory for Rx buffers\n");
            return false;
        }

        //
        // Don't pass single-producer/single-consumer flags to mbuf create as it
        // seems faster to use a cache instead.
        //
        struct rte_pktmbuf_pool_private roomsz = {};
        roomsz.mbuf_data_room_size = mbuf_data_size + RTE_PKTMBUF_HEADROOM;
        _pktmbuf_pool_rx =
            rte_mempool_create_empty(name.c_str(),
                                     mbufs_per_queue_rx, mbuf_overhead,
                                     mbuf_cache_size,
                                     sizeof(struct rte_pktmbuf_pool_private),
                                     rte_socket_id(), 0);
        if (!_pktmbuf_pool_rx) {
            printf("Failed to create mempool for Rx\n");
            exit(1);
        }

        rte_pktmbuf_pool_init(_pktmbuf_pool_rx, as_cookie(roomsz));

        if (rte_mempool_populate_virt(_pktmbuf_pool_rx,
                                      (char*)(_rx_xmem.get()), xmem_size,
                                      page_size,
                                      nullptr, nullptr) < 0) {
            printf("Failed to populate mempool for Rx\n");
            exit(1);
        }

        rte_mempool_obj_iter(_pktmbuf_pool_rx, rte_pktmbuf_init, nullptr);

        // reserve the memory for Rx buffers containers
        _rx_free_pkts.reserve(mbufs_per_queue_rx);
        _rx_free_bufs.reserve(mbufs_per_queue_rx);

        //
        // 1) Pull all entries from the pool.
        // 2) Bind data buffers to each of them.
        // 3) Return them back to the pool.
        //
        for (int i = 0; i < mbufs_per_queue_rx; i++) {
            rte_mbuf* m = rte_pktmbuf_alloc(_pktmbuf_pool_rx);
            SEASTAR_ASSERT(m);
            _rx_free_bufs.push_back(m);
        }

        for (auto&& m : _rx_free_bufs) {
            if (!init_noninline_rx_mbuf(m)) {
                printf("Failed to allocate data buffers for Rx ring. "
                       "Consider increasing the amount of memory.\n");
                exit(1);
            }
        }

        rte_mempool_put_bulk(_pktmbuf_pool_rx, (void**)_rx_free_bufs.data(),
                             _rx_free_bufs.size());

        _rx_free_bufs.clear();
    } else {
        struct rte_pktmbuf_pool_private roomsz = {};
        roomsz.mbuf_data_room_size = inline_mbuf_data_size + RTE_PKTMBUF_HEADROOM;
        _pktmbuf_pool_rx =
            rte_mempool_create(name.c_str(),
                               mbufs_per_queue_rx, inline_mbuf_size,
                               mbuf_cache_size,
                               sizeof(struct rte_pktmbuf_pool_private),
                               rte_pktmbuf_pool_init, as_cookie(roomsz),
                               rte_pktmbuf_init, nullptr,
                               rte_socket_id(), 0);
    }

    return _pktmbuf_pool_rx != nullptr;
}

// Map DMA address explicitly.
// XXX: does NOT work with Mellanox NICs as they use IB libs instead of VFIO.
template <bool HugetlbfsMemBackend>
bool dpdk_qp<HugetlbfsMemBackend>::map_dma()
{
    auto m = memory::get_memory_layout();
    rte_iova_t iova = rte_mem_virt2iova((const void*)m.start);

    return rte_vfio_container_dma_map(RTE_VFIO_DEFAULT_CONTAINER_FD,
                                      m.start, iova, m.end - m.start) == 0;
}

void dpdk_device::check_port_link_status()
{
    using namespace std::literals::chrono_literals;
    int count = 0;
    constexpr auto check_interval = 100ms;

    std::cout << "\nChecking link status " << std::endl;
    auto t = new timer<>;
    t->set_callback([this, count, t] () mutable {
        const int max_check_time = 90;  /* 9s (90 * 100ms) in total */
        struct rte_eth_link link;
        memset(&link, 0, sizeof(link));
        rte_eth_link_get_nowait(_port_idx, &link);

        if (link.link_status) {
            std::cout <<
                "done\nPort " << static_cast<unsigned>(_port_idx) <<
                " Link Up - speed " << link.link_speed <<
                " Mbps - " << ((link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ?
                          ("full-duplex") : ("half-duplex\n")) <<
                std::endl;
            _link_ready_promise.set_value();

            // We may start collecting statistics only after the Link is UP.
            _stats_collector.arm_periodic(2s);
        } else if (count++ < max_check_time) {
             std::cout << "." << std::flush;
             return;
        } else {
            std::cout << "done\nPort " << _port_idx << " Link Down" << std::endl;
        }
        t->cancel();
        delete t;
    });
    t->arm_periodic(check_interval);
}

// This function uses offsetof with non POD types.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

template <bool HugetlbfsMemBackend>
dpdk_qp<HugetlbfsMemBackend>::dpdk_qp(dpdk_device* dev, uint16_t qid,
                                      const std::string stats_plugin_name)
     : qp(true, stats_plugin_name, qid), _dev(dev), _qid(qid),
       _rx_gc_poller(reactor::poller::simple([&] { return rx_gc(); })),
       _tx_buf_factory(qid),
       _tx_gc_poller(reactor::poller::simple([&] { return _tx_buf_factory.gc(); }))
{
    if (!init_rx_mbuf_pool()) {
        rte_exit(EXIT_FAILURE, "Cannot initialize mbuf pools\n");
    }

    if (HugetlbfsMemBackend && !map_dma()) {
        rte_exit(EXIT_FAILURE, "Cannot map DMA\n");
    }

    static_assert(offsetof(class tx_buf, private_end) -
                  offsetof(class tx_buf, private_start) <= RTE_PKTMBUF_HEADROOM,
                  "RTE_PKTMBUF_HEADROOM is less than dpdk_qp::tx_buf size! "
                  "Increase the headroom size in the DPDK configuration");
    static_assert(offsetof(class tx_buf, _mbuf) == 0,
                  "There is a pad at the beginning of the tx_buf before _mbuf "
                  "field!");
    static_assert((inline_mbuf_data_size & (inline_mbuf_data_size - 1)) == 0,
                  "inline_mbuf_data_size has to be a power of two!");

    if (rte_eth_rx_queue_setup(_dev->port_idx(), _qid, default_ring_size,
            rte_eth_dev_socket_id(_dev->port_idx()),
            _dev->def_rx_conf(), _pktmbuf_pool_rx) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot initialize rx queue\n");
    }

    if (rte_eth_tx_queue_setup(_dev->port_idx(), _qid, default_ring_size,
            rte_eth_dev_socket_id(_dev->port_idx()), _dev->def_tx_conf()) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot initialize tx queue\n");
    }

    // Register error statistics: Rx total and checksum errors
    namespace sm = seastar::metrics;
    _metrics.add_group(_stats_plugin_name, {
        sm::make_counter(_queue_name + "_rx_csum_errors", _stats.rx.bad.csum,
                        sm::description("Counts a number of packets received by this queue that have a bad CSUM value. "
                                        "A non-zero value of this metric usually indicates a HW issue, e.g. a bad cable.")),

        sm::make_counter(_queue_name + "_rx_errors", _stats.rx.bad.total,
                        sm::description("Counts a total number of errors in the ingress path for this queue: CSUM errors, etc.")),

        sm::make_counter(_queue_name + "_rx_no_memory_errors", _stats.rx.bad.no_mem,
                        sm::description("Counts a number of ingress packets received by this HW queue but dropped by the SW due to low memory. "
                                        "A non-zero value indicates that seastar doesn't have enough memory to handle the packet reception or the memory is too fragmented.")),
    });
}

#pragma GCC diagnostic pop

template <bool HugetlbfsMemBackend>
void dpdk_qp<HugetlbfsMemBackend>::rx_start() {
    _rx_poller = reactor::poller::simple([&] { return poll_rx_once(); });
}

template<>
inline std::optional<packet>
dpdk_qp<false>::from_mbuf_lro(rte_mbuf* m)
{
    //
    // Try to allocate a buffer for the whole packet's data.
    // If we fail - construct the packet from mbufs.
    // If we succeed - copy the data into this buffer, create a packet based on
    // this buffer and return the mbuf to its pool.
    //
    auto pkt_len = rte_pktmbuf_pkt_len(m);
    char* buf = (char*)malloc(pkt_len);
    if (buf) {
        // Copy the contents of the packet into the buffer we've just allocated
        size_t offset = 0;
        for (rte_mbuf* m1 = m; m1 != nullptr; m1 = m1->next) {
            char* data = rte_pktmbuf_mtod(m1, char*);
            auto len = rte_pktmbuf_data_len(m1);

            rte_memcpy(buf + offset, data, len);
            offset += len;
        }

        rte_pktmbuf_free(m);

        return packet(fragment{buf, pkt_len}, make_free_deleter(buf));
    }

    // Drop if allocation failed
    rte_pktmbuf_free(m);

    return std::nullopt;
}

template<>
inline std::optional<packet>
dpdk_qp<false>::from_mbuf(rte_mbuf* m)
{
    if (!_dev->hw_features_ref().rx_lro || rte_pktmbuf_is_contiguous(m)) {
        //
        // Try to allocate a buffer for packet's data. If we fail - give the
        // application an mbuf itself. If we succeed - copy the data into this
        // buffer, create a packet based on this buffer and return the mbuf to
        // its pool.
        //
        auto len = rte_pktmbuf_data_len(m);
        char* buf = (char*)malloc(len);

        if (!buf) {
            // Drop if allocation failed
            rte_pktmbuf_free(m);

            return std::nullopt;
        } else {
            rte_memcpy(buf, rte_pktmbuf_mtod(m, char*), len);
            rte_pktmbuf_free(m);

            return packet(fragment{buf, len}, make_free_deleter(buf));
        }
    } else {
        return from_mbuf_lro(m);
    }
}

template<>
inline std::optional<packet>
dpdk_qp<true>::from_mbuf_lro(rte_mbuf* m)
{
    _frags.clear();
    _bufs.clear();

    for (; m != nullptr; m = m->next) {
        char* data = rte_pktmbuf_mtod(m, char*);

        _frags.emplace_back(fragment{data, rte_pktmbuf_data_len(m)});
        _bufs.push_back(data);
    }

    return packet(_frags.begin(), _frags.end(),
                  make_deleter(deleter(),
                          [bufs_vec = std::move(_bufs)] {
                              for (auto&& b : bufs_vec) {
                                  free(b);
                              }
                          }));
}

template<>
inline std::optional<packet> dpdk_qp<true>::from_mbuf(rte_mbuf* m)
{
    _rx_free_pkts.push_back(m);
    _num_rx_free_segs += m->nb_segs;

    if (!_dev->hw_features_ref().rx_lro || rte_pktmbuf_is_contiguous(m)) {
        char* data = rte_pktmbuf_mtod(m, char*);

        return packet(fragment{data, rte_pktmbuf_data_len(m)},
                      make_free_deleter(data));
    } else {
        return from_mbuf_lro(m);
    }
}

template <bool HugetlbfsMemBackend>
inline bool dpdk_qp<HugetlbfsMemBackend>::refill_one_cluster(rte_mbuf* head)
{
    for (; head != nullptr; head = head->next) {
        if (!refill_rx_mbuf(head)) {
            //
            // If we failed to allocate a new buffer - push the rest of the
            // cluster back to the free_packets list for a later retry.
            //
            _rx_free_pkts.push_back(head);
            return false;
        }
        _rx_free_bufs.push_back(head);
    }

    return true;
}

template <bool HugetlbfsMemBackend>
bool dpdk_qp<HugetlbfsMemBackend>::rx_gc()
{
    if (_num_rx_free_segs >= rx_gc_thresh) {
        while (!_rx_free_pkts.empty()) {
            //
            // Use back() + pop_back() semantics to avoid an extra
            // _rx_free_pkts.clear() at the end of the function - clear() has a
            // linear complexity.
            //
            auto m = _rx_free_pkts.back();
            _rx_free_pkts.pop_back();

            if (!refill_one_cluster(m)) {
                break;
            }
        }

        if (_rx_free_bufs.size()) {
            rte_mempool_put_bulk(_pktmbuf_pool_rx,
                                 (void **)_rx_free_bufs.data(),
                                 _rx_free_bufs.size());

            // TODO: SEASTAR_ASSERT() in a fast path! Remove me ASAP!
            SEASTAR_ASSERT(_num_rx_free_segs >= _rx_free_bufs.size());

            _num_rx_free_segs -= _rx_free_bufs.size();
            _rx_free_bufs.clear();

            // TODO: SEASTAR_ASSERT() in a fast path! Remove me ASAP!
            SEASTAR_ASSERT((_rx_free_pkts.empty() && !_num_rx_free_segs) ||
                   (!_rx_free_pkts.empty() && _num_rx_free_segs));
        }
    }

    return _num_rx_free_segs >= rx_gc_thresh;
}


template <bool HugetlbfsMemBackend>
void dpdk_qp<HugetlbfsMemBackend>::process_packets(
    struct rte_mbuf **bufs, uint16_t count)
{
    uint64_t nr_frags = 0, bytes = 0;

    for (uint16_t i = 0; i < count; i++) {
        struct rte_mbuf *m = bufs[i];
        offload_info oi;

        std::optional<packet> p = from_mbuf(m);

        // Drop the packet if translation above has failed
        if (!p) {
            _stats.rx.bad.inc_no_mem();
            continue;
        }

        nr_frags += m->nb_segs;
        bytes    += m->pkt_len;

        // Set stripped VLAN value if available
        if ((m->ol_flags & RTE_MBUF_F_RX_VLAN_STRIPPED) &&
            (m->ol_flags & RTE_MBUF_F_RX_VLAN)) {

            oi.vlan_tci = m->vlan_tci;
        }

        if (_dev->hw_features().rx_csum_offload) {
            if (m->ol_flags & (RTE_MBUF_F_RX_IP_CKSUM_BAD | RTE_MBUF_F_RX_L4_CKSUM_BAD)) {
                // Packet with bad checksum, just drop it.
                _stats.rx.bad.inc_csum_err();
                continue;
            }
            // Note that when _hw_features.rx_csum_offload is on, the receive
            // code for ip, tcp and udp will assume they don't need to check
            // the checksum again, because we did this here.
        }

        (*p).set_offload_info(oi);
        if (m->ol_flags & RTE_MBUF_F_RX_RSS_HASH) {
            (*p).set_rss_hash(m->hash.rss);
        }

        _dev->l2receive(std::move(*p));
    }

    _stats.rx.good.update_pkts_bunch(count);
    _stats.rx.good.update_frags_stats(nr_frags, bytes);

    if (!HugetlbfsMemBackend) {
        _stats.rx.good.copy_frags = _stats.rx.good.nr_frags;
        _stats.rx.good.copy_bytes = _stats.rx.good.bytes;
    }
}

template <bool HugetlbfsMemBackend>
bool dpdk_qp<HugetlbfsMemBackend>::poll_rx_once()
{
    struct rte_mbuf *buf[packet_read_size];

    /* read a port */
    uint16_t rx_count = rte_eth_rx_burst(_dev->port_idx(), _qid,
                                         buf, packet_read_size);

    /* Now process the NIC packets read */
    if (likely(rx_count > 0)) {
        process_packets(buf, rx_count);
    }

    return rx_count;
}

void dpdk_device::set_rss_table()
{
    if (_dev_info.reta_size == 0)
        return;

    int reta_conf_size =
        std::max(1, _dev_info.reta_size / RTE_ETH_RETA_GROUP_SIZE);
    std::vector<rte_eth_rss_reta_entry64> reta_conf(reta_conf_size);

    // Configure the HW indirection table
    unsigned i = 0;
    for (auto& x : reta_conf) {
        x.mask = ~0ULL;
        for (auto& r: x.reta) {
            r = i++ % _num_queues;
        }
    }

    if (rte_eth_dev_rss_reta_update(_port_idx, reta_conf.data(), _dev_info.reta_size)) {
        rte_exit(EXIT_FAILURE, "Port %d: Failed to update an RSS indirection table", _port_idx);
    }

    // Fill our local indirection table. Make it in a separate loop to keep things simple.
    i = 0;
    for (auto& r : _redir_table) {
        r = i++ % _num_queues;
    }
}

std::unique_ptr<qp> dpdk_device::init_local_queue(const program_options::option_group& opts, uint16_t qid) {
    auto net_opts = dynamic_cast<const net::native_stack_options*>(&opts);
    SEASTAR_ASSERT(net_opts);

    std::unique_ptr<qp> qp;
    if (net_opts->_hugepages) {
        qp = std::make_unique<dpdk_qp<true>>(this, qid,
                                 _stats_plugin_name + "-" + _stats_plugin_inst);
    } else {
        qp = std::make_unique<dpdk_qp<false>>(this, qid,
                                 _stats_plugin_name + "-" + _stats_plugin_inst);
    }

    // FIXME: future is discarded
    (void)smp::submit_to(_home_cpu, [this] () mutable {
        if (++_queues_ready == _num_queues) {
            init_port_fini();
        }
    });
    return qp;
}
} // namespace dpdk

/******************************** Interface functions *************************/

std::unique_ptr<net::device> create_dpdk_net_device(
                                    uint16_t port_idx,
                                    uint16_t num_queues,
                                    bool use_lro,
                                    bool enable_fc)
{
    static bool called = false;

    SEASTAR_ASSERT(!called);
    SEASTAR_ASSERT(dpdk::eal::initialized);

    called = true;

    // Check that we have at least one DPDK-able port
    if (rte_eth_dev_count_avail() == 0) {
        rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");
    } else {
        printf("ports number: %d\n", rte_eth_dev_count_avail());
    }

    return std::make_unique<dpdk::dpdk_device>(port_idx, num_queues, use_lro,
                                               enable_fc);
}

std::unique_ptr<net::device> create_dpdk_net_device(
                                    const hw_config& hw_cfg)
{
    return create_dpdk_net_device(*hw_cfg.port_index, smp::count, hw_cfg.lro, hw_cfg.hw_fc);
}

}

#else

#ifdef SEASTAR_MODULE
module;
#endif

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/net/dpdk.hh>
#endif

#endif // SEASTAR_HAVE_DPDK

namespace seastar::net {

dpdk_options::dpdk_options(program_options::option_group* parent_group)
#ifdef SEASTAR_HAVE_DPDK
    : program_options::option_group(parent_group, "DPDK net options")
    , dpdk_port_index(*this, "dpdk-port-index",
                0,
                "DPDK Port Index")
    , hw_fc(*this, "hw-fc",
                "on",
                "Enable HW Flow Control (on / off)")
#else
    : program_options::option_group(parent_group, "DPDK net options", program_options::unused{})
    , dpdk_port_index(*this, "dpdk-port-index", program_options::unused{})
    , hw_fc(*this, "hw-fc", program_options::unused{})
#endif
#if 0
    opts.add_options()
        ("csum-offload",
                boost::program_options::value<std::string>()->default_value("on"),
                "Enable checksum offload feature (on / off)")
        ("tso",
                boost::program_options::value<std::string>()->default_value("on"),
                "Enable TCP segment offload feature (on / off)")
        ("ufo",
                boost::program_options::value<std::string>()->default_value("on"),
                "Enable UDP fragmentation offload feature (on / off)")
        ;
#endif
{
}

}
