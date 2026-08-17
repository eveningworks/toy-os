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
#include "string.h"
#include "idt.h"
#include "debugflags.h"
#include "fault_inject.h"
#include "ata_cache.h"
#include <stddef.h>

#define ATA_PRIMARY_IO  0x1F0
#define ATA_PRIMARY_IRQ 14 // the primary IDE channel's fixed legacy IRQ line

#define REG_DATA        (ATA_PRIMARY_IO + 0)
#define REG_FEATURES    (ATA_PRIMARY_IO + 1)
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
// DATA SET MANAGEMENT. With the TRIM bit set in the Features register it
// tells the drive "these LBA ranges no longer hold data you need to
// keep" -- see ata_trim() below.
#define CMD_DATA_SET_MGMT 0x06
#define DSM_FEATURE_TRIM  0x01

// Bounded retry counts, not infinite loops -- if there's genuinely no
// drive attached (very possible: this is a hobby OS, most runs won't
// have a `-drive` argument), every wait_*() here needs to give up and
// report failure rather than hang the boot forever.
#define ATA_POLL_LIMIT 100000

static int g_present = 0;
// IDENTIFY word 169 bit 0 -- see ata_trim() at the end of this file.
static int g_trim_supported = 0;

// Total addressable sectors, from IDENTIFY words 60-61 (the 28-bit LBA
// capacity field) -- 0 if unknown, which is what every caller treats as
// "no bound available, don't range-check". Every one of IDENTIFY's 256
// words has always been read and discarded here; keeping two of them
// costs nothing and gives the filesystem a real answer to "how big is
// this disk" instead of tfs.c's hardcoded FS_DISK_TOTAL_BYTES guess
// (which silently allocates past the end of a smaller image -- see
// tfs.c's own clamp).
static uint32_t g_sector_count = 0;

// ~1s at the 100Hz PIT tick rate. See wait_not_busy() below for why a
// wall-clock bound, rather than ATA_POLL_LIMIT alone, is what this wait
// needs.
#define BUSY_WAIT_TICKS 100

static int spin_not_busy(void) {
    for (int i = 0; i < ATA_POLL_LIMIT; i++) {
        if (!(inb(REG_STATUS) & STATUS_BSY)) return 1;
        io_wait();
    }
    return 0;
}

// "Is the drive ready to accept a new command?" -- the wait that runs
// BEFORE anything is issued, as opposed to wait_dma_irq()'s wait for a
// command already in flight.
//
// Bounded two different ways for the same reason wait_dma_irq() is: a
// wall-clock budget needs pit_ticks() to advance, and it doesn't inside
// a syscall, because `int 0x80` is an interrupt gate so IF stays clear
// for the whole handler (see idt.h's isr_in_progress()). So this spends
// real time when it can and falls back to the fixed spin when it can't.
//
// Why it needs a wall-clock bound at all: ATA_POLL_LIMIT's 100000
// iterations measure out to ~12ms on this emulated hardware, and
// dma_transfer_with_retry()'s three attempts gave the drive ~37ms in
// total to stop being busy -- while DMA_WAIT_TICKS granted the very
// same transfer 5 SECONDS once its command was in flight. A host-side
// I/O stall falls straight through that asymmetry: the guest CPU keeps
// running at full speed while the emulated drive doesn't, so a fixed
// spin count elapses long before the drive comes back, and all three
// retries burn inside one stall. That surfaced as `dma write failed
// after 3 attempts (lba 2, last reason: drive stayed busy, command
// never issued)` during a `stress` run started seconds after
// grub-mkrescue had written a 746MB ISO to the same host disk.
// DMA_WAIT_TICKS's own comment records widening the completion half
// against exactly this class of stall; this half was missed then.
static int wait_not_busy(void) {
    if (isr_in_progress()) return spin_not_busy();

    uint64_t start = pit_ticks();
    // The iteration cap is belt-and-braces, not the real bound: this
    // path assumes ticks advance whenever isr_in_progress() is false,
    // which holds today, but a wall-clock loop that's WRONG about that
    // hangs the machine instead of failing one write. Sized well past
    // BUSY_WAIT_TICKS so it never fires first in normal operation.
    for (uint64_t guard = 0; guard < (uint64_t)ATA_POLL_LIMIT * 200; guard++) {
        if (!(inb(REG_STATUS) & STATUS_BSY)) return 1;
        if (pit_ticks() - start > BUSY_WAIT_TICKS) return 0;
        io_wait();
    }
    return 0;
}

// ~1s, same bound and same reasoning as BUSY_WAIT_TICKS above. Kept as
// its own name rather than shared, because the two answer different
// questions ("may I send a command?" vs "is a sector's data ready?")
// and there's no reason they'd have to move together.
#define DRQ_WAIT_TICKS 100

// Why the last PIO wait failed. Same idea, and the same one-static-
// pointer cost, as g_dma_fail_reason further down: wait_drq() returns 0
// both when the drive actively reported an error and when it simply
// never raised DRQ, and those have completely different causes -- a bad
// sector versus a drive that isn't responding. Collapsing them cost
// real detective work on the DMA side once (see that variable's
// comment); this is the same fix on the path that never got it.
static const char *g_pio_fail_reason = "unknown";

static int spin_drq(void) {
    for (int i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(REG_STATUS);
        if (status & STATUS_ERR) { g_pio_fail_reason = "drive reported ERR"; return 0; }
        if (status & STATUS_DRQ) return 1;
        io_wait();
    }
    g_pio_fail_reason = "DRQ never asserted (spin limit, syscall context)";
    return 0;
}

