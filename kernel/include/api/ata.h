#ifndef ATA_H
#define ATA_H

#include <stdint.h>

// ATA/IDE, primary bus, master drive only, 28-bit LBA addressing. Two
// transfer paths exist now (as of build 430), chosen automatically at
// init, with the exact same public API either way:
//
//   - Bus-Master IDE DMA, IRQ14-driven completion -- the normal path.
//     ata_init() looks for the IDE controller on the PCI bus (pci.c,
//     build 390): if it's there, its Bus-Master DMA registers (found
//     via BAR4) are usable, and a 2-frame contiguous physical
//     allocation for the PRDT + a bounce buffer succeeds (pmm.c, build
//     410), this driver sets up a Physical Region Descriptor Table,
//     issues READ DMA/WRITE DMA (0xC8/0xCA) instead of the old PIO
//     commands, and blocks on IRQ14 (irq.c, build 400) instead of
//     spinning on a status register.
//   - The original PIO, busy-polled path -- used automatically if
//     anything about DMA setup fails (no IDE controller found at all,
//     its BAR4 isn't an I/O-space BAR, or the contiguous-frame
//     allocation for the PRDT/bounce buffer comes back empty). No
//     capability this driver already had is lost by falling back --
//     see ata.c's top comment for the full detection sequence, which
//     is unchanged from before this build.
//
// ata_present()/ata_read_sector()/ata_write_sector() -- and every
// existing caller (the VFS, the filesystem) -- neither know nor care which path is
// active; this is purely an ata.c-internal choice.
//
// THIS IS THE LEGACY PATH NOW, not the only one. AHCI exists
// (kernel/drivers/ahci.c) and sits above this in the block layer's
// precedence, because real modern hardware increasingly lacks a legacy
// IDE controller at all; `noahci` on the boot line steps back down to
// here, which is what keeps this driver reachable and therefore tested.
// It is still exactly what QEMU's default `-drive ...,if=ide` presents,
// and still the whole disk on a machine with neither a virtio device
// nor an HBA.
#define ATA_SECTOR_SIZE 512

// Probes the primary bus for a master drive via IDENTIFY DEVICE and
// records whether one was found; if one is, also attempts to set up
// the Bus-Master DMA path described above (silently falling back to
// PIO on any failure -- see ata.c's ata_init_dma()). Safe to call even
// if there's no controller/drive at all (a floating bus reads back
// 0xFF and this returns immediately) -- every other ata_* function is
// a no-op after that, not a hang.
void ata_init(void);

// 1 if ata_init() found a usable drive, 0 otherwise (no controller, no
// drive, or something that identified as non-ATA, e.g. an ATAPI
// optical drive on the same bus -- the boot CD is on the SECONDARY bus
// in every QEMU invocation this project uses, see the Makefile, so it
// never conflicts with this).
int ata_present(void);

// Would a TRIM issued right now actually go out? That means all three
// of: a drive is present, it advertises DATA SET MANAGEMENT with the
// TRIM bit (IDENTIFY word 169 bit 0), and Bus-Master DMA came up --
// because DSM only works as a DMA command and there is no PIO fallback
// (see ata.c's dsm_send_block()).
//
// The IDENTIFY bit is asked rather than assumed because issuing an
// unsupported command earns an ABRT and, on some real controllers, a
// wedged channel. The DMA half is included because leaving it out made
// this a lie on any machine without Bus-Master DMA: every TRIM failed
// silently while `ata` reported "supported".
//
// NOT affected by `ata nodma`, which forces DATA transfers down the PIO
// path. TRIM keeps going out over DMA there, and still works -- measured,
// not assumed.
int ata_trim_supported(void);

// Tell the drive that `count` sectors from `lba` no longer hold data
// worth keeping. Returns 1 on success, 0 if unsupported, out of range,
// or the drive reported an error -- and a 0 is never fatal to the
// caller: TRIM is an optimisation, and a filesystem that couldn't issue
// one has still freed the block perfectly well.
//
// Why a hobby OS wants this: with `discard=unmap` on QEMU's -drive line
// this becomes a hole punch in the backing file, so deleting a file
// inside toy-os gives the space back to the host instead of the image
// growing forever. Without it, an image is sparse only until something
// writes to it once. See ata.c's own comment for the measured numbers.
int ata_trim(uint32_t lba, uint32_t count);
// Every run, packed 64 to a DSM command (kernel/block.h's list type).
struct blk_range;
int ata_trim_ranges(const struct blk_range *r, int n);

// 1 if the Bus-Master DMA path is active (see this header's top
// comment), 0 if every transfer is going through the PIO fallback --
// diagnostic only, no caller needs to branch on this (both paths
// implement the exact same ata_read_sector()/ata_write_sector()
// contract below).
int ata_dma_active(void);

// 1 if this machine's hardware HAS working Bus-Master DMA, regardless of
// whether it's currently in use. Differs from ata_dma_active() above
// only while DMA has been forced off -- which is exactly what a caller
// wanting to restore the previous mode (or to skip a PIO-vs-DMA
// comparison on a machine that has no DMA to compare against) needs to
// know.
int ata_dma_hardware_available(void);

