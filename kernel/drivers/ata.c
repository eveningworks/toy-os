// ATA/IDE, primary bus, master drive, 28-bit LBA. See ata.h for why
// this was chosen over AHCI, and for the two-transfer-path design (PIO
// fallback vs. Bus-Master DMA + IRQ14) this file implements. The
// detection sequence in ata_init() follows the standard "identify, bail
// out early at every step that doesn't look like a plain ATA master"
// algorithm (floating bus -> no drive at all -> non-ATA device ->
// success), same shape widely documented for this hardware, since
// getting any one of those checks wrong tends to mean hanging forever
// polling a status register that will never change, rather than a
// clean failure. This part is unchanged from before build 430.
#include "ata.h"
#include "io.h"
#include "pci.h"
#include "pmm.h"
#include "irq.h"
#include "pic.h"
#include "timer.h"
#include "klog.h"
#include "idt.h"
#include <stddef.h>

#define ATA_PRIMARY_IO  0x1F0
#define ATA_PRIMARY_IRQ 14 // the primary IDE channel's fixed legacy IRQ line

#define REG_DATA        (ATA_PRIMARY_IO + 0)
#define REG_SECCOUNT    (ATA_PRIMARY_IO + 2)
#define REG_LBA_LOW     (ATA_PRIMARY_IO + 3)
#define REG_LBA_MID     (ATA_PRIMARY_IO + 4)
#define REG_LBA_HIGH    (ATA_PRIMARY_IO + 5)
#define REG_DRIVE_HEAD  (ATA_PRIMARY_IO + 6)
#define REG_STATUS      (ATA_PRIMARY_IO + 7)
#define REG_COMMAND     (ATA_PRIMARY_IO + 7)

#define STATUS_BSY 0x80
#define STATUS_DRQ 0x08
#define STATUS_ERR 0x01

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_READ_DMA      0xC8
#define CMD_WRITE_DMA     0xCA
#define CMD_CACHE_FLUSH   0xE7
#define CMD_IDENTIFY      0xEC

// Bounded retry counts, not infinite loops -- if there's genuinely no
// drive attached (very possible: this is a hobby OS, most runs won't
// have a `-drive` argument), every wait_*() here needs to give up and
// report failure rather than hang the boot forever.
#define ATA_POLL_LIMIT 100000

static int g_present = 0;

static int wait_not_busy(void) {
    for (int i = 0; i < ATA_POLL_LIMIT; i++) {
        if (!(inb(REG_STATUS) & STATUS_BSY)) return 1;
        io_wait();
    }
    return 0;
}

static int wait_drq(void) {
    for (int i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(REG_STATUS);
        if (status & STATUS_ERR) return 0;
        if (status & STATUS_DRQ) return 1;
        io_wait();
    }
    return 0;
}

static void select_lba(uint32_t lba) {
    outb(REG_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F)); // 0xE0: LBA mode, master
    outb(REG_SECCOUNT, 1);
    outb(REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}

// ---------------------------------------------------------------------
// Bus-Master IDE DMA (build 430) -- see ata.h's top comment for the
// overall design. Register offsets/bit meanings below are the
// standard PIIX-and-everything-since Bus Master IDE layout, relative
// to the I/O BAR (BAR4) the IDE controller exposes on the PCI bus --
// present and usable even though this controller otherwise runs in
// legacy/compatibility mode at the fixed ports above (BAR4 is the one
// part of a "legacy mode" IDE controller that's still PCI-relocated).
// ---------------------------------------------------------------------

#define BM_CMD    0x0 // 8-bit: bit0 start/stop, bit3 direction
#define BM_STATUS 0x2 // 8-bit: bit0 active, bit1 error (W1C), bit2 IRQ (W1C)
#define BM_PRDT   0x4 // 32-bit: physical address of the PRD table

#define BM_CMD_START 0x01
#define BM_CMD_READ  0x08 // direction bit: set = device writes to memory (an ATA READ)

#define BM_STATUS_ERROR 0x02
#define BM_STATUS_IRQ   0x04

// One Physical Region Descriptor -- a single entry is always enough
// here: every transfer this driver issues is exactly one 512-byte
// sector, well under a PRD's 64KB max and never split across
// descriptors.
struct prd {
    uint32_t base;
    uint16_t count;
    uint16_t flags; // bit15: EOT (end of table)
} __attribute__((packed));