// "Is a sector's worth of data ready to move?" -- the PIO path's wait,
// where the CPU shovels every word through REG_DATA itself instead of
// the controller doing it. The drive raises DRQ once per SECTOR, not
// once per command, so unlike wait_not_busy() this sits inside the
// transfer loop and runs `count` times per request.
//
// Same fixed-spin problem wait_not_busy() had, so it gets the same
// isr_in_progress() split -- see that function's comment for the full
// reasoning about why a spin count isn't a duration, and why the
// wall-clock half can't simply be used everywhere.
//
// Being a per-sector hot loop, the ordering here matters in a way it
// doesn't up there: status is read and both exits are taken BEFORE the
// clock is consulted, so the overwhelmingly common case (DRQ already
// set on the first look) costs one extra `pit_ticks()` per sector and
// nothing else. That's a volatile counter read next to a port-I/O read
// that dominates it -- measured throughput was unchanged.
//
// Still returns 0 for BOTH "the drive reported an error" and "gave up
// waiting" -- callers only need pass/fail -- but it now records WHICH in
// g_pio_fail_reason above, so the log says which happened. That's the
// same shape g_dma_fail_reason settled on: a pass/fail return for
// control flow, a reason string for the human reading dmesg. A caller
// that genuinely needs to branch on the difference would want an enum
// return; nothing does yet.
static int wait_drq(void) {
    if (isr_in_progress()) return spin_drq();

    uint64_t start = pit_ticks();
    for (uint64_t guard = 0; guard < (uint64_t)ATA_POLL_LIMIT * 200; guard++) {
        uint8_t status = inb(REG_STATUS);
        if (status & STATUS_ERR) { g_pio_fail_reason = "drive reported ERR"; return 0; }
        if (status & STATUS_DRQ) return 1;
        if (pit_ticks() - start > DRQ_WAIT_TICKS) {
            g_pio_fail_reason = "DRQ never asserted within the wall-clock bound";
            return 0;
        }
        io_wait();
    }
    g_pio_fail_reason = "DRQ wait hit its iteration guard";
    return 0;
}

// `count` sectors, not just 1 -- see ata.h's ATA_MAX_SECTORS_PER_XFER.
// REG_SECCOUNT is genuinely 8 bits wide (0 there means "256" per the
// ATA spec); this driver never asks for more than 128 (one PRD's worth,
// see ata_max_sectors_per_xfer()), so the 0-means-256 encoding never
// comes up.
static void select_lba(uint32_t lba, uint8_t count) {
    outb(REG_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F)); // 0xE0: LBA mode, master
    outb(REG_SECCOUNT, count);
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

// ~5s at the 100Hz PIT tick rate -- bounded, same "never hang forever"
// philosophy as ATA_POLL_LIMIT above, in case the completion IRQ never
// arrives (a genuine driver bug, or a hardware/emulation quirk) rather
// than trusting it unconditionally. Was 3s; widened after a `stress`
// run on real hardware traced a "dma write failed after 3 attempts"
// (dma_transfer_with_retry() below, exhausting all 3 of ITS attempts)
// to the host's disk activity monitor showing a burst write right at
// that moment -- root-caused to the host filesystem (Btrfs) batching
// up copy-on-write metadata into a periodic transaction commit,
// independent of QEMU's own disk-cache mode (tried cache=writethrough
// first; it didn't help, since the bottleneck was never in QEMU's
// caching layer -- see CHANGELOG.md). `disk.img` itself now carries
// Btrfs's `+C` (no-COW) attribute as the real fix for that burst, but
// widening this bound too costs nothing on the success path and adds
// a little more headroom against whatever comparable host-side stall
// shows up next -- a genuinely dead/hung drive still surfaces as a
// hard failure, just up to ~2s later than before.
//
// wait_not_busy() above is the OTHER half of this, and only got the
// same treatment much later: widening the completion bound here while
// leaving the pre-issue bound at a fixed spin count meant the driver
// waited 5s for a command in flight and ~12ms for a drive that hadn't
// finished the previous one. Keep the two in mind together.
#define DMA_WAIT_TICKS 500

// ...and the per-ATTEMPT budget, which is a different question from the
// total one above.
//
// The 5s bound is right for "how long may a transfer take before the
// drive is declared dead", and badly wrong for "how long may the first
// attempt wait before trying again", because the whole of it is spent
// with the caller blocked -- and the caller is often the WM, so it is
// spent with the desktop frozen. A completion that goes missing for any
// reason (this driver had a lost-wakeup race until 2026-08-17) cost a
// flat 5 seconds of frozen UI and then succeeded instantly on attempt 2.
//
// So: escalate. Attempt 1 gives up quickly and retries, attempts 2 and 3
// widen out to the full bound, and the TOTAL budget across attempts is
// unchanged -- which is what the 3s -> 5s widening was actually
// protecting (a host-side burst stall, see above). A transient miss now
// costs 0.3s instead of 5s; a genuinely stalled host still gets every
// bit of the headroom it got before.
static uint64_t dma_attempt_ticks(int attempt) {
    if (attempt <= 1) return 30;   // 0.3s -- ~300x a real transfer
    if (attempt == 2) return 100;  // 1.0s
    return DMA_WAIT_TICKS;         // 5.0s, the full bound, on the last try
}

static int g_dma_available = 0;

// Forces transfers down the PIO path even when the hardware has working
// DMA. Exists because the PIO path was otherwise UNREACHABLE on every
// machine this OS boots: ata_init_dma() succeeds under QEMU and on
// ordinary PC hardware, so ~100 lines of fallback driver never ran, and
// could not be tested at all. Untested fallback code that only executes
// in an emergency is the worst kind to be wrong.
//
// Second use, which is not hypothetical: comparing a known-good PIO
// transfer against DMA is how an earlier session root-caused a DMA
// failure to a host-side stall rather than a driver bug (see
// CHANGELOG.md). That comparison had to be done by hand-editing the
// driver; now it's `ata nodma on`.
static int g_dma_forced_off = 0;

// The single question every dispatch site asks. Deliberately one
// helper rather than `g_dma_available && !g_dma_forced_off` repeated at
// each site: ata_max_sectors_per_xfer() reports a SMALLER cap for PIO,
// so a site that checked the flags differently from the site that sets
// the cap would let a caller batch 128 sectors into a path that can't
// take them.
static int dma_in_use(void);
static uint16_t g_bm_io = 0;

// Backs ata_flush_begin()/ata_flush_end() (ata.h) -- see that header's
// doc comment for the full "why". A depth counter, not a boolean, so
// nested begin/end pairs (a batched caller invoked from inside another
// batch) don't flush early on the inner end(). maybe_flush() is what
// dma_transfer()'s write path and pio_write_sectors() call where they
// used to flush unconditionally; ata_flush_end() calls the same
// helper once its own depth reaches 0, so there's exactly one place
// that decides "should a flush happen right now."
static int g_flush_defer_depth = 0;

static void maybe_flush(void) {
    if (g_flush_defer_depth != 0) return;
    if (!g_present) return; // nothing to flush, and REG_COMMAND would be meaningless
    if (wait_not_busy()) {
        outb(REG_COMMAND, CMD_CACHE_FLUSH);
        wait_not_busy();
    }
}

void ata_flush_begin(void) {
    g_flush_defer_depth++;
}

void ata_flush_end(void) {
    if (g_flush_defer_depth > 0) g_flush_defer_depth--;
    maybe_flush();
}

