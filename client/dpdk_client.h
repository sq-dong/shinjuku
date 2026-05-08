// dpdk_client.h
#ifndef DPDK_CLIENT_FIXED_H
#define DPDK_CLIENT_FIXED_H

#include "dist.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <thread>

#include <random>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_udp.h>
#include <rte_ip.h>
#include <rte_ether.h>

uint16_t compute_rss_port_for_queue(uint16_t target_queue, uint16_t num_rx_queues,
                                     uint16_t server_port);

#define NUM_MBUFS 32768
#define NUM_MBUFS_RX  65536 
#define MBUF_CACHE_SIZE 512
#define BURST_SIZE 64        
#define RX_RING_SIZE 4096    
#define TX_RING_SIZE 1024
#define DEFAULT_PACKET_LEN 64 

#define MAX_THREADS 64

#define RSS_HASH_KEY_LENGTH 40
#define RSS_HF RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_UDP

struct Request {
    uint64_t runNs;           
    uint64_t genNs;           
} __attribute__((packed));

struct Response {
    uint64_t runNs;            
    uint64_t genNs;              
    uint64_t networker_recv_ns;  
    uint64_t server_send_ns;    
    uint64_t networker_cy;      
    uint64_t dispatcher_cy;     
} __attribute__((packed));

struct LatencyStats {
    uint64_t count;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t p50_ns;
    uint64_t p90_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t p999_ns;
    std::vector<uint64_t> latency_samples;
    std::vector<uint64_t> work_ratio;  
};

class DpdkPortManager {
private:
    static DpdkPortManager* instance_;
    static std::mutex init_mutex_;
    
    uint8_t port_id_;
    std::vector<struct rte_mempool*> rx_mbuf_pools_;
    std::vector<struct rte_mempool*> tx_mbuf_pools_; 
    bool initialized_;
    uint16_t num_rx_queues_;
    uint16_t num_tx_queues_;
    
    DpdkPortManager() : port_id_(2),
                       initialized_(false), num_rx_queues_(4), num_tx_queues_(4) {}
    ~DpdkPortManager();
    
    bool initMemPool(uint16_t num_rx_queues, uint16_t num_tx_queues);
    bool configurePort(uint16_t num_rx_queues, uint16_t num_tx_queues);
    
public:
    static DpdkPortManager* getInstance();
    static void destroyInstance();
    
    bool initialize(uint16_t num_rx_queues = 4, uint16_t num_tx_queues = 4);
    bool isInitialized() const { return initialized_; }
    
    uint8_t getPortId() const { return port_id_; }

    struct rte_mempool* getMbufPool(uint16_t queue_id) const;
    uint16_t getNumRxQueues() const { return num_rx_queues_; }
    uint16_t getNumTxQueues() const { return num_tx_queues_; }
};

class DpdkLatencyClient {
    public:
        DpdkLatencyClient(uint16_t server_port, double qps, uint64_t work_ns,
                         uint16_t thread_id = 0, uint16_t client_port = 0);
        ~DpdkLatencyClient();
        
        bool initialize();
        virtual Request* startReq();
        int prepareBatch(Request* reqs[], int max_n);
        int sendBatch(Request* reqs[], int n, int max_retries = 1000);
        bool send(Request* req, int max_retries = 1000);
        
        bool recv(std::vector<Response>& responses);

        void finiReqs(const std::vector<Response>& responses);
        
        void dumpStats();
        const char* errmsg() { return error_msg_; }
        
        uint64_t getPacketsSent() const { return packets_sent_.load(); }
        uint64_t getPacketsReceived() const { return packets_received_.load(); }
        std::vector<uint64_t> getLatencySamples() const;
        std::vector<uint64_t> getNetworkerCySamples() const;
        std::vector<uint64_t> getDispatcherCySamples() const;
        void dumpDetailedStats();

        uint16_t getTxQueueId() const { return thread_id_ % num_tx_queues_; }
        uint16_t getRxQueueId() const { return thread_id_ % num_rx_queues_; }
        
        uint16_t getClientPort() const { return client_port_; }
        
    protected:
        std::vector<Request*> req_pool_;
    private:
        double tsc_hz_;

        std::unordered_map<uint64_t, uint64_t> send_timestamps_ns_;
        struct rte_mbuf* createPacket(const Request* req);
        bool parsePacket(struct rte_mbuf* m, Response* resp);
        void updateLatencyStats(uint64_t server_delay_ns, uint64_t run_ns,
                               uint64_t networker_cy, uint64_t dispatcher_cy);
        void flushPendingStats();
        void computePercentiles();
        void printPacketDebug(struct rte_mbuf* m);
        void cleanupOldTimestamps();
        
        uint8_t port_id_;
        uint16_t thread_id_;
        uint16_t num_rx_queues_;
        uint16_t num_tx_queues_;
        struct rte_mempool* mbuf_pool_;

        uint32_t server_ip_;
        uint32_t client_ip_;
        uint16_t server_port_;
        uint16_t client_port_;
        
        uint8_t client_mac_[6];
        uint8_t server_mac_[6];
        
        double qps_;
        uint64_t work_ns_;

        std::atomic<uint64_t> packets_sent_{0};
        std::atomic<uint64_t> packets_received_{0};
        LatencyStats latency_stats_;
        std::vector<uint64_t> latency_samples_;
        mutable std::mutex stats_mutex_;

        static const size_t STATS_BATCH_SIZE = 128;
        std::vector<uint64_t> pending_latency_;
        std::vector<uint64_t> pending_ratio_;
        std::vector<uint64_t> pending_networker_cy_;
        std::vector<uint64_t> pending_dispatcher_cy_;
        std::vector<uint64_t> networker_cy_samples_;
        std::vector<uint64_t> dispatcher_cy_samples_;
        
        char error_msg_[256];
        uint64_t next_gen_ns_{0};
        uint64_t last_cleanup_{0};
        bool stats_computed_{false};
        std::default_random_engine rng_;
        std::exponential_distribution<double> exp_dist_;
        
        bool debug_enabled_{false};
    };

class DpdkBimodalLatencyClient : public DpdkLatencyClient {
public:
    DpdkBimodalLatencyClient(uint16_t server_port, double qps,
                            uint64_t work1, uint64_t work2, double ratio,
                            uint16_t thread_id = 0, uint16_t client_port = 0,
                            const std::string& output_file = "");
    ~DpdkBimodalLatencyClient();
    Request* startReq() override;

private:
    BimodalDist* work_dist_{nullptr};
    ExpDist* dist_{nullptr};
    std::string output_file_;
};

#endif // DPDK_CLIENT_FIXED_H