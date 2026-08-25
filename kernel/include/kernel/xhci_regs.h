#ifndef XHCI_REGS_H
#define XHCI_REGS_H

#include <stdint.h>

// xHCI register offsets, TRB types and completion codes -- DATA ONLY.
// No logic lives here, so this header can be included by the ring
// arithmetic (which has no controller) as well as by the driver.
//
// Names follow the xHCI 1.2 specification's own, because the spec is
// the document a reader will have open beside this. Where the spec
// names a field rather than an offset, the shift/mask pair carries the
// field name.

// --- capability registers (BAR0 + 0) ---------------------------------
#define XHCI_CAPLENGTH      0x00  // u8:  bytes from BAR0 to the operational regs
#define XHCI_HCIVERSION     0x02  // u16
#define XHCI_HCSPARAMS1     0x04
#define XHCI_HCSPARAMS2     0x08
#define XHCI_HCCPARAMS1     0x10
#define XHCI_DBOFF          0x14  // doorbell array offset from BAR0
#define XHCI_RTSOFF         0x18  // runtime register offset from BAR0

// HCSPARAMS1: MaxSlots [7:0], MaxIntrs [18:8], MaxPorts [31:24].
#define XHCI_HCS1_MAXSLOTS(v)  ((v) & 0xFFu)
#define XHCI_HCS1_MAXINTRS(v)  (((v) >> 8) & 0x7FFu)
#define XHCI_HCS1_MAXPORTS(v)  (((v) >> 24) & 0xFFu)

// HCSPARAMS2: Max Scratchpad Buffers is SPLIT -- Hi [25:21], Lo [31:27].
// Reading only the low field is a classic bug on a controller wanting
// more than 31 buffers, so both halves are combined here.
#define XHCI_HCS2_SPB_MAX(v)   ((((v) >> 21) & 0x1Fu) << 5 | (((v) >> 27) & 0x1Fu))

// HCCPARAMS1: AC64 [0], CSZ [2], xECP [31:16] in DWORDS from BAR0.
#define XHCI_HCC1_AC64(v)      ((v) & 1u)
#define XHCI_HCC1_CSZ(v)       (((v) >> 2) & 1u)   // 1 => 64-byte contexts
#define XHCI_HCC1_XECP(v)      (((v) >> 16) & 0xFFFFu)

// --- operational registers (BAR0 + CAPLENGTH) ------------------------
#define XHCI_USBCMD         0x00
#define XHCI_USBSTS         0x04
#define XHCI_PAGESIZE       0x08
#define XHCI_DNCTRL         0x14
#define XHCI_CRCR           0x18  // 64-bit
#define XHCI_DCBAAP         0x30  // 64-bit
#define XHCI_CONFIG         0x38
#define XHCI_PORTSC(p)      (0x400 + (0x10 * (p)))  // p is 0-based

#define XHCI_CMD_RS         (1u << 0)   // Run/Stop
#define XHCI_CMD_HCRST      (1u << 1)   // Host Controller Reset
#define XHCI_CMD_INTE       (1u << 2)   // Interrupter Enable
#define XHCI_CMD_HSEE       (1u << 3)

#define XHCI_STS_HCH        (1u << 0)   // HCHalted
#define XHCI_STS_HSE        (1u << 2)   // Host System Error
#define XHCI_STS_EINT       (1u << 3)   // Event Interrupt   -- RW1C
#define XHCI_STS_PCD        (1u << 4)   // Port Change Detect -- RW1C
#define XHCI_STS_CNR        (1u << 11)  // Controller Not Ready

// CRCR bits. RCS is the cycle state the controller should expect.
#define XHCI_CRCR_RCS       (1ull << 0)

// --- PORTSC ----------------------------------------------------------
//
// THE TRAP THAT DEFINES THIS REGISTER. Seven of its bits are RW1C
// (write 1 to CLEAR), and PED -- bit 1 -- is write-1-to-DISABLE. So the
// obvious `portsc |= PR; write(portsc)` disables the port AND clears
// every change bit that happened to read as 1. Nothing in this driver
// writes PORTSC directly; everything goes through xhci_portsc_write()
// in xhci.c, which masks this set off. See its comment.
#define XHCI_PORTSC_CCS     (1u << 0)   // Current Connect Status  (RO)
#define XHCI_PORTSC_PED     (1u << 1)   // Port Enabled/Disabled   (RW1C-to-disable!)
#define XHCI_PORTSC_OCA     (1u << 3)   // Over-current Active     (RO)
#define XHCI_PORTSC_PR      (1u << 4)   // Port Reset
#define XHCI_PORTSC_PLS(v)  (((v) >> 5) & 0xFu)   // Port Link State
#define XHCI_PORTSC_PP      (1u << 9)   // Port Power
#define XHCI_PORTSC_SPEED(v) (((v) >> 10) & 0xFu)
#define XHCI_PORTSC_CSC     (1u << 17)  // Connect Status Change   -- RW1C
#define XHCI_PORTSC_PEC     (1u << 18)  // Port Enabled Change     -- RW1C
#define XHCI_PORTSC_WRC     (1u << 19)  // Warm Reset Change       -- RW1C
#define XHCI_PORTSC_OCC     (1u << 20)  // Over-current Change     -- RW1C
#define XHCI_PORTSC_PRC     (1u << 21)  // Port Reset Change       -- RW1C
#define XHCI_PORTSC_PLC     (1u << 22)  // Port Link State Change  -- RW1C
#define XHCI_PORTSC_CEC     (1u << 23)  // Port Config Error Chg   -- RW1C