// Flushes RIGHT NOW, whatever the deferral depth. Everything above is
// about removing flushes a caller doesn't need; this is for the caller
// that needs one at a specific point and can't express that with
// begin/end -- a write-ahead journal, where "write N must be durable
// before write N+1 is issued" is the entire point of the structure.
//
// ata_flush_end() can't serve as that barrier: it only flushes once its
// own depth reaches 0, so a journal sequence running inside an outer
// batch would silently get no barrier at all. That's not hypothetical
// -- tfs.c's fsck repair pass calls persist_record() inside a
// write_batch_begin()/end() pair, which suppressed every one of the
// journal's own flushes.
// The DRIVE's own cache flush, with nothing of ours in front of it.
// This is what the write-back cache calls once its dirty lines are out.
static int ata_flush_raw(void) {
    if (!g_present) return 1; // nothing to flush is success, not failure
    if (!wait_not_busy()) return 0;
    outb(REG_COMMAND, CMD_CACHE_FLUSH);
    return wait_not_busy();
}

// RETURNS A STATUS, and callers must look. It was `void`, which was
// survivable while a write reached the platter before this was called
// and is not survivable with a write-back cache in the path: this is
// where a deferred write's failure surfaces, and it is what TFS3's
// journal barriers mean by "durable". A barrier that cannot fail cannot
// keep the journal honest -- see ata_cache.h.
int ata_flush_now(void) {
    if (!g_present) return 1;
    if (atac_enabled()) return atac_flush(); // writes back, then ata_flush_raw()
    return ata_flush_raw();
}

// Closes a deferral region WITHOUT the flush ata_flush_end() would
// issue. Only correct for a caller that has already placed its own
// ata_flush_now() barriers where durability actually matters AND whose
// remaining trailing writes are safe to lose in a crash. persist_record()
// (tfs.c) is the motivating case: its last write clears the journal
// header, and losing that write costs one redundant (idempotent) replay
// on the next boot, nothing more. Anything less clear-cut should use
// ata_flush_end().
void ata_flush_end_no_flush(void) {
    if (g_flush_defer_depth > 0) g_flush_defer_depth--;
}

// One pmm_alloc_contiguous() call at init provides both of these --
// this driver's (and this allocator's) first real caller, see build
// 410. g_prd/g_prd_phys is the first frame (only 8 bytes of it used,
// for exactly one struct prd); g_dma_buf/g_dma_buf_phys is the frames
// after it, used as a bounce buffer for the actual sector data rather
// than DMA'ing directly to/from a caller-supplied pointer -- guarantees
// the DMA target never crosses a 64KB boundary (a PRD requirement) or
// turns out to be some address this driver hasn't verified is safe to
// hand a device, at the cost of one extra copy per transfer.
//
// The bounce buffer is DMA_BUF_FRAMES frames, not one: its size is
// what caps ATA_MAX_SECTORS_PER_XFER, and that cap used to be the
// filesystem's real throughput ceiling -- 4KB per buffer meant one ATA
// command + one completion IRQ per 4KB of file data, no matter how
// sequential the write was. 64KB is the most a single PRD entry can
// describe (its byte count is 16-bit, with 0 meaning 64KB), so it's
// the natural stopping point without adding scatter-gather.
//
// g_dma_buf_frames records what was ACTUALLY allocated: if the
// contiguous allocation for the big buffer fails (a fragmented pool),
// ata_init_dma() retries for the original 2 frames and the driver runs
// exactly as it did before this existed, just at the smaller limit --
// see ata_max_sectors_per_xfer().
#define DMA_BUF_FRAMES 16 // 16 * 4096 = 65536 bytes = 128 sectors, one full PRD entry's worth

