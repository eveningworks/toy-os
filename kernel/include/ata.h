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
// existing caller (fs.c, tfs.c) -- neither know nor care which path is
// active; this is purely an ata.c-internal choice.
//
// Chosen over AHCI/SATA specifically because it needs nothing this
// kernel doesn't already have: PCI enumeration (build 390) and IRQ
// handling (build 400) are both now in place, which is exactly what
// unlocked this build -- but AHCI would still additionally need MMIO
// BAR mapping and scatter-gather command lists this driver has no use
// for. It's also exactly what QEMU's default `-drive ...,if=ide`
// presents. The tradeoff: real modern hardware increasingly lacks a
// legacy IDE controller at all, so this won't find a disk on that
// class of machine -- see fs.c's graceful "no disk -> RAM-only"
// fallback for what happens then, and README's "Ideas for what's next"
// for what AHCI support would additionally require.
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

// 1 if the Bus-Master DMA path is active (see this header's top
// comment), 0 if every transfer is going through the PIO fallback --
// diagnostic only, no caller needs to branch on this (both paths
// implement the exact same ata_read_sector()/ata_write_sector()
// contract below).
int ata_dma_active(void);

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
// must be in [1, ATA_MAX_SECTORS_PER_XFER]; TFS2's block size (see
// tfs.c) is chosen to exactly match that limit so a whole filesystem
// block always transfers in one call. Bounded by the DMA path's
// existing 4096-byte bounce buffer (see ata.c's ata_init_dma()) --
// raising the limit would need a bigger buffer, not just a bigger
// REG_SECCOUNT value (the drive's 8-bit sector-count register alone
// could go up to 256 in 28-bit mode). Built specifically so TFS2 could
// stop issuing one ATA command per 512-byte sector for every block of
// a large file -- see CHANGELOG.md.
#define ATA_MAX_SECTORS_PER_XFER 8 // 8 * 512B = 4096B, matches the DMA bounce buffer and TFS2's block size
int ata_read_sectors(uint32_t lba, int count, void *buf);
int ata_write_sectors(uint32_t lba, int count, const void *buf);

#endif
