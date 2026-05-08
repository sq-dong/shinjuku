#!/bin/sh

# Remove kernel modules
sudo rmmod pcidma
sudo rmmod dune

# Unbind NICs
sudo ./deps/dpdk/tools/dpdk_nic_bind.py -u 86:00.0

# Build required kernel modules.
sudo chmod +r /boot/System.map-$(uname -r)
make -sj64 -C deps/dune
make -sj64 -C deps/pcidma
make -sj64 -C deps/dpdk config T=x86_64-native-linuxapp-gcc
make -sj64 -C deps/dpdk

# Set huge pages
sudo sh -c 'for i in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do echo 8192 > $i; done'

# Insert kernel modules
sudo insmod deps/dune/kern/dune.ko
sudo insmod deps/pcidma/pcidma.ko

sudo modprobe uio
sudo insmod deps/dpdk/build/kmod/igb_uio.ko
sudo deps/dpdk/tools/dpdk_nic_bind.py --status
