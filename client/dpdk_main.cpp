// dpdk_main.cpp
#include "dpdk_client.h"
#include <algorithm>
#include <iostream>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <signal.h>
#include <cstring>
#include <iomanip>
#include <pthread.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sstream>
#include <string>
#include <cstdlib>
#include <vector>

static const double TSC_HZ = 2799.98e6;
static inline uint64_t cycles_to_ns(uint64_t cy) {
    return static_cast<uint64_t>(static_cast<double>(cy) * 1e9 / TSC_HZ);
}

std::atomic<bool> should_stop(false);
std::atomic<uint64_t> drain_phase_received(0);

struct ThreadContext {
    int id;
    int lcore_id;
    DpdkLatencyClient* client;
    std::vector<DpdkLatencyClient*> send_clients;
    double qps;
    bool is_receiver;
    bool drain_recv;
    std::atomic<bool>* should_stop;
};

void sigint_handler(int sig) {
    should_stop.store(true);
    std::cout << "\nReceived Ctrl+C, stopping gracefully..." << std::endl;
}

void send_thread_func(ThreadContext* ctx) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ctx->lcore_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    const std::vector<DpdkLatencyClient*>& clients = ctx->send_clients.empty()
        ? std::vector<DpdkLatencyClient*>(1, ctx->client) : ctx->send_clients;
    size_t num_clients = clients.size();
    size_t next_client = 0;
    Request* batch[32];

    while (!ctx->should_stop->load()) {
        DpdkLatencyClient* c = clients[next_client % num_clients];
        next_client++;
        int n = c->prepareBatch(batch, 32);
        if (n > 0) {
            int sent = c->sendBatch(batch, n, 1000);
            if (sent == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    }

    delete ctx;
}

void recv_thread_func(ThreadContext* ctx) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ctx->lcore_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    DpdkLatencyClient* client = ctx->client;
    std::vector<Response> responses;
    
    while (!ctx->should_stop->load()) {
        responses.clear();
        if (client->recv(responses)) {
            if (!responses.empty()) {
                client->finiReqs(responses);
            }
        }
    }

    if (ctx->drain_recv) {
        uint64_t at_drain_start = client->getPacketsReceived();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        auto last_recv = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline) {
            responses.clear();
            if (client->recv(responses)) {
                last_recv = std::chrono::steady_clock::now();
                if (!responses.empty())
                    client->finiReqs(responses);
            } else if (std::chrono::steady_clock::now() - last_recv > std::chrono::seconds(1))
                break;
        }
        drain_phase_received += (client->getPacketsReceived() - at_drain_start);
    } else {
        for (int i = 0; i < 10; i++) {
            responses.clear();
            if (client->recv(responses)) {
                if (!responses.empty())
                    client->finiReqs(responses);
            }
        }
    }
    
    delete ctx;
}