#define PRD_EOT 0x8000

// ~3s at the 100Hz PIT tick rate -- bounded, same "never hang forever"
// philosophy as ATA_POLL_LIMIT above, in case the completion IRQ never
// arrives (a genuine driver bug, or a hardware/emulation quirk) rather
// than trusting it unconditionally.
#define DMA_WAIT_TICKS 300

static int g_dma_available = 0;
static uint16_t g_bm_io = 0;

// One pmm_alloc_contiguous(2) call at init provides both of these --
// this driver's (and this allocator's) first real caller, see build
// 410. g_prd/g_prd_phys is the first frame (only 8 bytes of it used,
// for exactly one struct prd); g_dma_buf/g_dma_buf_phys is the second,
// used as a bounce buffer for the actual sector data rather than
// DMA'ing directly to/from a caller-supplied pointer -- guarantees the
// DMA target never crosses a 64KB boundary (a PRD requirement) or
// turns out to be some address this driver hasn't verified is safe to
// hand a device, at the cost of one extra 512-byte copy per transfer.
static volatile struct prd *g_prd = NULL;
static uint64_t g_prd_phys = 0;
static uint8_t *g_dma_buf = NULL;
static uint64_t g_dma_buf_phys = 0;

static volatile int g_dma_irq_fired = 0;

// Registered for IRQ14 (see ata_init_dma()) -- every hardware-IRQ
// handler has this signature (irq.h). The drive's own status register
// still needs reading afterward to actually acknowledge its IRQ line
// (see dma_transfer()) -- this handler only records that the IRQ
// happened, so wait_dma_irq()'s hlt loop can stop waiting.
static void ata_irq_handler(uint64_t *regs) {
    (void)regs;
    g_dma_irq_fired = 1;
}

// Blocks until ata_irq_handler() fires or a bounded wait elapses -- but
// HOW it blocks depends on where this got called from, which is exactly
// what isr_in_progress() (idt.h) answers:
//
//   - Called from ordinary kernel-space code (fs_init() at boot, or any
//     of the apps/ code that calls fs_write()/fs_read() directly --
//     shell commands, Notepad, the editor, timezone/font config saves;
//     apps run in kernel space in this OS, so this is the common case
//     for real disk I/O) -- interrupts are already enabled (idt_init()
//     ran long before ata_init(), and we're not nested inside any
//     interrupt handler), so this genuinely blocks via `hlt` (same
//     idle-until-any-interrupt pattern keyboard_getchar() uses) and the
//     CPU goes idle instead of spinning until IRQ14 actually fires.
//   - Called from inside a syscall (SYS_WRITE/SYS_READ, e.g. the
//     ring-3 *_test.c programs) -- isr_dispatch() runs `int 0x80`
//     through an interrupt gate, which means interrupts are OFF for the
//     whole syscall, and `hlt` here would just park forever with no way
//     to wake (not even the timer can tick). Worse, `sti`-then-block
//     would be actively unsafe, not just ineffective -- see idt.h's
//     isr_in_progress() doc comment for the exact g_next_kernel_rsp
//     reentrancy bug this avoids (already hit and documented once, for
//     SYS_READ_KEY). So instead: poll the Bus-Master status register's
//     own IRQ bit directly. The hardware still raises it regardless of
//     the CPU's interrupt-enable state -- IF only gates whether the CPU
//     SERVICES an IRQ, not whether the chipset/drive sets the status
//     bit -- so this still observes real DMA completion, it just
//     doesn't block on the CPU interrupt line to find out. (The PIC's
//     own pending IRQ14 from this transfer gets cleared for free the
//     next time interrupts are re-enabled -- irq_dispatch() always
//     sends EOI, so the eventual, otherwise-harmless spurious call to
//     ata_irq_handler() cleans it up.) pit_ticks() is frozen for the
//     same "interrupts are off" reason, so the bound here is a plain
//     iteration count (ATA_POLL_LIMIT), not wall-clock, matching
//     wait_not_busy()/wait_drq() above.
static int wait_dma_irq(void) {
    if (isr_in_progress()) {
        for (int i = 0; i < ATA_POLL_LIMIT; i++) {
            if (inb(g_bm_io + BM_STATUS) & BM_STATUS_IRQ) {
                g_dma_irq_fired = 1; // keep both paths' postcondition identical
                return 1;
            }
            io_wait();
        }
        return 0;
    }

    uint64_t start = pit_ticks();
    while (!g_dma_irq_fired) {
        if (pit_ticks() - start > DMA_WAIT_TICKS) return 0;
        __asm__ volatile ("hlt");
    }
    return 1;
}

