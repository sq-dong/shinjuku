// dpdk_client.cpp
#include "dpdk_client.h"
#include <rte_thash.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <arpa/inet.h>
#include <cmath>

static const double TSC_HZ_SERVER = 2799.98e6;
static inline uint64_t cycles_to_ns_server(uint64_t cy) {
    return static_cast<uint64_t>(static_cast<double>(cy) * 1e9 / TSC_HZ_SERVER);
}

static inline uint64_t tsc_to_ns(uint64_t tsc, double tsc_hz) {
    return static_cast<uint64_t>(tsc * 1e9 / tsc_hz);
}

static uint8_t rss_hash_key[RSS_HASH_KEY_LENGTH] = {
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A
};

uint16_t compute_rss_port_for_queue(uint16_t target_queue, uint16_t num_rx_queues,
                                    uint16_t server_port) {
    const uint32_t server_ip = 5 | (200 << 8) | (168 << 16) | (192 << 24);
    const uint32_t client_ip = 137 | (200 << 8) | (168 << 16) | (192 << 24);

    union rte_thash_tuple tuple;
    tuple.v4.src_addr = server_ip;
    tuple.v4.dst_addr = client_ip;
    tuple.v4.sport = server_port;
    tuple.v4.dport = 0;

    for (uint32_t client_port = 1024; client_port <= 65535; client_port++) {
        tuple.v4.dport = client_port;
        uint32_t hash = rte_softrss((uint32_t*)&tuple, RTE_THASH_V4_L4_LEN, rss_hash_key);
        uint16_t queue = hash % num_rx_queues;
        if (queue == target_queue) {
            return static_cast<uint16_t>(client_port);
        }
    }
    return 10000 + target_queue * 100;
}

// ============ DpdkPortManager ============
DpdkPortManager* DpdkPortManager::instance_ = nullptr;
std::mutex DpdkPortManager::init_mutex_;

DpdkPortManager* DpdkPortManager::getInstance() {
    if (!instance_) {
        std::lock_guard<std::mutex> lock(init_mutex_);
        if (!instance_) {
            instance_ = new DpdkPortManager();
        }
    }
    return instance_;
}

void DpdkPortManager::destroyInstance() {
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (instance_) {
        delete instance_;
        instance_ = nullptr;
    }
}

DpdkPortManager::~DpdkPortManager() {
    if (initialized_) {
        if (port_id_ < RTE_MAX_ETHPORTS) {
            rte_eth_dev_stop(port_id_);
            rte_eth_dev_close(port_id_);
        }
        for (struct rte_mempool* pool : rx_mbuf_pools_) {
            if (pool)
                rte_mempool_free(pool);
        }
        rx_mbuf_pools_.clear();
        for (struct rte_mempool* pool : tx_mbuf_pools_) {
            if (pool)
                rte_mempool_free(pool);
        }
        tx_mbuf_pools_.clear();
        initialized_ = false;
    }
}

bool DpdkPortManager::initMemPool(uint16_t num_rx_queues, uint16_t num_tx_queues) {
    rx_mbuf_pools_.resize(num_rx_queues, nullptr);
    for (uint16_t qid = 0; qid < num_rx_queues; qid++) {
        char pool_name[32];
        snprintf(pool_name, sizeof(pool_name), "mbuf_rx_%u_%u", port_id_, qid);
        struct rte_mempool* pool = rte_pktmbuf_pool_create(pool_name, NUM_MBUFS_RX,
                                                           MBUF_CACHE_SIZE, 0,
                                                           RTE_MBUF_DEFAULT_BUF_SIZE,
                                                           rte_socket_id());
        if (!pool) {
            std::cerr << "Cannot create RX mbuf pool " << qid << " for port " << static_cast<int>(port_id_) << std::endl;
            for (struct rte_mempool* p : rx_mbuf_pools_) {
                if (p) rte_mempool_free(p);
            }
            rx_mbuf_pools_.clear();
            return false;
        }
        rx_mbuf_pools_[qid] = pool;
    }

    tx_mbuf_pools_.resize(num_tx_queues, nullptr);
    for (uint16_t qid = 0; qid < num_tx_queues; qid++) {
        char pool_name[32];
        snprintf(pool_name, sizeof(pool_name), "mbuf_tx_%u_%u", port_id_, qid);
        struct rte_mempool* pool = rte_pktmbuf_pool_create(pool_name, NUM_MBUFS,
                                                           MBUF_CACHE_SIZE, 0,
                                                           RTE_MBUF_DEFAULT_BUF_SIZE,
                                                           rte_socket_id());
        if (!pool) {
            std::cerr << "Cannot create TX mbuf pool " << qid << " for port " << static_cast<int>(port_id_) << std::endl;
            for (struct rte_mempool* p : rx_mbuf_pools_) { if (p) rte_mempool_free(p); }
            rx_mbuf_pools_.clear();
            for (struct rte_mempool* p : tx_mbuf_pools_) {
                if (p) rte_mempool_free(p);
            }
            tx_mbuf_pools_.clear();
            return false;
        }
        tx_mbuf_pools_[qid] = pool;
    }
    return true;
}