// 1 if DMA has been FORCED off by ata_set_dma_forced_off() -- what was
// asked for, as opposed to what is in effect.
//
// The distinction is the whole reason this exists. ata_dma_active()
// answers "are transfers going through DMA right now?", which is
// `hardware_available && !forced_off` -- so on a machine with no DMA
// engine it is 0 whatever anyone asked for. A CONTROL that sets the
// forcing has to report the forcing back, or it shows a state its own
// setter cannot produce and snaps back to it (kernel.ata_nodma did
// exactly that, and the Settings radio was unusable on a virtio boot).
//
// Rule of thumb for any setting: get() must return what apply() last
// accepted. Report the effective state somewhere it cannot be mistaken
// for the knob -- `/bin/ata` and the setting's own unavailable() reason.
int ata_dma_forced_off(void);

// Forces every transfer down the PIO fallback (`off` non-zero), or
// allows DMA again (0). The `ata nodma` shell command and
// kernel/drivers/ata_test.c's PIO round-trip are the callers.
//
// Exists because the PIO path is otherwise unreachable on any machine
// where DMA works, i.e. all of them -- see ata.c's g_dma_forced_off
// comment for both reasons that matters.
//
// Returns 1 if applied, 0 if refused because a non-blocking transfer is
// currently in flight (dma_transfer_start()/poll()) -- switching modes
// underneath one would strand its poller. Callers must report a 0
// rather than assume the mode changed.
int ata_set_dma_forced_off(int off);

// Total addressable 512-byte sectors on the attached drive, from
// IDENTIFY's 28-bit LBA capacity field (words 60-61), or 0 if no drive
// is present or it didn't report one. Multiply by ATA_SECTOR_SIZE for
// bytes; note the 28-bit LBA ceiling means this can never exceed 2^28
// sectors (128 GiB) regardless of the real drive's size.
//
// Added so the filesystem can size itself to the disk it actually has
// instead of a compile-time guess (see TFS2's FS_DISK_TOTAL_BYTES and
// its runtime clamp). ata_read_sectors()/ata_write_sectors() also
// range-check against this themselves, so a transfer past the end of
// the drive fails cleanly and loudly rather than being handed to the
// hardware.
uint32_t ata_sector_count(void);

// Reads/writes exactly one ATA_SECTOR_SIZE-byte sector at 28-bit LBA
// `lba`. Returns 1 on success, 0 on failure (no drive present, the
// drive reported an error, or -- DMA path only -- the completion IRQ
// never arrived within a bounded wait; the PIO path's own bounded
// polling retry covers the equivalent case there).
int ata_read_sector(uint32_t lba, void *buf);
int ata_write_sector(uint32_t lba, const void *buf);

// Multi-sector transfer, `count` consecutive sectors starting at `lba`
// in one command instead of `count` separate ones -- same
// success/failure contract as the single-sector calls above. `count`
// must be in [1, ata_max_sectors_per_xfer()] -- note the RUNTIME limit,
// not the compile-time one below.
//
// ATA_MAX_SECTORS_PER_XFER is the compile-time ceiling: 128 sectors =
// 65536 bytes, which is exactly what one Physical Region Descriptor can
// describe (its byte count field is 16-bit, 0 meaning 64KB), so it's
// the most a single-PRD driver like this one can move per command
// without adding scatter-gather. Size any buffer you intend to fill in
// one call against this.
//
// This was 8 (4096 bytes, one TFS2 block) until the DMA bounce buffer
// grew from 1 frame to 16 -- and that 4KB buffer, not the drive or the
// controller, was the filesystem's actual throughput ceiling: one ATA
// command plus one completion IRQ per 4KB of file data no matter how
// sequential the write was. The drive's own 8-bit REG_SECCOUNT was
// never the constraint (it reaches 256 in 28-bit mode).
//
// ata_max_sectors_per_xfer() reports what's usable THIS boot, which is
// smaller when the 64KB contiguous allocation failed and the driver
// fell back to a 4KB buffer, or when DMA never came up at all and every
// transfer is going through the PIO path. Callers that batch work into
// transfers should ask it rather than assuming the maximum; callers
// that just want one block (the common case) can keep passing a fixed
// small count and never think about either number.
#define ATA_MAX_SECTORS_PER_XFER 128 // 128 * 512B = 65536B, one full PRD entry
#define ATA_PIO_MAX_SECTORS_PER_XFER 8 // PIO fallback stays at TFS2's block size, unchanged
int ata_max_sectors_per_xfer(void);
int ata_read_sectors(uint32_t lba, int count, void *buf);
int ata_write_sectors(uint32_t lba, int count, const void *buf);