// Looks for the IDE controller on the PCI bus (class 0x01, subclass
// 0x01 -- mass storage, IDE) and tries to stand up the Bus-Master DMA
// path through it. Leaves g_dma_available at 0 (its already-0 default)
// on ANY failure -- no PCI IDE controller found, its BAR4 isn't an
// I/O-space BAR, or the contiguous 2-frame allocation fails -- so
// ata_read_sector()/ata_write_sector() fall back to the PIO path
// automatically; see ata.h's top comment. Only called once ata_init()
// has already confirmed a drive is actually present, so this never
// spends the 2-frame allocation on a machine with no drive to use it.
static void ata_init_dma(void) {
    const struct pci_device *ide = NULL;
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d && d->class_code == 0x01 && d->subclass == 0x01) { ide = d; break; }
    }
    if (!ide) {
        klog_write("ata: no IDE controller on the PCI bus -- staying on PIO\n");
        return;
    }

    uint32_t bar4 = ide->bar[4];
    if (bar4 == 0 || !pci_bar_is_io(bar4)) {
        klog_write("ata: IDE controller has no usable Bus-Master I/O BAR -- staying on PIO\n");
        return;
    }

    uint64_t frames = pmm_alloc_contiguous(2);
    if (!frames) {
        klog_write("ata: out of contiguous memory for the PRDT/DMA buffer -- staying on PIO\n");
        return;
    }

    // Without this, the controller's BM_CMD/BM_STATUS/BM_PRDT registers
    // keep working and even report success, but no real memory
    // read/write cycles happen -- see pci.h's doc comment.
    pci_enable_bus_master(ide);

    g_bm_io = (uint16_t)pci_bar_addr(bar4);
    g_prd_phys = frames;
    g_prd = (volatile struct prd *)(uintptr_t)frames; // identity-mapped low 4GB, see vmm.c
    g_dma_buf_phys = frames + 4096;
    g_dma_buf = (uint8_t *)(uintptr_t)g_dma_buf_phys;

    irq_register_handler(ATA_PRIMARY_IRQ, ata_irq_handler);
    pic_clear_mask(ATA_PRIMARY_IRQ);

    g_dma_available = 1;
    klog_write("ata: Bus-Master DMA available, IRQ14-driven\n");
}

// One sector, either direction -- see the field comments above for why
// this always goes through the bounce buffer rather than the caller's
// own pointer.
static int dma_transfer(uint32_t lba, void *buf, int is_write) {
    g_prd->base = (uint32_t)g_dma_buf_phys;
    g_prd->count = ATA_SECTOR_SIZE;
    g_prd->flags = PRD_EOT;

    if (is_write) {
        // The controller reads FROM memory in this direction -- stage
        // the caller's data into the bounce buffer first.
        const uint8_t *src = (const uint8_t *)buf;
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) g_dma_buf[i] = src[i];
    }

    outl(g_bm_io + BM_PRDT, (uint32_t)g_prd_phys);
    outb(g_bm_io + BM_STATUS, BM_STATUS_ERROR | BM_STATUS_IRQ); // W1C: clear stale bits from any earlier transfer
    outb(g_bm_io + BM_CMD, is_write ? 0 : BM_CMD_READ); // program direction, not started yet

    if (!wait_not_busy()) return 0;
    select_lba(lba);
    outb(REG_COMMAND, is_write ? CMD_WRITE_DMA : CMD_READ_DMA);

    g_dma_irq_fired = 0;
    outb(g_bm_io + BM_CMD, (uint8_t)((is_write ? 0 : BM_CMD_READ) | BM_CMD_START)); // go

    int ok = wait_dma_irq();

    outb(g_bm_io + BM_CMD, 0); // stop the bus-master engine regardless of outcome
    uint8_t bm_status = inb(g_bm_io + BM_STATUS);
    outb(g_bm_io + BM_STATUS, BM_STATUS_ERROR | BM_STATUS_IRQ); // W1C, ready for the next transfer
    (void)inb(REG_STATUS); // reading the drive's own status register acknowledges its IRQ line

    if (!ok || (bm_status & BM_STATUS_ERROR)) return 0;

    if (!is_write) {
        uint8_t *dst = (uint8_t *)buf;
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) dst[i] = g_dma_buf[i];
    } else {
        // Best-effort cache flush, same as the PIO write path below.
        if (wait_not_busy()) {
            outb(REG_COMMAND, CMD_CACHE_FLUSH);
            wait_not_busy();
        }
    }
    return 1;
}