static volatile struct prd *g_prd = NULL;
static uint64_t g_prd_phys = 0;
static uint8_t *g_dma_buf = NULL;
static uint64_t g_dma_buf_phys = 0;
static uint32_t g_dma_buf_frames = 0; // 0 until ata_init_dma() succeeds

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
static int wait_dma_irq(uint64_t budget_ticks) {
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
        if (pit_ticks() - start > budget_ticks) return 0;
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

    // One frame for the PRD table plus the bounce buffer's frames. Try
    // for the full 64KB buffer first; fall back to the original 4KB one
    // if the pool can't produce that many contiguous frames, since a
    // smaller DMA window is still far better than dropping to PIO.
    uint32_t buf_frames = DMA_BUF_FRAMES;
    uint64_t frames = pmm_alloc_contiguous(1 + buf_frames);
    if (!frames) {
        buf_frames = 1;
        frames = pmm_alloc_contiguous(1 + buf_frames);
        if (!frames) {
            klog_write("ata: out of contiguous memory for the PRDT/DMA buffer -- staying on PIO\n");
            return;
        }
        klog_write("ata: only got a 4KB DMA bounce buffer (contiguous pool too fragmented for 64KB)\n");
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
    g_dma_buf_frames = buf_frames;

    irq_register_handler(ATA_PRIMARY_IRQ, ata_irq_handler);
    pic_clear_mask(ATA_PRIMARY_IRQ);

    g_dma_available = 1;
    klog_write("ata: Bus-Master DMA available, IRQ14-driven, ");
    klog_write_dec(ata_max_sectors_per_xfer()); klog_write(" sectors/transfer\n");
}

// dma_issue()/dma_finish() are dma_transfer()'s old body split at the
// one point that actually blocks (wait_dma_irq()) -- dma_issue() does
// everything up through kicking the transfer off, dma_finish() does
// everything after the wait resolves. dma_transfer() below still calls
// both back-to-back with a blocking wait in between, so every existing
// caller (dma_transfer_with_retry(), and everything built on it) is
// byte-for-byte unchanged. dma_transfer_start()/dma_transfer_poll()
// (further below) call the same two halves but let the CALLER decide
// how to wait in between -- see their own comments for why that split
// is useful on its own, ahead of anything actually using it yet.
//
// `count` sectors (1..ATA_MAX_SECTORS_PER_XFER), either direction --
// see the field comments above g_prd/g_dma_buf for why this always
// goes through the bounce buffer rather than the caller's own pointer.
// One PRD descriptor covering `count * ATA_SECTOR_SIZE` bytes is
// enough -- no scatter-gather needed since the bounce buffer is
// already one physically contiguous frame (see ata_init_dma()).
//
// Returns 0 only on a same-call setup failure (wait_not_busy() timing
// out before the command even reached the drive) -- at that point
// nothing was issued, so there's nothing for a poll-style caller to
// wait on. Returns 1 once the command is genuinely in flight.
static int dma_issue(uint32_t lba, int count, void *buf, int is_write) {
    uint32_t bytes = (uint32_t)count * ATA_SECTOR_SIZE;
    g_prd->base = (uint32_t)g_dma_buf_phys;
    // A PRD's byte count is 16-bit, and the truncation at the maximum
    // is deliberate rather than a bug: a full 64KB transfer (128
    // sectors, ATA_MAX_SECTORS_PER_XFER) gives bytes == 65536, which
    // narrows to 0 here -- and 0 is exactly how the Bus Master IDE spec
    // encodes "64KB" in this field. Any smaller count stores its real
    // value. This is why 128 sectors is the hard ceiling: 129 would
    // truncate to a genuinely wrong small number instead.
    g_prd->count = (uint16_t)bytes;
    g_prd->flags = PRD_EOT;

    if (is_write) {
        // The controller reads FROM memory in this direction -- stage
        // the caller's data into the bounce buffer first.
        const uint8_t *src = (const uint8_t *)buf;
        for (uint32_t i = 0; i < bytes; i++) g_dma_buf[i] = src[i];
    }

    outl(g_bm_io + BM_PRDT, (uint32_t)g_prd_phys);
    outb(g_bm_io + BM_STATUS, BM_STATUS_ERROR | BM_STATUS_IRQ); // W1C: clear stale bits from any earlier transfer
    outb(g_bm_io + BM_CMD, is_write ? 0 : BM_CMD_READ); // program direction, not started yet

    if (!wait_not_busy()) return 0;

    // ARM THE COMPLETION FLAG BEFORE ANYTHING CAN RAISE THE INTERRUPT.
    // This clear used to sit after the command byte below, which is a
    // lost-wakeup race: the drive can complete and raise IRQ14 between
    // the command and the clear, the handler sets the flag, and then
    // this line wipes it. wait_dma_irq() then waits out the whole
    // DMA_WAIT_TICKS (5s) budget and the retry succeeds instantly --
    // which is exactly what it looked like in the log.
    //
    // The window is real but tiny, and it is a function of how fast the
    // transfer completes, so TCG effectively never hit it and KVM (host
    // page cache, completion in microseconds) hit it constantly: the
    // desktop froze 2-5 seconds per disk read under `make run-kvm` and
    // was clean under `make run`. Anything armed after the trigger has
    // this bug; the flag has to be armed first.
    g_dma_irq_fired = 0;

    select_lba(lba, (uint8_t)count);
    outb(REG_COMMAND, is_write ? CMD_WRITE_DMA : CMD_READ_DMA);
    outb(g_bm_io + BM_CMD, (uint8_t)((is_write ? 0 : BM_CMD_READ) | BM_CMD_START)); // go
    return 1;
}

// The other half of dma_issue() -- called once completion (or a
// timeout) has already been determined by whatever waited (blocking
// wait_dma_irq(), or a non-blocking poll loop). Stops the bus-master
// engine, acknowledges the drive's IRQ line, and on success copies a
// read's data out of the bounce buffer (or flushes, for a write).
static int dma_finish(int ok, int count, void *buf, int is_write) {
    uint32_t bytes = (uint32_t)count * ATA_SECTOR_SIZE;

    outb(g_bm_io + BM_CMD, 0); // stop the bus-master engine regardless of outcome
    uint8_t bm_status = inb(g_bm_io + BM_STATUS);
    outb(g_bm_io + BM_STATUS, BM_STATUS_ERROR | BM_STATUS_IRQ); // W1C, ready for the next transfer
    (void)inb(REG_STATUS); // reading the drive's own status register acknowledges its IRQ line

    if (!ok || (bm_status & BM_STATUS_ERROR)) return 0;

    if (!is_write) {
        uint8_t *dst = (uint8_t *)buf;
        for (uint32_t i = 0; i < bytes; i++) dst[i] = g_dma_buf[i];
    } else {
        // Best-effort cache flush, same as the PIO write path below --
        // suppressed while a caller has an ata_flush_begin()/end() batch
        // open (ata.h), same as pio_write_sectors()'s equivalent call.
        maybe_flush();
    }
    return 1;
}

// Why the last dma_transfer() failed. A bare "dma write failed after 3
// attempts" doesn't say whether the drive never accepted the command,
// the completion interrupt never arrived, or the controller reported an
// error -- and those have completely different causes. CI hit a
// transient failure that took real detective work to narrow down purely
// because the log didn't distinguish them; it costs one static pointer
// not to have that problem again.
static const char *g_dma_fail_reason = "unknown";

static int dma_transfer(uint32_t lba, int count, void *buf, int is_write,
                        int attempt) {
    if (!dma_issue(lba, count, buf, is_write)) {
        g_dma_fail_reason = "drive stayed busy, command never issued";
        return 0;
    }
    int ok = wait_dma_irq(dma_attempt_ticks(attempt));
    if (!ok) g_dma_fail_reason = isr_in_progress()
                 ? "completion IRQ never arrived (polled, syscall context)"
                 : "completion IRQ never arrived within the wall-clock bound";
    int done = dma_finish(ok, count, buf, is_write);
    if (ok && !done) g_dma_fail_reason = "controller reported a bus-master error";
    return done;
}

// ---------------------------------------------------------------------
// Non-blocking start/poll pair (Phase 1 of the async-I/O roadmap item,
// see docs/roadmap.md) -- built from the exact same dma_issue()/
// dma_finish() halves dma_transfer() uses above, so this doesn't
// duplicate the register-level protocol, just gives a second way to
// wait on it. No real caller uses this yet: fs.c/tfs.c still go
// through the blocking dma_transfer_with_retry() path below unchanged.
// This exists to prove the primitive works in isolation first (see
// ata_dma_nonblocking_selftest()) before anything higher up the stack
// (a steppable fs_write_range(), then wm_run() polling one) is built
// on top of it.
//
// Only one transfer can be "started" at a time -- enforced by
// g_pending.in_flight, since there's only one PRD/bounce buffer
// (g_prd/g_dma_buf) to share. A caller MUST poll to either
// ATA_POLL_DONE or ATA_POLL_FAILED before starting another; there's no
// queueing here, on purpose -- that's exactly the kind of policy a
// caller built on top of this (a pending-write-batch tracker, say)
// should own, not this driver-level primitive. enum ata_poll_result
// itself is declared in ata.h (dma_transfer_poll()'s return type is
// part of the public contract), not here.

static struct {
    int in_flight;
    int count;
    void *buf;
    int is_write;
    uint64_t start_tick; // only meaningful when NOT called from inside
                          // a syscall -- see dma_transfer_poll()'s use
                          // of it, mirroring wait_dma_irq()'s own split.
} g_pending;

// Kicks off a transfer without waiting for it. Returns 1 once genuinely
// in flight (poll it from here on), 0 on an immediate setup failure
// (see dma_issue()) -- in the 0 case nothing is pending, don't poll.
int dma_transfer_start(uint32_t lba, int count, void *buf, int is_write) {
    if (g_pending.in_flight) return 0; // caller bug: didn't poll the last one to completion
    if (!dma_issue(lba, count, buf, is_write)) return 0;
    g_pending.in_flight = 1;
    g_pending.count = count;
    g_pending.buf = buf;
    g_pending.is_write = is_write;
    g_pending.start_tick = pit_ticks();
    return 1;
}

// Checks the started transfer WITHOUT blocking -- one register read (or
// one flag check), then returns immediately either way. A caller polls
// this in its own loop (a wm_run() frame, a diagnostic test's own
// counted loop, whatever it's driving), doing other work between calls
// instead of sitting inside this function the way wait_dma_irq() does.
// Same isr_in_progress() split as wait_dma_irq() for what "has the IRQ
// happened" actually checks (see that function's comment for the full
// reasoning) -- the difference here is a single non-blocking check
// instead of a loop that doesn't return until it's true or timed out.
enum ata_poll_result dma_transfer_poll(void) {
    if (!g_pending.in_flight) return ATA_POLL_FAILED; // caller bug: nothing started

    int done;
    if (isr_in_progress()) {
        done = (inb(g_bm_io + BM_STATUS) & BM_STATUS_IRQ) != 0;
        if (done) g_dma_irq_fired = 1; // keep both paths' postcondition identical, same as wait_dma_irq()
    } else {
        done = g_dma_irq_fired != 0;
    }

    if (!done) {
        uint64_t elapsed = isr_in_progress() ? 0 : pit_ticks() - g_pending.start_tick;
        // The syscall-context path has no wall-clock bound available
        // (pit_ticks() is frozen with interrupts off, same reason
        // wait_dma_irq() uses ATA_POLL_LIMIT there instead) -- a
        // syscall-context caller is responsible for bounding its own
        // poll loop, same as wait_dma_irq()'s ATA_POLL_LIMIT does today.
        if (!isr_in_progress() && elapsed > DMA_WAIT_TICKS) {
            g_pending.in_flight = 0;
            dma_finish(0, g_pending.count, g_pending.buf, g_pending.is_write);
            return ATA_POLL_FAILED;
        }
        return ATA_POLL_PENDING;
    }

    g_pending.in_flight = 0;
    int ok = dma_finish(1, g_pending.count, g_pending.buf, g_pending.is_write);
    return ok ? ATA_POLL_DONE : ATA_POLL_FAILED;
}

// A single dma_transfer() failure isn't necessarily the drive/data
// actually being bad -- wait_dma_irq()'s 3s bound (DMA_WAIT_TICKS) can
// be tripped by a merely-late IRQ (host scheduling jitter under real
// desktop load: other processes stealing the CPU from the QEMU
// process, background I/O, etc. -- conditions this project's own dev/
// test sandbox never reproduces, but a real machine running this in a
// VM alongside everything else on your desktop absolutely can). The
// driver used to treat that identically to a genuine hardware error --
// one miss and the whole transfer, and by extension whatever multi-
// block operation it was part of (e.g. `stress`'s multi-MB write/read
// pass), failed outright with no second attempt. Found live: `stress
// 10` failing non-deterministically ("usually", not always) on real
// hardware/QEMU while the exact same code path never failed in this
// project's sandboxed test runs -- the signature of a transient timing
// miss, not a reproducible logic bug. Retrying the whole transfer
// (re-issuing the command from scratch, not just re-waiting on the
// same one) a bounded number of times absorbs that kind of one-off
// miss without masking a REAL failure -- a drive that's actually
// erroring, or genuinely gone, will keep erroring across every retry
// and still surface as a hard failure once ATA_DMA_MAX_RETRIES is
// exhausted, same as before this existed.
#define ATA_DMA_MAX_RETRIES 3

// ~250ms at the 100Hz PIT tick rate, multiplied by the attempt number
// so three attempts span roughly 0.25s + 0.5s of waiting rather than
// retrying instantly.
#define RETRY_BACKOFF_TICKS 25

// Pause between retry attempts. Retrying a transfer immediately after
// it failed is the one thing guaranteed not to help when the cause is
// a host-side stall: without this, all three attempts finish inside the
// same stall and the retry budget buys nothing. Same context split as
// wait_not_busy() above, and the `hlt` follows the project's standing
// rule for blocking waits -- hlt when it's safe, poll when inside a
// syscall (see docs/decisions.md).
static void retry_backoff(int attempt) {
    if (isr_in_progress()) {
        for (int i = 0; i < ATA_POLL_LIMIT * attempt; i++) io_wait();
        return;
    }
    uint64_t start = pit_ticks();
    uint64_t want = (uint64_t)attempt * RETRY_BACKOFF_TICKS;
    while (pit_ticks() - start < want) __asm__ volatile ("hlt");
}

static int dma_transfer_with_retry(uint32_t lba, int count, void *buf, int is_write) {
    for (int attempt = 1; attempt <= ATA_DMA_MAX_RETRIES; attempt++) {
        if (dma_transfer(lba, count, buf, is_write, attempt)) {
            if (attempt > 1 && dbgflag_enabled(DBGFLAG_ATA)) {
                klog_write("ata: dma "); klog_write(is_write ? "write" : "read");
                klog_write(" ok on attempt "); klog_write_dec((uint32_t)attempt);
                klog_write(" (lba "); klog_write_dec(lba); klog_write(")\n");
            }
            return 1;
        }
        if (dbgflag_enabled(DBGFLAG_ATA)) {
            klog_write("ata: dma "); klog_write(is_write ? "write" : "read");
            klog_write(" attempt "); klog_write_dec((uint32_t)attempt);
            klog_write(" failed (lba "); klog_write_dec(lba); klog_write(")\n");
        }
        if (attempt < ATA_DMA_MAX_RETRIES) retry_backoff(attempt);
    }
    // Always logged, not gated behind DBGFLAG_ATA -- exhausting every
    // retry means this is a real failure the caller (fs.c, ultimately
    // whoever called fs_write()/fs_read()) is about to report as one
    // too, and dmesg should have the disk-level detail on hand even
    // with debug logging off.
    klog_write("ata: dma "); klog_write(is_write ? "write" : "read");
    klog_write(" failed after "); klog_write_dec((uint32_t)ATA_DMA_MAX_RETRIES);
    klog_write(" attempts (lba "); klog_write_dec(lba);
    klog_write(", last reason: "); klog_write(g_dma_fail_reason);
    klog_write(")\n");
    return 0;
}

// ---------------------------------------------------------------------
// PIO fallback -- exactly the driver this file was before build 430,
// used automatically whenever ata_init_dma() couldn't stand up the DMA
// path (see its comment for every reason that can happen).
// ---------------------------------------------------------------------

// `count` sectors in one command (CMD_READ_SECTORS/CMD_WRITE_SECTORS
// both accept a sector count > 1, same opcodes as the single-sector
// case -- only REG_SECCOUNT and the transfer loop below change). The
// drive raises DRQ once per sector, not once for the whole command, so
// the inner transfer loop still runs `count` times -- the saving vs.
// `count` separate ata_read_sector() calls is the command dispatch/
// wait_not_busy() overhead for sectors 2..count, not the wire transfer
// time itself (that's inherent to the hardware either way).
// Always logged, not gated behind DBGFLAG_ATA -- same reasoning as
// dma_transfer_with_retry()'s final message: a PIO transfer that failed
// is a failure the caller is about to report as one too, and dmesg
// should carry the disk-level detail even with debug logging off.
static int pio_fail(uint32_t lba, int is_write) {
    klog_write("ata: pio "); klog_write(is_write ? "write" : "read");
    klog_write(" failed (lba "); klog_write_dec(lba);
    klog_write(", reason: "); klog_write(g_pio_fail_reason);
    klog_write(")\n");
    return 0;
}

static int pio_read_sectors(uint32_t lba, int count, void *buf) {
    if (!wait_not_busy()) {
        g_pio_fail_reason = "drive stayed busy, command never issued";
        return pio_fail(lba, 0);
    }

    select_lba(lba, (uint8_t)count);
    outb(REG_COMMAND, CMD_READ_SECTORS);

    uint16_t *p = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (!wait_drq()) return pio_fail(lba, 0);
        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) *p++ = inw(REG_DATA);
    }
    return 1;
}

