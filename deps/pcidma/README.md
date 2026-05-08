Description
---
This kernel module is a PCI device driver. Its purpose is to control the DMA / bus mastering. A program can enable the DMA of a PCI device given its location. The kernel module makes sure that DMA will be disabled when the program terminates.


Usage
---
In order to use this kernel module, you have first to compile it and load it:
```
make
sudo insmod pcidma.ko
```

Let's assume that you have a PCI device on your system, such as:
```
$ lspci -D | grep Ethernet
0000:42:00.1 Ethernet controller: Intel Corporation Ethernet 10G 2P X520 Adapter (rev 01)
```

An example program that uses the kernel module to enable DMA is the following:
```
#include "pcidma.h"

int main(int argc, char **argv)
{
    int fd;
    struct args_enable args;

    args.pci_loc.domain = 0;
    args.pci_loc.bus = 0x42;
    args.pci_loc.slot = 0;
    args.pci_loc.func = 1;

    fd = open("/dev/pcidma", O_RDONLY);
    ioctl(fd, PCIDMA_ENABLE, &args);
    /* do_work(); */
    return 0;
}
```

Note: In order for the `ioctl` to succeed, the specified device must not be assigned to another device driver.