struct rte_mempool* DpdkPortManager::getMbufPool(uint16_t queue_id) const {
    if (queue_id >= tx_mbuf_pools_.size())
        return tx_mbuf_pools_.empty() ? nullptr : tx_mbuf_pools_[0];
    return tx_mbuf_pools_[queue_id];
}

bool DpdkPortManager::configurePort(uint16_t num_rx_queues, uint16_t num_tx_queues) {
    struct rte_eth_conf port_conf = {};
    struct rte_eth_dev_info dev_info;
    
    rte_eth_dev_info_get(port_id_, &dev_info);
    
    num_rx_queues_ = std::min(num_rx_queues, dev_info.max_rx_queues);
    num_tx_queues_ = std::min(num_tx_queues, dev_info.max_tx_queues);
    
    std::cout << "[DPDK] Configuring port with " << num_rx_queues_ 
              << " RX queues and " << num_tx_queues_ << " TX queues" << std::endl;

    memset(&port_conf, 0, sizeof(port_conf));

    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    port_conf.rxmode.max_lro_pkt_size = RTE_ETHER_MAX_LEN;
    port_conf.rxmode.mtu = RTE_ETHER_MTU;
    port_conf.rxmode.offloads = dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_CHECKSUM;

    port_conf.rx_adv_conf.rss_conf.rss_key = rss_hash_key;
    port_conf.rx_adv_conf.rss_conf.rss_key_len = RSS_HASH_KEY_LENGTH;
    port_conf.rx_adv_conf.rss_conf.rss_hf = RSS_HF;
    
    int ret = rte_eth_dev_configure(port_id_, num_rx_queues_, num_tx_queues_, &port_conf);
    if (ret < 0) {
        std::cerr << "Cannot configure device: err=" << ret << std::endl;
        return false;
    }

    struct rte_eth_rxconf rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = port_conf.rxmode.offloads;
    
    for (uint16_t qid = 0; qid < num_rx_queues_; qid++) {
        ret = rte_eth_rx_queue_setup(port_id_, qid, RX_RING_SIZE,
                                    rte_eth_dev_socket_id(port_id_),
                                    &rxq_conf, rx_mbuf_pools_[qid]);
        if (ret < 0) {
            std::cerr << "RX queue " << qid << " setup failed: err=" << ret << std::endl;
            return false;
        }
    }

    struct rte_eth_txconf txq_conf = dev_info.default_txconf;
    txq_conf.offloads = port_conf.txmode.offloads;
    
    for (uint16_t qid = 0; qid < num_tx_queues_; qid++) {
        ret = rte_eth_tx_queue_setup(port_id_, qid, TX_RING_SIZE,
                                    rte_eth_dev_socket_id(port_id_),
                                    &txq_conf);
        if (ret < 0) {
            std::cerr << "TX queue " << qid << " setup failed: err=" << ret << std::endl;
            return false;
        }
    }

    return true;
}