static int pio_write_sectors(uint32_t lba, int count, const void *buf) {
    if (!wait_not_busy()) {
        g_pio_fail_reason = "drive stayed busy, command never issued";
        return pio_fail(lba, 1);
    }

    select_lba(lba, (uint8_t)count);
    outb(REG_COMMAND, CMD_WRITE_SECTORS);

    const uint16_t *p = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (!wait_drq()) return pio_fail(lba, 1);
        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) outw(REG_DATA, *p++);
    }

    // Best-effort: ask the drive to flush its write cache so the data
    // is actually durable, not just handed off. Failure here doesn't
    // undo the write above (PIO WRITE SECTORS has already transferred
    // the data by this point) -- it just means we can't be as sure it
    // survives a real power loss, which for QEMU's emulated disk backed
    // by a host file is already close to moot. Suppressed while a
    // caller has an ata_flush_begin()/end() batch open (ata.h).
    maybe_flush();

    return 1;
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

// Defined after the raw ops it registers; see ata_cache_start() below.
static void ata_cache_start(void);

void ata_init(void) {
    g_present = 0;
    g_dma_available = 0;
    g_sector_count = 0;

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

    // Every one of the 256 words must be read regardless of how many
    // are actually inspected -- leaving any unread would desync the
    // controller for the next command. Words 60-61 are the 28-bit LBA
    // sector count (low word first); everything else is still discarded.
    uint16_t identify[256];
    for (int i = 0; i < 256; i++) identify[i] = inw(REG_DATA);
    g_sector_count = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    // Word 169 bit 0: the drive supports DATA SET MANAGEMENT's TRIM bit.
    // Asked rather than assumed -- issuing an unsupported command gets
    // an ABRT and, on some real controllers, a wedged channel.
    g_trim_supported = (identify[169] & 0x0001) != 0;

    g_present = 1;

    // Only worth attempting once a drive is confirmed present -- see
    // ata_init_dma()'s own comment.
    ata_init_dma();

    // The write-back cache goes on last, once the transfer path it will
    // drive is settled. It is handed the RAW ops rather than the public
    // entry points, so a write-back cannot recurse back into the cache.
    ata_cache_start();
}

