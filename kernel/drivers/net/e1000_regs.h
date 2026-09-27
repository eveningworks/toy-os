#ifndef E1000_REGS_H
#define E1000_REGS_H

// The Intel 8254x's registers and legacy descriptors, shared by the
// driver (e1000.c) and the kernel debugger's own polled copy
// (e1000_kdb.c), which drives a SECOND card the driver never sees.

#include <stdint.h>

#define E1000_VENDOR 0x8086
#define E1000_DEV_82540EM 0x100E

// Register offsets (8254x manual section 13).
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_SLU   (1u << 6)   // set link up
#define CTRL_ASDE  (1u << 5)   // auto-speed detection

#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)  // accept broadcast
#define RCTL_SECRC (1u << 26)  // strip the Ethernet CRC
// BSIZE 00 with no BSEX is 2048 bytes, which is why neither appears here.

#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)   // pad short packets

#define ICR_RXT0   (1u << 7)   // receive timer -- "there are packets"
#define ICR_RXDMT0 (1u << 4)   // ring is running low
#define ICR_RXO    (1u << 6)   // overrun

#define RAH_AV     (1u << 31)  // the address register holds a valid address

#define RX_STATUS_DD  (1u << 0)   // descriptor done
#define RX_STATUS_EOP (1u << 1)

#define TX_CMD_EOP  (1u << 0)
#define TX_CMD_IFCS (1u << 1)
#define TX_CMD_RS   (1u << 3)
#define TX_STATUS_DD (1u << 0)

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

_Static_assert(sizeof(struct rx_desc) == 16, "rx descriptor is 16 bytes");
_Static_assert(sizeof(struct tx_desc) == 16, "tx descriptor is 16 bytes");

#endif