bool DpdkPortManager::initialize(uint16_t num_rx_queues, uint16_t num_tx_queues) {
    if (initialized_) {
        return true;
    }

    int nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        std::cerr << "No Ethernet ports available" << std::endl;
        return false;
    }
    
    if (port_id_ >= nb_ports) {
        std::cerr << "Port " << static_cast<int>(port_id_) << " not available" << std::endl;
        return false;
    }
 
    if (!initMemPool(num_rx_queues, num_tx_queues)) {
        return false;
    }

    if (!configurePort(num_rx_queues, num_tx_queues)) {
        return false;
    }

    int ret = rte_eth_dev_start(port_id_);
    if (ret < 0) {
        std::cerr << "Failed to start port " << static_cast<int>(port_id_) << ": " << ret << std::endl;
        return false;
    }

    struct rte_eth_dev_info dev_info2;
    rte_eth_dev_info_get(port_id_, &dev_info2);
    uint16_t reta_size = dev_info2.reta_size;
    if (reta_size > 0) {
        uint16_t nb_entries = std::min(static_cast<uint16_t>(reta_size),
                                       static_cast<uint16_t>(RTE_ETH_RSS_RETA_SIZE_512));
        struct rte_eth_rss_reta_entry64 reta_conf[RTE_ETH_RSS_RETA_SIZE_512 / RTE_ETH_RETA_GROUP_SIZE];
        memset(reta_conf, 0, sizeof(reta_conf));
        for (uint16_t i = 0; i < nb_entries; i++) {
            uint16_t group = i / RTE_ETH_RETA_GROUP_SIZE;
            uint16_t idx = i % RTE_ETH_RETA_GROUP_SIZE;
            reta_conf[group].reta[idx] = i % num_rx_queues_;
            reta_conf[group].mask |= (1ULL << idx);
        }
        ret = rte_eth_dev_rss_reta_update(port_id_, reta_conf, nb_entries);
        if (ret < 0) {
            std::cerr << "RETA update failed: err=" << ret << std::endl;
            rte_eth_dev_stop(port_id_);
            return false;
        }
    }

    rte_eth_promiscuous_enable(port_id_);

    struct rte_eth_link link;
    rte_eth_link_get(port_id_, &link);

    struct rte_eth_dev_info dev_info;

    rte_eth_dev_info_get(port_id_, &dev_info);
    std::cout << "Max RX queues: " << dev_info.max_rx_queues << std::endl;
    std::cout << "Max TX queues: " << dev_info.max_tx_queues << std::endl;
    
    std::cout << "[DPDK] Port " << static_cast<int>(port_id_) << " initialized: "
              << (link.link_status ? "UP" : "DOWN") << " "
              << link.link_speed << " Mbps" << std::endl;
    
    initialized_ = true;
    return true;
}

// ============ DpdkLatencyClient ============
DpdkLatencyClient::DpdkLatencyClient(uint16_t server_port, double qps, uint64_t work_ns,
                                   uint16_t thread_id, uint16_t client_port)
    : server_port_(server_port), qps_(qps), work_ns_(work_ns), thread_id_(thread_id),
      exp_dist_(qps_ * 1e-9) {
    
    rng_.seed(static_cast<unsigned>(rte_get_tsc_cycles() & 0xFFFFFFFFu));

    tsc_hz_ = rte_get_tsc_hz();
 
    DpdkPortManager* port_mgr = DpdkPortManager::getInstance();
    port_id_ = port_mgr->getPortId();
    num_rx_queues_ = port_mgr->getNumRxQueues();
    num_tx_queues_ = port_mgr->getNumTxQueues();
    mbuf_pool_ = port_mgr->getMbufPool(thread_id_ % num_tx_queues_);

    latency_stats_.count = 0;
    latency_stats_.total_ns = 0;
    latency_stats_.min_ns = UINT64_MAX;
    latency_stats_.max_ns = 0;
    latency_stats_.p50_ns = 0;
    latency_stats_.p90_ns = 0;
    latency_stats_.p95_ns = 0;
    latency_stats_.p99_ns = 0;
    latency_stats_.p999_ns = 0;
    
    // client MAC: 00:1b:21:bc:d6:7d
    client_mac_[0] = 0x00;
    client_mac_[1] = 0x1b;
    client_mac_[2] = 0x21;
    client_mac_[3] = 0xbc;
    client_mac_[4] = 0xd6;
    client_mac_[5] = 0x7d;
    
    // server MAC: 00:1b:21:bc:d6:0f
    server_mac_[0] = 0x00;
    server_mac_[1] = 0x1b;
    server_mac_[2] = 0x21;
    server_mac_[3] = 0xbc;
    server_mac_[4] = 0xd6;
    server_mac_[5] = 0x0f;
    
    // client IP: 192.168.200.137
    client_ip_ = 137 | (200 << 8) | (168 << 16) | (192 << 24);
    
    // server IP: 192.168.200.5
    server_ip_ = 5 | (200 << 8) | (168 << 16) | (192 << 24);

    if (client_port == 0) {
        static std::atomic<uint32_t> next_port_base(10000);
        client_port_ = next_port_base.fetch_add(1000) + thread_id;
    } else {
        client_port_ = client_port;
    }
    
    if (client_port_ > 60000) {
        client_port_ = 1024 + thread_id;
    }
}