int ata_present(void) {
    return g_present;
}

// "Would a TRIM issued right now actually go out?" -- not merely "does
// the drive advertise it". Both callers want the former: ata_trim()
// bails on it, and the `ata` command reports it, and answering the
// IDENTIFY question alone made that report a lie on any machine whose
// Bus-Master DMA never came up (dsm_send_block() has no PIO fallback to
// fall back TO, so every TRIM there fails silently while `ata` said
// "supported").
//
// g_dma_available is the right gate rather than dma_in_use(): it is set
// only on ata_init_dma()'s full-success path, so it implies g_prd and
// g_dma_buf both exist. Deliberately NOT dma_in_use() -- `ata nodma`
// forces DATA transfers down the PIO path, and TRIM keeps using DMA
// regardless because there is no PIO form of DSM that works (see
// dsm_send_block()). Verified by measurement, not assumed: with PIO
// forced, `stress 30` still leaves the image at its pre-run size.
int ata_trim_supported(void) {
    return g_present && g_trim_supported && g_dma_available;
}

uint32_t ata_sector_count(void) {
    return g_present ? g_sector_count : 0;
}

// Shared by ata_read_sectors()/ata_write_sectors(). A transfer that
// runs past the end of the drive is a caller bug (or a filesystem
// configured for a bigger disk than it actually has), and catching it
// here turns "the drive quietly errors, or worse, wraps" into a clean
// failure with a log line naming the LBA. Skipped entirely when the
// capacity is unknown (g_sector_count == 0), so a drive whose IDENTIFY
// didn't report one behaves exactly as it did before this existed.
static int lba_range_ok(uint32_t lba, int count) {
    if (g_sector_count == 0) return 1;
    if (lba > g_sector_count || (uint32_t)count > g_sector_count - lba) {
        klog_write("ata: refusing transfer past end of drive (lba ");
        klog_write_dec(lba); klog_write(", count "); klog_write_dec((uint32_t)count);
        klog_write(", drive has "); klog_write_dec(g_sector_count);
        klog_write(" sectors)\n");
        return 0;
    }
    return 1;
}

// Definition sits here, after g_pending below is in scope for
// ata_set_dma_forced_off().
static int dma_in_use(void) { return g_dma_available && !g_dma_forced_off; }