int main(int argc, char* argv[]) {
    std::vector<const char*> dpdk_argv_vec;
    dpdk_argv_vec.push_back(argv[0]);
    dpdk_argv_vec.push_back("-l");
    dpdk_argv_vec.push_back("0-63");
    dpdk_argv_vec.push_back("--proc-type=auto");
    
    // Add file-prefix if DPDK_FILE_PREFIX is set
    const char* file_prefix = std::getenv("DPDK_FILE_PREFIX");
    if (file_prefix && strlen(file_prefix) > 0) {
        dpdk_argv_vec.push_back("--file-prefix");
        dpdk_argv_vec.push_back(file_prefix);
    }
    dpdk_argv_vec.push_back(nullptr);
    
    int dpdk_argc = dpdk_argv_vec.size() - 1;
    int ret = rte_eal_init(dpdk_argc, const_cast<char**>(dpdk_argv_vec.data()));
    if (ret < 0) {
        std::cerr << "Failed to initialize DPDK EAL" << std::endl;
        return -1;
    }

    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <server_port> <qps> <work_ns>"
                  << " <send_threads> [recv_threads] [drain_recv] [output_file_or_not]" << std::endl;
        std::cerr << "Example: " << argv[0] << " 1234 100000 1000 2 1 0 1" << std::endl;
        std::cerr << "         (recv_threads can be 2x or 3x send_threads to reduce loss, e.g. 32 64)" << std::endl;
        std::cerr << "\nHardcoded IX configuration:" << std::endl;
        std::cerr << " Server IP: 192.168.200.5" << std::endl;
        std::cerr << " Client IP: 192.168.200.137" << std::endl;
        std::cerr << " Server MAC: 00:1b:21:bc:d6:0f" << std::endl;
        std::cerr << " Client MAC: 00:1b:21:bc:d6:7d" << std::endl;
        return -1;
    }

    uint16_t server_port = static_cast<uint16_t>(atoi(argv[1]));
    double qps = atof(argv[2]);
    uint64_t work_ns = static_cast<uint64_t>(atoi(argv[3]));
    int num_send_threads = atoi(argv[4]);
    int num_recv_threads = (argc >= 6) ? atoi(argv[5]) : 1;
    int drain_recv = 0;
    int output_file_or_not = 0;
    if (argc >= 8) {
        drain_recv = atoi(argv[6]);
        output_file_or_not = atoi(argv[7]);
    } else if (argc >= 7) {
        drain_recv = atoi(argv[6]);
    }
    
    if (num_send_threads < 1) num_send_threads = 1;
    if (num_recv_threads < 1) num_recv_threads = 1;

    int recv_ratio = 1;
    if (num_recv_threads > num_send_threads && num_send_threads > 0) {
        recv_ratio = num_recv_threads / num_send_threads;
        num_recv_threads = num_send_threads * recv_ratio;
        std::cout << "[INFO] Receive threads = " << num_recv_threads
                  << " (" << recv_ratio << " per send thread)" << std::endl;
    }
    
    signal(SIGINT, sigint_handler);
    
    std::cout << "\n=================================================================" << std::endl;
    std::cout << "DPDK Latency Client - Multi-Threaded Version" << std::endl;
    std::cout << "=================================================================" << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << " Server IP:       192.168.200.5" << std::endl;
    std::cout << " Client IP:       192.168.200.137" << std::endl;
    std::cout << " Server MAC:      00:1b:21:bc:d6:0f" << std::endl;
    std::cout << " Client MAC:      00:1b:21:bc:d6:7d" << std::endl;
    std::cout << " Server Port:     " << server_port << std::endl;
    std::cout << " Target QPS:      " << qps << std::endl;
    std::cout << " Work time:       " << work_ns << " ns" << std::endl;
    std::cout << " Send threads:    " << num_send_threads << std::endl;
    std::cout << " Receive threads: " << num_recv_threads
              << (recv_ratio > 1 ? " (" + std::to_string(recv_ratio) + " per send thread)" : "") << std::endl;
    std::cout << "\nPress Ctrl+C to stop the test" << std::endl;
    std::cout << "=================================================================\n" << std::endl;

    int num_queues = (num_recv_threads > num_send_threads) ? num_recv_threads : num_send_threads;
    DpdkPortManager* port_mgr = DpdkPortManager::getInstance();
    if (!port_mgr->initialize(num_queues, num_queues)) {
        std::cerr << "Failed to initialize DPDK port with multiple queues" << std::endl;
        return -1;
    }

    std::vector<DpdkLatencyClient*> clients;
    std::vector<std::thread> threads;
    uint16_t num_rx_queues = port_mgr->getNumRxQueues();
    int num_send_clients = (num_recv_threads > num_send_threads) ? num_recv_threads : num_send_threads;

    for (int i = 0; i < num_send_clients; ++i) {
        uint16_t client_port = compute_rss_port_for_queue(i, num_rx_queues, server_port);
        double qps_per_client = (num_recv_threads > num_send_threads) ? (qps / num_recv_threads) : (qps / num_send_threads);
        auto* send_client = new DpdkLatencyClient(server_port, qps_per_client, work_ns, i, client_port);
        if (!send_client->initialize()) {
            std::cerr << "Failed to initialize sender client " << i << ": " << send_client->errmsg() << std::endl;
            for (auto c : clients) delete c;
            return -1;
        }
        clients.push_back(send_client);
    }

    int clients_per_send = num_send_clients / num_send_threads;
    for (int i = 0; i < num_send_threads; ++i) {
        ThreadContext* send_ctx = new ThreadContext();
        send_ctx->id = i;
        send_ctx->lcore_id = i % 64;
        send_ctx->client = nullptr;
        send_ctx->qps = qps / num_send_threads;
        send_ctx->is_receiver = false;
        send_ctx->drain_recv = false;
        send_ctx->should_stop = &should_stop;
        int start = i * clients_per_send;
        int end = (i + 1) * clients_per_send;
        for (int j = start; j < end; ++j)
            send_ctx->send_clients.push_back(clients[j]);
        threads.emplace_back(send_thread_func, send_ctx);
    }

    int recv_lcore_base = std::min(num_send_threads, 64 - 1);
    for (int i = 0; i < num_recv_threads; ++i) {
        uint16_t client_port = compute_rss_port_for_queue(i, num_rx_queues, server_port);
        auto* recv_client = new DpdkLatencyClient(server_port, 0, 0, i, client_port);
        if (!recv_client->initialize()) {
            std::cerr << "Failed to initialize receiver client " << i << std::endl;
            delete recv_client;
            continue;
        }
        clients.push_back(recv_client);
        ThreadContext* recv_ctx = new ThreadContext();
        recv_ctx->id = num_send_threads + i;
        recv_ctx->lcore_id = recv_lcore_base + (i % std::max(1, 64 - recv_lcore_base));
        recv_ctx->client = recv_client;
        recv_ctx->qps = 0;
        recv_ctx->is_receiver = true;
        recv_ctx->drain_recv = (drain_recv != 0);
        recv_ctx->should_stop = &should_stop;
        threads.emplace_back(recv_thread_func, recv_ctx);
    }
    
    std::cout << "\nTest is running... Press Ctrl+C to stop\n" << std::endl;
    
    while (!should_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t total_sent = 0, total_received = 0;
        for (int i = 0; i < num_send_clients; i++) total_sent += clients[i]->getPacketsSent();
        for (int i = num_send_clients; i < (int)clients.size(); i++) total_received += clients[i]->getPacketsReceived();
        std::cout << "\rProgress: Sent=" << total_sent
                  << " Received=" << total_received
                  << " Loss=" << std::fixed << std::setprecision(2)
                  << (total_sent > 0 ? (100.0 * (total_sent - total_received) / total_sent) : 0.0)
                  << "%" << std::flush;
    }
    
    if (drain_recv)
        std::cout << "\n\nStopping send threads, draining receive..." << std::endl;
    else
        std::cout << "\n\nStopping threads..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    if (drain_recv) {
        for (int i = 0; i < num_send_threads; i++)
            if (threads[i].joinable()) threads[i].join();
        for (int i = num_send_threads; i < num_send_threads + num_recv_threads; i++)
            if (threads[i].joinable()) threads[i].join();
    } else {
        for (auto& t : threads) { if (t.joinable()) t.join(); }
    }

    uint64_t total_sent = 0, total_received = 0;
    for (int i = 0; i < num_send_clients; i++) total_sent += clients[i]->getPacketsSent();
    for (int i = num_send_clients; i < (int)clients.size(); i++) total_received += clients[i]->getPacketsReceived();

    std::cout << "\nOVERALL SUMMARY:" << std::endl;
    std::cout << "  Total packets sent:     " << total_sent << std::endl;
    std::cout << "  Total packets received: " << total_received << std::endl;
    double loss_rate = 0.0;
    if (total_sent > 0) {
        loss_rate = 100.0 * (total_sent - total_received) / total_sent;
        std::cout << "  Packet loss rate:      " << std::fixed << std::setprecision(2)
                  << loss_rate << "%" << std::endl;
    }

    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "FINAL STATISTICS" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    if (drain_recv) {
        std::cout << "  Drain recv total: " << total_received << std::endl;
        std::cout << "  Drain phase received: " << drain_phase_received.load() << std::endl;
    }

    std::vector<uint64_t> all_latency_samples;
    std::vector<uint64_t> all_networker_cy;
    std::vector<uint64_t> all_dispatcher_cy;
    for (int i = num_send_clients; i < (int)clients.size(); ++i) {
        auto s = clients[i]->getLatencySamples();
        all_latency_samples.insert(all_latency_samples.end(), s.begin(), s.end());
        auto nc = clients[i]->getNetworkerCySamples();
        all_networker_cy.insert(all_networker_cy.end(), nc.begin(), nc.end());
        auto dc = clients[i]->getDispatcherCySamples();
        all_dispatcher_cy.insert(all_dispatcher_cy.end(), dc.begin(), dc.end());
    }

    std::vector<uint64_t> all_networker_ns;
    std::vector<uint64_t> all_dispatcher_ns;
    all_networker_ns.reserve(all_networker_cy.size());
    all_dispatcher_ns.reserve(all_dispatcher_cy.size());
    for (uint64_t cy : all_networker_cy) all_networker_ns.push_back(cycles_to_ns(cy));
    for (uint64_t cy : all_dispatcher_cy) all_dispatcher_ns.push_back(cycles_to_ns(cy));

    std::vector<uint64_t> all_latency_samples_original = all_latency_samples;

    uint64_t min_ns = 0, max_ns = 0, p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0;
    double avg_ns = 0.0;
    size_t count = 0;

    if (!all_latency_samples.empty()) {
        std::vector<uint64_t> samples_for_stats;
        if (output_file_or_not == 0) {
            size_t total_count = all_latency_samples.size();
            size_t skip_count = total_count / 10;
            if (skip_count > 0 && skip_count < total_count) {
                samples_for_stats.assign(all_latency_samples.begin() + skip_count, all_latency_samples.end());
            } else {
                samples_for_stats = all_latency_samples;
            }
        } else {
            samples_for_stats = all_latency_samples;
        }
        
        std::sort(samples_for_stats.begin(), samples_for_stats.end());
        count = samples_for_stats.size();
        if (count > 0) {
            uint64_t total_ns = 0;
            for (auto v : samples_for_stats) total_ns += v;
            min_ns = samples_for_stats.front();
            max_ns = samples_for_stats.back();
            p50 = samples_for_stats[static_cast<size_t>(count * 0.5)];
            p90 = samples_for_stats[static_cast<size_t>(count * 0.9)];
            p95 = samples_for_stats[static_cast<size_t>(count * 0.95)];
            p99 = samples_for_stats[static_cast<size_t>(count * 0.99)];
            p999 = (count >= 1000) ? samples_for_stats[static_cast<size_t>(count * 0.999)] : samples_for_stats.back();
            avg_ns = static_cast<double>(total_ns) / count;
            std::cout << "\nServer Processing Latency Statistics (ns) [merged]:" << std::endl;
            std::cout << "  Count:      " << count << std::endl;
            std::cout << "  Average:    " << std::fixed << std::setprecision(2) << avg_ns << std::endl;
            std::cout << "  Min:        " << min_ns << std::endl;
            std::cout << "  Max:        " << max_ns << std::endl;
            std::cout << "  50th (p50): " << p50 << std::endl;
            std::cout << "  90th (p90): " << p90 << std::endl;
            std::cout << "  95th (p95): " << p95 << std::endl;
            std::cout << "  99th (p99): " << p99 << std::endl;
            std::cout << "  99.9th:     " << p999 << std::endl;
        }
    }

    auto print_ns_stats = [&output_file_or_not](const std::vector<uint64_t>& samples_ns, const char* title) {
        if (samples_ns.empty()) return;
        std::vector<uint64_t> samples_for_stats;
        if (output_file_or_not == 0) {
            size_t total_count = samples_ns.size();
            size_t skip_count = total_count / 10;
            if (skip_count > 0 && skip_count < total_count)
                samples_for_stats.assign(samples_ns.begin() + skip_count, samples_ns.end());
            else
                samples_for_stats = samples_ns;
        } else {
            samples_for_stats = samples_ns;
        }
        std::sort(samples_for_stats.begin(), samples_for_stats.end());
        size_t n = samples_for_stats.size();
        if (n == 0) return;
        uint64_t total = 0;
        for (auto v : samples_for_stats) total += v;
        uint64_t min_v = samples_for_stats.front();
        uint64_t max_v = samples_for_stats.back();
        uint64_t p50_v = samples_for_stats[static_cast<size_t>(n * 0.5)];
        uint64_t p90_v = samples_for_stats[static_cast<size_t>(n * 0.9)];
        uint64_t p95_v = samples_for_stats[static_cast<size_t>(n * 0.95)];
        uint64_t p99_v = samples_for_stats[static_cast<size_t>(n * 0.99)];
        uint64_t p999_v = (n >= 1000) ? samples_for_stats[static_cast<size_t>(n * 0.999)] : samples_for_stats.back();
        double avg_v = static_cast<double>(total) / n;
        std::cout << "\n" << title << std::endl;
        std::cout << "  Count:      " << n << std::endl;
        std::cout << "  Average:    " << std::fixed << std::setprecision(2) << avg_v << std::endl;
        std::cout << "  Min:        " << min_v << std::endl;
        std::cout << "  Max:        " << max_v << std::endl;
        std::cout << "  50th (p50): " << p50_v << std::endl;
        std::cout << "  90th (p90): " << p90_v << std::endl;
        std::cout << "  95th (p95): " << p95_v << std::endl;
        std::cout << "  99th (p99): " << p99_v << std::endl;
        std::cout << "  99.9th:     " << p999_v << std::endl;
    };
    print_ns_stats(all_networker_ns, "Networker Processing Time Statistics (ns) [merged]:");
    print_ns_stats(all_dispatcher_ns, "Dispatcher Processing Time Statistics (ns) [merged]:");

    if (output_file_or_not == 1) {
        const char* results_dir = "results";
        struct stat info;
        if (stat(results_dir, &info) != 0) {
            if (mkdir(results_dir, 0755) != 0)
                std::cerr << "Warning: Failed to create results directory: " << results_dir << std::endl;
        }

        std::string filename;
        int file_index = 0;
        while (true) {
            std::ostringstream oss;
            oss << results_dir << "/output_" << file_index << ".txt";
            filename = oss.str();
            std::ifstream test_file(filename);
            if (!test_file.good()) break;
            test_file.close();
            file_index++;
        }
        std::ofstream out_file(filename);
        if (out_file.is_open()) {
            out_file << std::fixed << std::setprecision(2);
            out_file << "DPDK Latency Client Statistics\n";
            out_file << "================================\n\n";
            out_file << "OVERALL SUMMARY:\n";
            out_file << "  Total packets sent:     " << total_sent << "\n";
            out_file << "  Total packets received: " << total_received << "\n";
            if (total_sent > 0) out_file << "  Packet loss rate:      " << loss_rate << "%\n";
            if (!all_latency_samples_original.empty()) {
                out_file << "\nAll Latency Samples (ns):\n";
                for (const auto& latency : all_latency_samples_original)
                    out_file << latency << "\n";
            }
            out_file.close();
            std::cout << "\nStatistics saved to: " << filename << std::endl;
        } else {
            std::cerr << "Warning: Failed to open file for writing: " << filename << std::endl;
        }

        auto write_ns_to_subdir = [&](const std::string& subdir, const std::vector<uint64_t>& samples_ns) {
            if (samples_ns.empty()) return;
            std::string dir_path = std::string(results_dir) + "/" + subdir;
            if (stat(dir_path.c_str(), &info) != 0) {
                if (mkdir(dir_path.c_str(), 0755) != 0) return;
            }
            std::string fname = dir_path + "/1.txt";
            std::ifstream test_file(fname);
            if (test_file.good()) { test_file.close(); fname = dir_path + "/2.txt"; }
            std::ofstream of(fname);
            if (of.is_open()) {
                for (const auto& v : samples_ns) of << v << "\n";
                of.close();
                std::cout << "Samples (ns) saved to: " << fname << std::endl;
            }
        };
        write_ns_to_subdir("networker", all_networker_ns);
        write_ns_to_subdir("dispatcher", all_dispatcher_ns);
    }
    
    for (auto client : clients) {
        delete client;
    }
    
    DpdkPortManager::destroyInstance();
    
    std::cout << "\nDPDK client finished successfully." << std::endl;
    return 0;
}