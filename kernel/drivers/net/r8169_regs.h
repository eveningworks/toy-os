#ifndef R8169_REGS_H
#define R8169_REGS_H

// The RTL8111/8168's registers and C+ descriptor, shared by the driver
// (r8169.c) and the kernel debugger's own polled copy (r8169_kdb.c),
// which drives the card when `kdebug=net` has taken it from the driver.
// Adapted from FreeBSD's re(4) -- see r8169.c's header and LICENSE.

#include <stdint.h>

#define REALTEK_VENDOR 0x10EC

#define REG_IDR0      0x00   // MAC address, six bytes
#define REG_MAR0      0x08   // multicast hash, eight bytes
#define REG_TNPDS     0x20   // transmit ring base, 64-bit
#define REG_CR        0x37
#define REG_TPPOLL    0x38
#define REG_IMR       0x3C
#define REG_ISR       0x3E
#define REG_TCR       0x40
#define REG_RCR       0x44
#define REG_9346CR    0x50
#define REG_PHYAR     0x60
#define REG_PHYSTATUS 0x6C
#define REG_RMS       0xDA   // largest receive frame accepted
#define REG_CPCR      0xE0   // the C+ command register
#define REG_RDSAR     0xE4   // receive ring base, 64-bit
#define REG_MTPS      0xEC

#define CR_RESET      0x10
#define CR_RX_ENB     0x08
#define CR_TX_ENB     0x04

#define TPPOLL_NPQ    0x40   // "the normal-priority queue has work"

#define CFG_UNLOCK    0xC0   // 9346CR: config registers writable
#define CFG_LOCK      0x00

#define ISR_ROK       0x0001
#define ISR_RER       0x0002
#define ISR_TOK       0x0004
#define ISR_TER       0x0008
#define ISR_RDU       0x0010  // ran out of receive descriptors
#define ISR_LINKCHG   0x0020
#define ISR_FOVW      0x0040  // receive FIFO overflow
#define ISR_SYSERR    0x8000

#define RCR_ALLPHYS   0x00000001
#define RCR_INDIV     0x00000002  // frames addressed to us
#define RCR_MULTI     0x00000004
#define RCR_BROAD     0x00000008
#define RCR_DMA_UNLIM 0x00000700  // burst 7 == unlimited
#define RCR_FIFO_NONE 0x0000E000  // threshold 7 == whole frame

#define TCR_DMA_UNLIM 0x00000700
#define TCR_IFG_STD   0x03000000  // the IEEE 802.3 interframe gap
#define TCR_HWREV     0x7CF00000  // the chip version, XID

#define CPCR_PCI_MRW  0x0008      // memory read/write multiple
#define CPCR_RXCSUM   0x0020
#define CPCR_VLANSTRIP 0x0040

#define PHYSTATUS_LINK     0x02
#define PHYSTATUS_10MBPS   0x04
#define PHYSTATUS_100MBPS  0x08
#define PHYSTATUS_1000MBPS 0x10

struct rl_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
} __attribute__((packed));

_Static_assert(sizeof(struct rl_desc) == 16, "a C+ descriptor is 16 bytes");

#endif