int ata_dma_active(void) {
    return dma_in_use();
}

int ata_dma_hardware_available(void) {
    return g_dma_available;
}

// Refuses while a non-blocking transfer is in flight: dma_transfer_
// poll() would otherwise be left waiting on an engine the dispatch
// sites have stopped considering active, and the caller (a stepped
// Notepad save) has no way to hear about that. Returns 1 if the mode
// was applied, 0 if it was refused -- callers report the difference.
int ata_set_dma_forced_off(int off) {
    if (g_pending.in_flight) return 0;
    g_dma_forced_off = off ? 1 : 0;
    return 1;
}

// The real per-transfer sector cap for THIS boot. ATA_MAX_SECTORS_PER_
// XFER (ata.h) is the compile-time maximum callers size their buffers
// against; this is what the DMA bounce buffer actually turned out to
// be, which is smaller if the big contiguous allocation failed, and is
// the PIO path's own limit when DMA never came up at all. Callers that
// batch work into transfers (tfs.c's block coalescing) ask this rather
// than assuming the maximum.
int ata_max_sectors_per_xfer(void) {
    if (!dma_in_use()) return ATA_PIO_MAX_SECTORS_PER_XFER;
    uint32_t sectors = g_dma_buf_frames * (4096 / ATA_SECTOR_SIZE);
    return sectors > ATA_MAX_SECTORS_PER_XFER ? ATA_MAX_SECTORS_PER_XFER : (int)sectors;
}

int ata_read_sector(uint32_t lba, void *buf) {
    return ata_read_sectors(lba, 1, buf);
}

int ata_write_sector(uint32_t lba, const void *buf) {
    return ata_write_sectors(lba, 1, buf);
}

// The raw transfer, with no cache in the path. ata_read_sectors()
// below is the public entry point and consults the cache; this is what
// the cache itself calls back into (see ata_cache.h's struct atac_ops).
static int ata_read_sectors_raw(uint32_t lba, int count, void *buf) {
    if (!g_present) return 0;
    if (count < 1 || count > ata_max_sectors_per_xfer()) return 0;
    if (!lba_range_ok(lba, count)) return 0;
    if (dma_in_use()) return dma_transfer_with_retry(lba, count, buf, 0);
    return pio_read_sectors(lba, count, buf);
}

static int ata_write_sectors_raw(uint32_t lba, int count, const void *buf) {
    if (!g_present) return 0;
    if (count < 1 || count > ata_max_sectors_per_xfer()) return 0;
    if (!lba_range_ok(lba, count)) return 0;
    if (dma_in_use()) return dma_transfer_with_retry(lba, count, (void *)(uintptr_t)buf, 1);
    return pio_write_sectors(lba, count, buf);
}

// ---- the public entry points, and the cache under them ---------------
//
// FAULT INJECTION STAYS HERE, ABOVE THE CACHE, and that placement is
// the whole point of it. Armed from a test (fault_inject.h), it has to
// make a call fail exactly as a refusing drive would -- and if it sat
// in the raw path instead, a read served from cache would quietly
// succeed and a write would fail only later at a flush, so the error
// paths the KTESTs exist to exercise would stop being reached. The
// cache is an optimisation; whether an injected failure is observed
// must not depend on it.
// A WRITE-BACK is where a deferred write finally meets the drive, so it
// is the second place an injected write failure has to be observable --
// with a write-back cache in the path, "the drive refused this write" no
// longer necessarily happens during the caller's write() at all.
// Without this, a test can arm a failure, dirty a line and flush, and
// the flush reports success because nothing on the way to the platter
// ever asked. (It is not double-counting against the public entry
// point: a caller's write that was refused there never became a dirty
// line, so it has no write-back to consume a second failure.)
static int ata_write_back(uint32_t lba, int count, const void *buf) {
    if (fault_should_fail_ata_write()) return 0;
    return ata_write_sectors_raw(lba, count, buf);
}

static const struct atac_ops ATA_CACHE_OPS = {
    .read = ata_read_sectors_raw,
    .write = ata_write_back,
    .flush = ata_flush_raw,
};

static void ata_cache_start(void) { atac_init(&ATA_CACHE_OPS); }

int ata_cache_active(void) { return atac_enabled(); }

uint32_t ata_cache_dirty(void) {
    struct atac_stats st;
    atac_get_stats(&st);
    return st.dirty;
}

int ata_sync(uint32_t *out_written, uint32_t *out_pending) {
    struct atac_stats before, after;
    atac_get_stats(&before);
    int ok = ata_flush_now();
    atac_get_stats(&after);
    if (out_written) *out_written = after.writebacks - before.writebacks;
    if (out_pending) *out_pending = after.dirty;
    return ok;
}

int ata_read_sectors(uint32_t lba, int count, void *buf) {
    if (fault_should_fail_ata_read()) return 0;
    if (!g_present) return 0;
    if (count < 1 || count > ata_max_sectors_per_xfer()) return 0;
    if (!lba_range_ok(lba, count)) return 0;
    if (atac_enabled()) return atac_read(lba, count, buf);
    return ata_read_sectors_raw(lba, count, buf);
}

int ata_write_sectors(uint32_t lba, int count, const void *buf) {
    if (fault_should_fail_ata_write()) return 0;
    if (!g_present) return 0;
    if (count < 1 || count > ata_max_sectors_per_xfer()) return 0;
    if (!lba_range_ok(lba, count)) return 0;
    if (atac_enabled()) return atac_write(lba, count, buf);
    return ata_write_sectors_raw(lba, count, buf);
}

// Diagnostic only (the shell's `dmatest`, apps/shell_sys.c) -- proves
// dma_transfer_start()/dma_transfer_poll() actually work, read-only so
// it can never touch real filesystem data: reads `lba` once through the
// existing, already-trusted blocking path (ata_read_sector()) and once
// through the new non-blocking start/poll pair, byte-compares the two,
// and reports how many polls the non-blocking read needed. `*out_polls`
// is always written (0 if this returns early). Requires the DMA path
// to be active -- there's nothing to prove on a PIO-only machine, this
// primitive doesn't exist there (see ata.h's top comment).
int ata_dma_nonblocking_selftest(uint32_t lba, uint32_t *out_polls) {
    *out_polls = 0;
    if (!g_present || !dma_in_use()) return 0;

    uint8_t via_blocking[ATA_SECTOR_SIZE];
    if (!ata_read_sector(lba, via_blocking)) return 0;

    uint8_t via_poll[ATA_SECTOR_SIZE];
    if (!dma_transfer_start(lba, 1, via_poll, 0)) return 0;

    uint32_t polls = 0;
    enum ata_poll_result r;
    // ATA_POLL_LIMIT is this driver's existing "don't loop forever"
    // bound for syscall-context busy-polling (wait_dma_irq() above uses
    // the same constant) -- reused here as this test's own outer bound
    // rather than inventing a new one, since it's already sized to be
    // generous for a single sector.
    while ((r = dma_transfer_poll()) == ATA_POLL_PENDING) {
        polls++;
        if (polls > ATA_POLL_LIMIT) return 0; // driver bug, not a real timeout -- poll() itself already times out via DMA_WAIT_TICKS
    }
    *out_polls = polls;
    if (r != ATA_POLL_DONE) return 0;

    if (k_memcmp(via_blocking, via_poll, ATA_SECTOR_SIZE) != 0) return 0;
    return 1;
}