// Every RW1C bit in PORTSC, as one mask. Writing this back verbatim
// would clear all of them, which is why a read-modify-write must mask
// it OFF and only then set the bit it wants.
#define XHCI_PORTSC_RW1C \
    (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | \
     XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

// PORTSC speed IDs. These are the DEFAULT xHCI protocol speed IDs; a
// controller may redefine them through a Supported Protocol capability,
// which this driver reads but does not yet remap (QEMU uses defaults).
#define XHCI_SPEED_FULL     1   // USB 2.0  12 Mb/s
#define XHCI_SPEED_LOW      2   // USB 2.0  1.5 Mb/s
#define XHCI_SPEED_HIGH     3   // USB 2.0  480 Mb/s
#define XHCI_SPEED_SUPER    4   // USB 3.0  5 Gb/s

// --- runtime registers (BAR0 + RTSOFF) -------------------------------
// Interrupter 0 lives at RTSOFF + 0x20; each interrupter is 32 bytes.
#define XHCI_IR0            0x20
#define XHCI_IMAN           0x00
#define XHCI_IMOD           0x04
#define XHCI_ERSTSZ         0x08
#define XHCI_ERSTBA         0x10  // 64-bit
#define XHCI_ERDP           0x18  // 64-bit

#define XHCI_IMAN_IP        (1u << 0)   // Interrupt Pending -- RW1C
#define XHCI_IMAN_IE        (1u << 1)   // Interrupt Enable
#define XHCI_ERDP_EHB       (1ull << 3) // Event Handler Busy -- RW1C

// --- TRBs ------------------------------------------------------------
// A TRB is 4 dwords. The type is control[15:10]; the cycle bit is
// control[0].
#define XHCI_TRB_CYCLE      (1u << 0)
#define XHCI_TRB_ENT        (1u << 1)   // Evaluate Next TRB
#define XHCI_TRB_ISP        (1u << 2)   // Interrupt on Short Packet
#define XHCI_TRB_CH         (1u << 4)   // Chain
#define XHCI_TRB_IOC        (1u << 5)   // Interrupt On Completion
#define XHCI_TRB_IDT        (1u << 6)   // Immediate Data
#define XHCI_TRB_TC         (1u << 1)   // Toggle Cycle (Link TRBs only)

#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_TYPE(v)    (((v) >> XHCI_TRB_TYPE_SHIFT) & 0x3Fu)
#define XHCI_TRB_SET_TYPE(t) ((uint32_t)(t) << XHCI_TRB_TYPE_SHIFT)

// Transfer ring TRB types
#define XHCI_TRB_NORMAL             1
#define XHCI_TRB_SETUP_STAGE        2
#define XHCI_TRB_DATA_STAGE         3
#define XHCI_TRB_STATUS_STAGE       4
#define XHCI_TRB_LINK               6
// Command ring TRB types
#define XHCI_TRB_ENABLE_SLOT        9
#define XHCI_TRB_DISABLE_SLOT       10
#define XHCI_TRB_ADDRESS_DEVICE     11
#define XHCI_TRB_CONFIGURE_ENDPOINT 12
#define XHCI_TRB_EVALUATE_CONTEXT   13
#define XHCI_TRB_NOOP_CMD           23
// Event ring TRB types
#define XHCI_TRB_TRANSFER_EVENT     32
#define XHCI_TRB_CMD_COMPLETION     33
#define XHCI_TRB_PORT_STATUS_CHANGE 34

// Completion codes. Only the ones this driver can act on are named;
// xhci_completion_name() has the full table, as a TABLE rather than a
// switch (tools/check_dispatch.py, and it is data either way).
#define XHCI_CC_INVALID             0
#define XHCI_CC_SUCCESS             1
#define XHCI_CC_DATA_BUFFER_ERROR   2
#define XHCI_CC_BABBLE              3
#define XHCI_CC_USB_TRANSACTION_ERR 4
#define XHCI_CC_TRB_ERROR           5
#define XHCI_CC_STALL               6
#define XHCI_CC_SHORT_PACKET        13

// --- extended capabilities (xECP) ------------------------------------
#define XHCI_XECP_ID(v)     ((v) & 0xFFu)
#define XHCI_XECP_NEXT(v)   (((v) >> 8) & 0xFFu)   // in DWORDS, 0 = end
#define XHCI_XECP_ID_LEGACY 1   // USB Legacy Support
#define XHCI_XECP_ID_PROTO  2   // Supported Protocol

// USB Legacy Support (xECP id 1)
#define XHCI_LEGSUP_BIOS_OWNED (1u << 16)
#define XHCI_LEGSUP_OS_OWNED   (1u << 24)

// A TRB, as the controller sees it. 16 bytes, and the layout is the
// same for every type -- what the first three dwords MEAN varies.
struct xhci_trb {
    uint32_t p0;
    uint32_t p1;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16)));

// An Event Ring Segment Table entry.
struct xhci_erst_entry {
    uint64_t base;
    uint32_t size;    // TRBs in the segment
    uint32_t rsvd;
} __attribute__((packed));

#endif