// ---------------------------------------------------------------------
// PIO fallback -- exactly the driver this file was before build 430,
// used automatically whenever ata_init_dma() couldn't stand up the DMA
// path (see its comment for every reason that can happen).
// ---------------------------------------------------------------------

static int pio_read_sector(uint32_t lba, void *buf) {
    if (!wait_not_busy()) return 0;

    select_lba(lba);
    outb(REG_COMMAND, CMD_READ_SECTORS);

    if (!wait_drq()) return 0;

    uint16_t *p = (uint16_t *)buf;
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) p[i] = inw(REG_DATA);
    return 1;
}

static int pio_write_sector(uint32_t lba, const void *buf) {
    if (!wait_not_busy()) return 0;

    select_lba(lba);
    outb(REG_COMMAND, CMD_WRITE_SECTORS);

    if (!wait_drq()) return 0;

    const uint16_t *p = (const uint16_t *)buf;
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) outw(REG_DATA, p[i]);

    // Best-effort: ask the drive to flush its write cache so the data
    // is actually durable, not just handed off. Failure here doesn't
    // undo the write above (PIO WRITE SECTORS has already transferred
    // the data by this point) -- it just means we can't be as sure it
    // survives a real power loss, which for QEMU's emulated disk backed
    // by a host file is already close to moot.
    if (wait_not_busy()) {
        outb(REG_COMMAND, CMD_CACHE_FLUSH);
        wait_not_busy();
    }

    return 1;
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

void ata_init(void) {
    g_present = 0;
    g_dma_available = 0;

    // Select primary master (0xA0; slave would be 0xB0 -- this driver
    // never touches the slave, only ever the one drive most setups
    // (including this project's QEMU invocation) actually attach).
    outb(REG_DRIVE_HEAD, 0xA0);
    io_wait(); io_wait(); io_wait(); io_wait(); // ~400ns select settle time

    if (inb(REG_STATUS) == 0xFF) return; // floating bus: no controller here at all

    outb(REG_SECCOUNT, 0);
    outb(REG_LBA_LOW, 0);
    outb(REG_LBA_MID, 0);
    outb(REG_LBA_HIGH, 0);
    outb(REG_COMMAND, CMD_IDENTIFY);

    if (inb(REG_STATUS) == 0) return; // controller present, but no drive on it

    if (!wait_not_busy()) return;

    // A real ATA drive leaves these two at 0 throughout IDENTIFY; an
    // ATAPI device (like this project's boot CD, were it ever on this
    // bus instead of the secondary one -- see ata.h) reports its packet
    // signature here instead. Bail rather than try to read a "sector"
    // from something that isn't a plain block device.
    if (inb(REG_LBA_MID) != 0 || inb(REG_LBA_HIGH) != 0) return;

    if (!wait_drq()) return;

    // Must still read all 256 words even though nothing here is
    // inspected yet (no capacity/feature checks -- see the header) --
    // leaving them unread would desync the controller for the next
    // command.
    for (int i = 0; i < 256; i++) (void)inw(REG_DATA);

    g_present = 1;

    // Only worth attempting once a drive is confirmed present -- see
    // ata_init_dma()'s own comment.
    ata_init_dma();
}

int ata_present(void) {
    return g_present;
}

int ata_dma_active(void) {
    return g_dma_available;
}

int ata_read_sector(uint32_t lba, void *buf) {
    if (!g_present) return 0;
    if (g_dma_available) return dma_transfer(lba, buf, 0);
    return pio_read_sector(lba, buf);
}

int ata_write_sector(uint32_t lba, const void *buf) {
    if (!g_present) return 0;
    if (g_dma_available) return dma_transfer(lba, (void *)(uintptr_t)buf, 1);
    return pio_write_sector(lba, buf);
}
