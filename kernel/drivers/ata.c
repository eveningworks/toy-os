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

// Total addressable sectors, from IDENTIFY words 60-61 (the 28-bit LBA
// capacity field) -- 0 if unknown, which is what every caller treats as
// "no bound available, don't range-check". Every one of IDENTIFY's 256
// words has always been read and discarded here; keeping two of them
// costs nothing and gives the filesystem a real answer to "how big is
// this disk" instead of tfs.c's hardcoded FS_DISK_TOTAL_BYTES guess
// (which silently allocates past the end of a smaller image -- see
// tfs.c's own clamp).
static uint32_t g_sector_count = 0;

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
#define DMA_WAIT_TICKS 500

static int g_dma_available = 0;
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
void ata_flush_now(void) {
    if (!g_present) return;
    if (wait_not_busy()) {
        outb(REG_COMMAND, CMD_CACHE_FLUSH);
        wait_not_busy();
    }
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
    select_lba(lba, (uint8_t)count);
    outb(REG_COMMAND, is_write ? CMD_WRITE_DMA : CMD_READ_DMA);

    g_dma_irq_fired = 0;
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

static int dma_transfer(uint32_t lba, int count, void *buf, int is_write) {
    if (!dma_issue(lba, count, buf, is_write)) {
        g_dma_fail_reason = "drive stayed busy, command never issued";
        return 0;
    }
    int ok = wait_dma_irq();
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

static int dma_transfer_with_retry(uint32_t lba, int count, void *buf, int is_write) {
    for (int attempt = 1; attempt <= ATA_DMA_MAX_RETRIES; attempt++) {
        if (dma_transfer(lba, count, buf, is_write)) {
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
static int pio_read_sectors(uint32_t lba, int count, void *buf) {
    if (!wait_not_busy()) return 0;

    select_lba(lba, (uint8_t)count);
    outb(REG_COMMAND, CMD_READ_SECTORS);

    uint16_t *p = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (!wait_drq()) return 0;
        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) *p++ = inw(REG_DATA);
    }
    return 1;
}

static int pio_write_sectors(uint32_t lba, int count, const void *buf) {
    if (!wait_not_busy()) return 0;

    select_lba(lba, (uint8_t)count);
    outb(REG_COMMAND, CMD_WRITE_SECTORS);

    const uint16_t *p = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (!wait_drq()) return 0;
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

    g_present = 1;

    // Only worth attempting once a drive is confirmed present -- see
    // ata_init_dma()'s own comment.
    ata_init_dma();
}

int ata_present(void) {
    return g_present;
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

int ata_dma_active(void) {
    return g_dma_available;
}

// The real per-transfer sector cap for THIS boot. ATA_MAX_SECTORS_PER_
// XFER (ata.h) is the compile-time maximum callers size their buffers
// against; this is what the DMA bounce buffer actually turned out to
// be, which is smaller if the big contiguous allocation failed, and is
// the PIO path's own limit when DMA never came up at all. Callers that
// batch work into transfers (tfs.c's block coalescing) ask this rather
// than assuming the maximum.
int ata_max_sectors_per_xfer(void) {
    if (!g_dma_available) return ATA_PIO_MAX_SECTORS_PER_XFER;
    uint32_t sectors = g_dma_buf_frames * (4096 / ATA_SECTOR_SIZE);
    return sectors > ATA_MAX_SECTORS_PER_XFER ? ATA_MAX_SECTORS_PER_XFER : (int)sectors;
}

int ata_read_sector(uint32_t lba, void *buf) {
    return ata_read_sectors(lba, 1, buf);
}

int ata_write_sector(uint32_t lba, const void *buf) {
    return ata_write_sectors(lba, 1, buf);
}

int ata_read_sectors(uint32_t lba, int count, void *buf) {
    // Deliberate failure injection for tests (fault_inject.h) -- inert
    // unless a test armed it. Placed at the public entry point, before
    // any hardware is touched, so an injected failure looks exactly
    // like the drive refusing: same return value, no side effects.
    if (fault_should_fail_ata_read()) return 0;
    if (!g_present) return 0;
    if (count < 1 || count > ata_max_sectors_per_xfer()) return 0;
    if (!lba_range_ok(lba, count)) return 0;
    if (g_dma_available) return dma_transfer_with_retry(lba, count, buf, 0);
    return pio_read_sectors(lba, count, buf);
}

int ata_write_sectors(uint32_t lba, int count, const void *buf) {
    // Deliberate failure injection for tests (fault_inject.h) -- inert
    // unless a test armed it. Placed at the public entry point, before
    // any hardware is touched, so an injected failure looks exactly
    // like the drive refusing: same return value, no side effects.
    if (fault_should_fail_ata_write()) return 0;
    if (!g_present) return 0;
    if (count < 1 || count > ata_max_sectors_per_xfer()) return 0;
    if (!lba_range_ok(lba, count)) return 0;
    if (g_dma_available) return dma_transfer_with_retry(lba, count, (void *)(uintptr_t)buf, 1);
    return pio_write_sectors(lba, count, buf);
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
    if (!g_present || !g_dma_available) return 0;

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