// ---------------------------------------------------------------------
// TRIM (DATA SET MANAGEMENT)
// ---------------------------------------------------------------------
//
// Tells the drive that an LBA range no longer holds data worth keeping.
// On real SSDs that's what keeps write amplification down; here the
// drive is QEMU backed by a host file, and with `discard=unmap` on the
// -drive line QEMU turns it into a hole punch -- so a toy-os `rm` gives
// the space back to the host filesystem instead of the image growing
// forever.
//
// **Why this matters here.** The image is created sparse (`truncate -s
// 9G`) and costs nothing at first, but sparseness is only ever LOST: a
// block written once stays allocated on the host even after the file
// that owned it is deleted. Measured before this existed: 8.1 GiB
// actually allocated against 2.3 MiB the filesystem considered in use.
// tools/tfs2_writer.py's `trim` is the host-side reclaim for images in
// that state; this is the half that stops them getting there.
//
// **It must go out over DMA, not PIO.** DATA SET MANAGEMENT looks like
// an ordinary PIO data-out command in the spec, and the first version
// here sent it that way: the drive accepted the command, returned no
// error, and nothing whatsoever was discarded. QEMU implements DSM as a
// DMA command (hw/ide/core.c dispatches it through
// ide_sector_start_dma() with IDE_DMA_TRIM), so the range list has to
// arrive by bus-master transfer. Over PIO it never arrives at all, and
// the "success" is the drive acknowledging a command whose payload it
// is still waiting for. That failure is completely silent from the
// guest side -- worth knowing before trusting any DSM return value.
//
// The payload is a 512-byte block of 8-byte range entries: a 48-bit
// starting LBA in the low 6 bytes, then a 16-bit sector count. A zero
// count terminates the list, which is why the buffer is zeroed first.
#define DSM_ENTRIES_PER_BLOCK (ATA_SECTOR_SIZE / 8)
#define DSM_MAX_RANGE 0xFFFF // a single entry's 16-bit sector count

// Issues one already-built descriptor block. Mirrors dma_issue()/
// dma_finish() rather than calling them, because those hardcode
// READ/WRITE DMA and their own LBA/count semantics -- DSM's Sector
// Count register means "how many 512-byte DESCRIPTOR blocks follow",
// and its LBA registers are unused.
static int dsm_send_block(const uint8_t *block) {
    // Belt and braces -- ata_trim_supported() already gated on
    // g_dma_available, which implies both of these. There is no PIO
    // fallback to reach for: DSM only works as a DMA command.
    if (!g_dma_buf || !g_prd) return 0;

    for (uint32_t i = 0; i < ATA_SECTOR_SIZE; i++) g_dma_buf[i] = block[i];

    g_prd->base = (uint32_t)g_dma_buf_phys;
    g_prd->count = (uint16_t)ATA_SECTOR_SIZE;
    g_prd->flags = PRD_EOT;

    outl(g_bm_io + BM_PRDT, (uint32_t)g_prd_phys);
    outb(g_bm_io + BM_STATUS, BM_STATUS_ERROR | BM_STATUS_IRQ); // W1C stale bits
    outb(g_bm_io + BM_CMD, 0); // direction: memory -> device, same as a write

    if (!wait_not_busy()) return 0;
    outb(REG_FEATURES, DSM_FEATURE_TRIM);
    outb(REG_SECCOUNT, 1); // one 512-byte descriptor block
    outb(REG_LBA_LOW, 0);  // unused by DSM, and required to be zero
    outb(REG_LBA_MID, 0);
    outb(REG_LBA_HIGH, 0);
    outb(REG_DRIVE_HEAD, 0xE0); // LBA mode, master
    outb(REG_COMMAND, CMD_DATA_SET_MGMT);

    g_dma_irq_fired = 0;
    outb(g_bm_io + BM_CMD, BM_CMD_START);

    int ok = wait_dma_irq(DMA_WAIT_TICKS); // no retry loop here -- full bound
    outb(g_bm_io + BM_CMD, 0);
    uint8_t bm_status = inb(g_bm_io + BM_STATUS);
    outb(g_bm_io + BM_STATUS, BM_STATUS_ERROR | BM_STATUS_IRQ);
    uint8_t st = inb(REG_STATUS); // also acknowledges the drive's IRQ line

    if (!ok || (bm_status & BM_STATUS_ERROR) || (st & STATUS_ERR)) return 0;
    return 1;
}

int ata_trim(uint32_t lba, uint32_t count) {
    if (!ata_trim_supported() || count == 0) return 0;
    if (!lba_range_ok(lba, (int)count)) return 0;

    uint8_t block[ATA_SECTOR_SIZE];
    while (count > 0) {
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) block[i] = 0;

        int n = 0;
        while (count > 0 && n < DSM_ENTRIES_PER_BLOCK) {
            uint32_t chunk = count > DSM_MAX_RANGE ? DSM_MAX_RANGE : count;
            uint8_t *e = &block[n * 8];
            e[0] = (uint8_t)(lba & 0xFF);
            e[1] = (uint8_t)((lba >> 8) & 0xFF);
            e[2] = (uint8_t)((lba >> 16) & 0xFF);
            e[3] = (uint8_t)((lba >> 24) & 0xFF);
            e[4] = 0; // this driver is 28-bit LBA throughout; the top
            e[5] = 0; // 16 bits of the 48-bit field are always zero
            e[6] = (uint8_t)(chunk & 0xFF);
            e[7] = (uint8_t)((chunk >> 8) & 0xFF);
            lba += chunk;
            count -= chunk;
            n++;
        }

        if (!dsm_send_block(block)) return 0;
    }
    return 1;
}