// Every ata_write_sector(s) call issues its own CMD_CACHE_FLUSH
// afterward, by default -- fine (even desirable) for a single
// standalone write, but ruinous for a loop of many (e.g. one
// filesystem block per 4KB of a large file write): flush is
// inherently synchronous, so N writes means N full round trips to the
// drive, dominating throughput regardless of how fast the underlying
// DMA transfer itself is. Measured live: `stress 100` (a 100MB write/
// read/verify pass, apps/shell_sys.c) ran at ~1.4MB/s before this
// existed -- see docs/decisions.md.
//
// ata_flush_begin()/ata_flush_end() bracket a batch of writes that
// only need to be durable as a whole, not after each individual one:
// every write still goes to the drive immediately (this only defers
// the FLUSH command, not the write -- a read-back during the batch
// still sees correct data), and ata_flush_end() issues exactly one
// real CMD_CACHE_FLUSH once the OUTERMOST matching call returns,
// covering everything written since the matching ata_flush_begin().
// Nestable (a depth counter, not a boolean) so a batched caller can
// safely be invoked from inside another batch without triggering an
// early flush. Every begin() MUST be matched by an end() on every
// exit path (including error returns) -- an unmatched begin() leaves
// every future write silently unflushed until the next begin/end
// pair happens to close it out.
//
// No caller HAS to use this -- ata_write_sector(s) with no
// begin()/end() around it behaves exactly as before (flush every
// write), which is still what every crash-safety-sensitive path
// (TFS2's journal/table-slot writes in persist_record()) uses
// deliberately, unchanged. This is opt-in for genuinely bulk,
// re-derivable-on-failure data (TFS2's write_range_impl() data-block
// loop) where losing a bit of durability window in exchange for real
// throughput is the right tradeoff -- see docs/decisions.md for the
// full reasoning on where the line is drawn.
void ata_flush_begin(void);
void ata_flush_end(void);

// An explicit flush barrier, independent of the deferral depth above,
// and a way to close a deferral region without one. Both exist for
// write-ahead-journal-shaped callers, where a specific write must be
// durable before the next is issued and the begin/end pair can't say
// that -- see ata.c's own comments on each, and TFS2's
// persist_record() for the real caller. Don't reach for either just to
// "flush a bit less"; ata_flush_begin()/end() is the tool for that.
int ata_flush_now(void);

// ---- the write-back cache, as much of it as apps may see -------------
//
// The cache itself is kernel-internal (kernel/ata_cache.h, not on
// apps/'s include path -- see kernel/include/README.md). What an app
// legitimately needs is only "is anything being held back?" and "write
// it out and tell me what happened", which is what `sync` is, so that
// is what crosses the boundary. struct atac_ops and the line table stay
// where they belong.

// 1 if a write-back cache is active, so a caller can say "writes go
// straight to the disk" rather than reporting a sync of nothing.
int ata_cache_active(void);

// Flushes the cache and the drive. Returns 1 only if everything is
// durable. `out_written` gets the number of sectors actually written
// back, `out_pending` the number still dirty afterwards -- non-zero
// only on failure, and then it is the count of sectors that exist in
// RAM ONLY. Either pointer may be NULL.
int ata_sync(uint32_t *out_written, uint32_t *out_pending);

// How many sectors are dirty right now -- i.e. written by somebody,
// acknowledged, and still only in RAM. 0 when there is no cache.
// Exists so a test can establish that there is something to lose before
// asserting about losing it; a check that silently has nothing to
// measure passes for the wrong reason.
uint32_t ata_cache_dirty(void);
void ata_flush_end_no_flush(void);

// Phase 1 of the async-I/O roadmap item (docs/roadmap.md): a
// non-blocking start/poll pair for the DMA path, built alongside the
// existing blocking ata_read_sectors()/ata_write_sectors() rather than
// replacing them -- no filesystem uses this yet. Only usable
// when ata_dma_active() is true (the PIO fallback has no equivalent;
// polling a busy-wait loop non-blockingly isn't meaningfully
// different from just blocking on it).
//
// One transfer at a time: dma_transfer_start() returns 0 immediately
// if a previous one hasn't been polled to completion yet (ATA_POLL_DONE
// or ATA_POLL_FAILED), or if the command couldn't even be issued (see
// ata.c's dma_issue()). Once started, call dma_transfer_poll()
// repeatedly -- from a loop, a per-frame callback, wherever -- until it
// stops returning ATA_POLL_PENDING. `buf` must stay valid and
// unmodified by the caller until polling reaches a terminal result.
enum ata_poll_result { ATA_POLL_PENDING = 0, ATA_POLL_DONE = 1, ATA_POLL_FAILED = 2 };
int dma_transfer_start(uint32_t lba, int count, void *buf, int is_write);
enum ata_poll_result dma_transfer_poll(void);

// Diagnostic only -- see ata.c's doc comment. Proves the pair above
// against a real read (never a write, so it's always safe to run),
// comparing its result against the existing blocking path and
// reporting how many dma_transfer_poll() calls it took in *out_polls.
// Returns 1 on a verified match, 0 on any failure (including "DMA
// isn't active on this machine" -- check ata_dma_active() first if the
// caller wants a more specific message than this test's own 0/1).
int ata_dma_nonblocking_selftest(uint32_t lba, uint32_t *out_polls);

#endif
