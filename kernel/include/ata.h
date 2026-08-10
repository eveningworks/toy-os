#ifndef ATA_H
#define ATA_H

#include <stdint.h>

// A minimal legacy PIO ATA/IDE driver -- primary bus, master drive only,
// 28-bit LBA addressing, polling (no interrupts: IRQ14 stays masked in
// the PIC, see pic_clear_mask() calls in idt.c, so this deliberately
// never needs an ISR). This is the same "poll a status register in a
// loop" shape as i8042.c and the PIT, just for a different piece of
// hardware -- no DMA, no command queueing, one sector at a time.
//
// Chosen over AHCI/SATA specifically because it needs nothing this
// kernel doesn't already have: no PCI enumeration (legacy ATA lives at
// fixed ports 0x1F0-0x1F7/0x3F6, unlike AHCI which is found via PCI
// config space and driven through an MMIO BAR), no interrupt handling,
// no scatter-gather command lists. It's also exactly what QEMU's
// default `-drive ...,if=ide` presents. The tradeoff: real modern
// hardware increasingly lacks a legacy IDE controller at all, so this
// won't find a disk on that class of machine -- see fs.c's graceful
// "no disk -> RAM-only" fallback for what happens then, and README's
// "Ideas for what's next" for what AHCI support would additionally
// require.
#define ATA_SECTOR_SIZE 512

// Probes the primary bus for a master drive via IDENTIFY DEVICE and
// records whether one was found. Safe to call even if there's no
// controller/drive at all (a floating bus reads back 0xFF and this
// returns immediately) -- every other ata_* function is a no-op after
// that, not a hang.
void ata_init(void);

// 1 if ata_init() found a usable drive, 0 otherwise (no controller, no
// drive, or something that identified as non-ATA, e.g. an ATAPI
// optical drive on the same bus -- the boot CD is on the SECONDARY bus
// in every QEMU invocation this project uses, see the Makefile, so it
// never conflicts with this).
int ata_present(void);

// Reads/writes exactly one ATA_SECTOR_SIZE-byte sector at 28-bit LBA
// `lba`. Returns 1 on success, 0 on failure (no drive present, or the
// drive reported an error / timed out waiting for it to become ready --
// this polls with a bounded retry count rather than looping forever).
int ata_read_sector(uint32_t lba, void *buf);
int ata_write_sector(uint32_t lba, const void *buf);

#endif
