# Shinjuku Client

A high‑performance DPDK‑based UDP load generator with Poisson arrival control, multi‑queue RSS support, and detailed latency breakdown (server, networker, dispatcher).

## Setup

The system requires DPDK version 20.11 or higher.

Bind the NIC to DPDK:

```bash
sudo dpdk-devbind.py --bind=vfio-pci 0000:af:00.0
```

Allocate huge pages:

```bash
sudo sh -c 'for i in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do echo 8192 > $i; done'
```

## Build

```bash
sudo rm -rf build
mkdir build && cd build
cmake ..
make
cd ..
```

## Usage

### Single workload

```bash
sudo ./dpdk_client <server_port> <rps> <work_ns> <send_threads> [recv_threads] [drain_recv] [output_file]
```

### Bimodal workload

```bash
sudo ./dpdk_bimodal_client <server_port> <rps> <work1_ns> <work2_ns> <ratio1> <send_threads> [recv_threads] [drain_recv] [output_file]
```

### Parameters

| Parameter | Description |
|-----------|-------------|
| server_port | Server UDP port |
| rps | Target requests per second |
| work_ns | Server execution time (ns) |
| work1_ns / work2_ns | Two possible execution times for bimodal |
| ratio1 | Fraction of requests using work1_ns (0–1) |
| send_threads | Number of sender threads |
| recv_threads | Number of receiver threads (default: 0 = auto) |
| drain_recv | 1 = drain remaining packets on exit (default: 0) |
| output_file | 1 = write stats to results/ (default: 0) |

### Example

```bash
# Server port 1234, 100K RPS, 1000ns work, 2 senders, 2 receivers, drain on, save results
sudo ./dpdk_client 1234 100000 1000 2 2 1 1
```

## Hardware Assumptions

- **Server TSC frequency:** `2799.98 MHz` (hardcoded). Change in source according to your device.
- **NIC port:** `port 2` (hardcoded). Change in source if needed.