DpdkLatencyClient::~DpdkLatencyClient() {
    for (Request* r : req_pool_)
        delete r;
    req_pool_.clear();
}

bool DpdkLatencyClient::initialize() {
    DpdkPortManager* port_mgr = DpdkPortManager::getInstance();
    if (!port_mgr->isInitialized()) {
        snprintf(error_msg_, sizeof(error_msg_), "DPDK port not initialized");
        return false;
    }
    
    return true;
}

Request* DpdkLatencyClient::startReq() {
    Request* req;
    if (!req_pool_.empty()) {
        req = req_pool_.back();
        req_pool_.pop_back();
    } else {
        req = new Request();
    }
    uint64_t now_ns = tsc_to_ns(rte_get_tsc_cycles(), tsc_hz_);
    if (next_gen_ns_ == 0) {
        next_gen_ns_ = now_ns;
    }
    while (tsc_to_ns(rte_get_tsc_cycles(), tsc_hz_) < next_gen_ns_)
        rte_pause();
    req->genNs = next_gen_ns_;
    req->runNs = work_ns_;
    next_gen_ns_ += static_cast<uint64_t>(exp_dist_(rng_));
    return req;
}

int DpdkLatencyClient::prepareBatch(Request* reqs[], int max_n) {
    uint64_t now_ns = tsc_to_ns(rte_get_tsc_cycles(), tsc_hz_);
    if (next_gen_ns_ == 0) {
        next_gen_ns_ = now_ns;
    }
    int n = 0;
    while (n < max_n && now_ns >= next_gen_ns_) {
        Request* req;
        if (!req_pool_.empty()) {
            req = req_pool_.back();
            req_pool_.pop_back();
        } else {
            req = new Request();
        }
        req->genNs = next_gen_ns_;
        req->runNs = work_ns_;
        next_gen_ns_ += static_cast<uint64_t>(exp_dist_(rng_));
        reqs[n++] = req;
        now_ns = tsc_to_ns(rte_get_tsc_cycles(), tsc_hz_);
    }
    if (n == 0) {
        while (tsc_to_ns(rte_get_tsc_cycles(), tsc_hz_) < next_gen_ns_)
            rte_pause();
        return prepareBatch(reqs, max_n);
    }
    return n;
}

int DpdkLatencyClient::sendBatch(Request* reqs[], int n, int max_retries) {
    if (n <= 0) return 0;
    struct rte_mbuf* mbufs[BURST_SIZE];
    uint16_t tx_queue_id = getTxQueueId();

    int created = 0;
    for (int i = 0; i < n; i++) {
        struct rte_mbuf* m = createPacket(reqs[i]);
        if (!m) {
            for (int j = 0; j < i; j++) {
                rte_pktmbuf_free(mbufs[j]);
                req_pool_.push_back(reqs[j]);
            }
            req_pool_.push_back(reqs[i]);
            return 0;
        }
        mbufs[created++] = m;
    }

    uint16_t nb_tx = rte_eth_tx_burst(port_id_, tx_queue_id, mbufs, created);
    for (int i = 0; i < (int)nb_tx; i++)
        req_pool_.push_back(reqs[i]);
    for (uint16_t i = nb_tx; i < (uint16_t)created; i++) {
        rte_pktmbuf_free(mbufs[i]);
        req_pool_.push_back(reqs[i]);
    }
    packets_sent_ += nb_tx;
    return nb_tx;
}

struct rte_mbuf* DpdkLatencyClient::createPacket(const Request* req) {
    struct rte_mbuf* m = rte_pktmbuf_alloc(mbuf_pool_);
    if (!m) {
        snprintf(error_msg_, sizeof(error_msg_), "Cannot allocate mbuf");
        return nullptr;
    }

    const uint16_t payload_len = sizeof(Request); 
    const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len; 
    const uint16_t ip_len = sizeof(struct rte_ipv4_hdr) + udp_len;
    const uint16_t total_len = sizeof(struct rte_ether_hdr) + ip_len; 
    
