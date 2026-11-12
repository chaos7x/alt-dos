/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#define FAR
unsigned inb(unsigned port);
void outb(unsigned port, unsigned v);
#define irq_off() 0
#define irq_restore(f) ((void)(f))
extern unsigned short bios_ports[7];
struct fapi_pcidev { unsigned short loc, vendor, device; unsigned char cls, sub, progif, irq; };
int pci_info(unsigned i, struct fapi_pcidev *p);
struct fapi_pnpdev {
    unsigned char csn, ldn, act, rsv;
    unsigned char vid[4], lid[4];
    unsigned short io[4];
    unsigned char irq[2], dma[2];
};