    uint8_t* pkt_data = (uint8_t*)rte_pktmbuf_append(m, total_len);
    if (!pkt_data) {
        rte_pktmbuf_free(m);
        snprintf(error_msg_, sizeof(error_msg_), "Cannot append data to mbuf");
        return nullptr;
    }
    memset(pkt_data, 0, total_len);
    
    // ========== Ethernet header (14 bytes) ==========
    struct rte_ether_hdr* eth_hdr = reinterpret_cast<struct rte_ether_hdr*>(pkt_data);
    memcpy(&eth_hdr->dst_addr, server_mac_, RTE_ETHER_ADDR_LEN);
    memcpy(&eth_hdr->src_addr, client_mac_, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    
    // ========== IPv4 header (20 bytes) ==========
    struct rte_ipv4_hdr* ip_hdr = reinterpret_cast<struct rte_ipv4_hdr*>(
        pkt_data + sizeof(struct rte_ether_hdr));
    ip_hdr->version_ihl = 0x45; 
    ip_hdr->type_of_service = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(ip_len);  
    ip_hdr->packet_id = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = IPPROTO_UDP;
    ip_hdr->src_addr = rte_cpu_to_be_32(client_ip_);
    ip_hdr->dst_addr = rte_cpu_to_be_32(server_ip_);
    ip_hdr->hdr_checksum = 0;  
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    
    // ========== UDP header (8 bytes) ==========
    struct rte_udp_hdr* udp_hdr = reinterpret_cast<struct rte_udp_hdr*>(
        pkt_data + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
    udp_hdr->src_port = rte_cpu_to_be_16(client_port_);
    udp_hdr->dst_port = rte_cpu_to_be_16(server_port_);
    udp_hdr->dgram_len = rte_cpu_to_be_16(udp_len);
    
    // ========== UDP payload (16 bytes) ==========
    uint8_t* payload = pkt_data + sizeof(struct rte_ether_hdr) +
                      sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);
    memcpy(payload, req, sizeof(Request));
 
    udp_hdr->dgram_cksum = 0;
    uint16_t udp_cksum = rte_ipv4_udptcp_cksum(ip_hdr, (const void*)udp_hdr);
    if (udp_cksum == 0) {
        udp_hdr->dgram_cksum = 0xFFFF;
    } else {
        udp_hdr->dgram_cksum = udp_cksum;
    }

    // udp_hdr->dgram_cksum = 0;
    
    return m;
}

bool DpdkLatencyClient::send(Request* req, int max_retries) {
    struct rte_mbuf* m = createPacket(req);
    if (!m) {
        req_pool_.push_back(req);
        return false;
    }
    
    int retry_count = 0;
    uint16_t tx_queue_id = getTxQueueId();
    
    while (retry_count < max_retries) {
        uint16_t nb_tx = rte_eth_tx_burst(port_id_, tx_queue_id, &m, 1);
        
        if (nb_tx == 1) {
            packets_sent_++;
            req_pool_.push_back(req);
            return true;
        }
        
        retry_count++;
        if (retry_count < max_retries) {
            rte_pause();
            if (retry_count % 100 == 0) {
                rte_delay_us_block(1);
            }
        }
    }
    
    rte_pktmbuf_free(m);
    req_pool_.push_back(req);
    return false;
}

void DpdkLatencyClient::cleanupOldTimestamps() {
    uint64_t current_ns = tsc_to_ns(rte_get_tsc_cycles(), tsc_hz_);
    const uint64_t TIMEOUT_NS = 1e9;
    
    auto it = send_timestamps_ns_.begin();
    while (it != send_timestamps_ns_.end()) {
        uint64_t send_ns = it->second;
        if (current_ns > send_ns && (current_ns - send_ns) > TIMEOUT_NS) {
            it = send_timestamps_ns_.erase(it);
        } else {
            ++it;
        }
    }
}

bool DpdkLatencyClient::recv(std::vector<Response>& responses) {
    
    struct rte_mbuf* bufs[BURST_SIZE];
    uint16_t rx_queue_id = getRxQueueId();
    uint16_t nb_rx = rte_eth_rx_burst(port_id_, rx_queue_id, bufs, BURST_SIZE);
    
    if (nb_rx == 0) {
        rte_pause();
        return false;
    }
    
    Response resp;
    bool got_response = false;
    
    for (uint16_t i = 0; i < nb_rx; i++) {
        struct rte_mbuf* m = bufs[i];
        
        if (parsePacket(m, &resp)) {
            responses.push_back(resp);
            got_response = true;
        }
        rte_pktmbuf_free(m);
    }
    
    if (got_response) {
        packets_received_ += responses.size();
        return true;
    }
    
    return false;
}

bool DpdkLatencyClient::parsePacket(struct rte_mbuf* m, Response* resp) {
    uint16_t min_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                      sizeof(struct rte_udp_hdr) + sizeof(Response);
    
    if (rte_pktmbuf_data_len(m) < min_len) {
        return false;
    }
    
    struct rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
    if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        return false;
    }
    
    struct rte_ipv4_hdr* ip_hdr = reinterpret_cast<struct rte_ipv4_hdr*>(eth_hdr + 1);
    if (ip_hdr->next_proto_id != IPPROTO_UDP) {
        return false;
    }
    
    uint32_t dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
    if (dst_ip != client_ip_) {
        return false;
    }
    
    struct rte_udp_hdr* udp_hdr = reinterpret_cast<struct rte_udp_hdr*>(
        (uint8_t*)ip_hdr + ((ip_hdr->version_ihl & 0xF) * 4));

    if (rte_be_to_cpu_16(udp_hdr->dst_port) != client_port_) {
        return false;
    }
    
    Response* resp_data = reinterpret_cast<Response*>(udp_hdr + 1);
    *resp = *resp_data;
    return true;
}

void DpdkLatencyClient::finiReqs(const std::vector<Response>& responses) {
    for (const auto& resp : responses) {
        uint64_t server_delay_cy = resp.server_send_ns - resp.networker_recv_ns;
        uint64_t server_delay_ns = cycles_to_ns_server(server_delay_cy);
        updateLatencyStats(server_delay_ns, resp.runNs, resp.networker_cy, resp.dispatcher_cy);
    }
}

void DpdkLatencyClient::updateLatencyStats(uint64_t server_delay_ns, uint64_t run_ns,
                                           uint64_t networker_cy, uint64_t dispatcher_cy) {
    pending_latency_.push_back(server_delay_ns);
    pending_ratio_.push_back(run_ns > 0 ? server_delay_ns / run_ns : 0);
    pending_networker_cy_.push_back(networker_cy);
    pending_dispatcher_cy_.push_back(dispatcher_cy);
    if (pending_latency_.size() >= STATS_BATCH_SIZE) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        for (size_t i = 0; i < pending_latency_.size(); i++) {
            uint64_t d = pending_latency_[i];
            latency_samples_.push_back(d);
            latency_stats_.count++;
            latency_stats_.total_ns += d;
            if (d < latency_stats_.min_ns) latency_stats_.min_ns = d;
            if (d > latency_stats_.max_ns) latency_stats_.max_ns = d;
            latency_stats_.work_ratio.push_back(pending_ratio_[i]);
            networker_cy_samples_.push_back(pending_networker_cy_[i]);
            dispatcher_cy_samples_.push_back(pending_dispatcher_cy_[i]);
        }
        pending_latency_.clear();
        pending_ratio_.clear();
        pending_networker_cy_.clear();
        pending_dispatcher_cy_.clear();
    }
}

void DpdkLatencyClient::flushPendingStats() {
    if (pending_latency_.empty()) return;
    std::lock_guard<std::mutex> lock(stats_mutex_);
    for (size_t i = 0; i < pending_latency_.size(); i++) {
        uint64_t d = pending_latency_[i];
        latency_samples_.push_back(d);
        latency_stats_.count++;
        latency_stats_.total_ns += d;
        if (d < latency_stats_.min_ns) latency_stats_.min_ns = d;
        if (d > latency_stats_.max_ns) latency_stats_.max_ns = d;
        latency_stats_.work_ratio.push_back(pending_ratio_[i]);
        networker_cy_samples_.push_back(pending_networker_cy_[i]);
        dispatcher_cy_samples_.push_back(pending_dispatcher_cy_[i]);
    }
    pending_latency_.clear();
    pending_ratio_.clear();
    pending_networker_cy_.clear();
    pending_dispatcher_cy_.clear();
}

void DpdkLatencyClient::computePercentiles() {
    flushPendingStats();
    if (stats_computed_ || latency_samples_.empty()) {
        return;
    }

    std::sort(latency_samples_.begin(), latency_samples_.end());
    
    size_t count = latency_samples_.size();

    if (count > 0) {
        latency_stats_.p50_ns = latency_samples_[static_cast<size_t>(count * 0.5)];
        latency_stats_.p90_ns = latency_samples_[static_cast<size_t>(count * 0.9)];
        latency_stats_.p95_ns = latency_samples_[static_cast<size_t>(count * 0.95)];
        latency_stats_.p99_ns = latency_samples_[static_cast<size_t>(count * 0.99)];
        if (count >= 1000) {
            latency_stats_.p999_ns = latency_samples_[static_cast<size_t>(count * 0.999)];
        } else {
            latency_stats_.p999_ns = latency_samples_.back();
        }
    }
    
    stats_computed_ = true;
}

std::vector<uint64_t> DpdkLatencyClient::getLatencySamples() const {
    const_cast<DpdkLatencyClient*>(this)->flushPendingStats();
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return latency_samples_;
}

std::vector<uint64_t> DpdkLatencyClient::getNetworkerCySamples() const {
    const_cast<DpdkLatencyClient*>(this)->flushPendingStats();
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return networker_cy_samples_;
}

std::vector<uint64_t> DpdkLatencyClient::getDispatcherCySamples() const {
    const_cast<DpdkLatencyClient*>(this)->flushPendingStats();
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return dispatcher_cy_samples_;
}

void DpdkLatencyClient::dumpStats() {

    computePercentiles();
    
    std::cout << "\n=== DPDK Client (Thread " << thread_id_ << ") Statistics ===" << std::endl;
    std::cout << "Packets sent:     " << packets_sent_.load() << std::endl;
    std::cout << "Packets received: " << packets_received_.load() << std::endl;
    
    if (latency_stats_.count > 0) {
        double avg_latency = static_cast<double>(latency_stats_.total_ns) / latency_stats_.count;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "\nServer Processing Latency Statistics (ns):" << std::endl;
        std::cout << "  Count:      " << latency_stats_.count << std::endl;
        std::cout << "  Average:    " << avg_latency << std::endl;
        std::cout << "  Min:        " << latency_stats_.min_ns << std::endl;
        std::cout << "  Max:        " << latency_stats_.max_ns << std::endl;
        std::cout << "  50th (p50): " << latency_stats_.p50_ns << std::endl;
        std::cout << "  90th (p90): " << latency_stats_.p90_ns << std::endl;
        std::cout << "  95th (p95): " << latency_stats_.p95_ns << std::endl;
        std::cout << "  99th (p99): " << latency_stats_.p99_ns << std::endl;
        std::cout << "  99.9th:     " << latency_stats_.p999_ns << std::endl;
        
        if (latency_stats_.count >= 1000) {
            double estimated_test_time = latency_stats_.count * (1.0 / qps_);
            
            if (estimated_test_time > 0) {
                double actual_rps = latency_stats_.count / estimated_test_time;
                std::cout << "\nThroughput:" << std::endl;
                std::cout << "  Target RPS: " << std::setprecision(0) << qps_ << std::endl;
                std::cout << "  Actual RPS: " << std::setprecision(0) << actual_rps << std::endl;
                std::cout << "  Estimated test time: " << std::setprecision(3) 
                         << estimated_test_time << " seconds" << std::endl;
            }
        }
    }
    std::cout << std::endl;
}

// ---------- DpdkBimodalLatencyClient ----------
DpdkBimodalLatencyClient::DpdkBimodalLatencyClient(uint16_t server_port, double qps,
                                                   uint64_t work1, uint64_t work2, double ratio,
                                                   uint16_t thread_id, uint16_t client_port,
                                                   const std::string& output_file)
    : DpdkLatencyClient(server_port, qps, 0, thread_id, client_port), output_file_(output_file) {
    uint64_t seed = static_cast<uint64_t>(rte_get_tsc_cycles() & 0xFFFFFFFFu);
    uint64_t now = tsc_to_ns(rte_get_tsc_cycles(), rte_get_tsc_hz());
    work_dist_ = new BimodalDist(seed, work1, work2, ratio);
    dist_ = new ExpDist(qps / 1e9, seed, now);
}

DpdkBimodalLatencyClient::~DpdkBimodalLatencyClient() {
    delete work_dist_;
    delete dist_;
}

Request* DpdkBimodalLatencyClient::startReq() {
    Request* req;
    if (!req_pool_.empty()) {
        req = req_pool_.back();
        req_pool_.pop_back();
    } else {
        req = new Request();
    }
    req->runNs = work_dist_->workNs();
    req->genNs = dist_->nextArrivalNs();
    while (tsc_to_ns(rte_get_tsc_cycles(), rte_get_tsc_hz()) < req->genNs)
        rte_pause();
    return req;
}

void DpdkLatencyClient::printPacketDebug(struct rte_mbuf* m) {
    uint8_t* data = rte_pktmbuf_mtod(m, uint8_t*);
    uint16_t len = rte_pktmbuf_pkt_len(m);
    
    printf("\n=== Packet Debug (Length: %u) ===\n", len);
    
    // Ethernet header
    struct rte_ether_hdr* eth = (struct rte_ether_hdr*)data;
    printf("Ethernet: Src %02x:%02x:%02x:%02x:%02x:%02x -> Dst %02x:%02x:%02x:%02x:%02x:%02x\n",
           eth->src_addr.addr_bytes[0], eth->src_addr.addr_bytes[1],
           eth->src_addr.addr_bytes[2], eth->src_addr.addr_bytes[3],
           eth->src_addr.addr_bytes[4], eth->src_addr.addr_bytes[5],
           eth->dst_addr.addr_bytes[0], eth->dst_addr.addr_bytes[1],
           eth->dst_addr.addr_bytes[2], eth->dst_addr.addr_bytes[3],
           eth->dst_addr.addr_bytes[4], eth->dst_addr.addr_bytes[5]);
    
    // IP header
    struct rte_ipv4_hdr* ip = (struct rte_ipv4_hdr*)(data + sizeof(struct rte_ether_hdr));
    printf("IP: %d.%d.%d.%d -> %d.%d.%d.%d\n",
           (rte_be_to_cpu_32(ip->src_addr) >> 24) & 0xFF,
           (rte_be_to_cpu_32(ip->src_addr) >> 16) & 0xFF,
           (rte_be_to_cpu_32(ip->src_addr) >> 8) & 0xFF,
           rte_be_to_cpu_32(ip->src_addr) & 0xFF,
           (rte_be_to_cpu_32(ip->dst_addr) >> 24) & 0xFF,
           (rte_be_to_cpu_32(ip->dst_addr) >> 16) & 0xFF,
           (rte_be_to_cpu_32(ip->dst_addr) >> 8) & 0xFF,
           rte_be_to_cpu_32(ip->dst_addr) & 0xFF);
    
    // UDP header
    struct rte_udp_hdr* udp = (struct rte_udp_hdr*)((uint8_t*)ip + ((ip->version_ihl & 0xF) * 4));
    printf("UDP: Src Port %u -> Dst Port %u\n",
           rte_be_to_cpu_16(udp->src_port), rte_be_to_cpu_16(udp->dst_port));
    
    printf("=================================\n\n");
}

void DpdkLatencyClient::dumpDetailedStats() {
    computePercentiles();
    
    std::cout << "\n=== Detailed Server Processing Latency Breakdown ===" << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    
    if (latency_stats_.count > 0) {
        double avg_latency = static_cast<double>(latency_stats_.total_ns) / latency_stats_.count;
        
        std::cout << "Server Processing Time (receive→send):" << std::endl;
        std::cout << "  Average: " << avg_latency / 1000.0 << " µs" << std::endl;
        std::cout << "  Min:     " << latency_stats_.min_ns / 1000.0 << " µs" << std::endl;
        std::cout << "  Max:     " << latency_stats_.max_ns / 1000.0 << " µs" << std::endl;
        std::cout << "  p50:     " << latency_stats_.p50_ns / 1000.0 << " µs" << std::endl;
        std::cout << "  p99:     " << latency_stats_.p99_ns / 1000.0 << " µs" << std::endl;
        
        std::cout << "\nNote: This is SERVER PROCESSING TIME ONLY" << std::endl;
        std::cout << "      Measured from server receive to server send" << std::endl;
    }
}