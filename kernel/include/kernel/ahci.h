#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

// AHCI: SATA drives on a host bus adapter, the interface real hardware
// presents where ata.c's legacy IDE controller increasingly does not.
//
// THE SHAPE, AND WHERE IT DELIBERATELY STOPS SHORT OF LINUX'S. An HBA
// owns up to 32 PORTS, each with its own command list of 32 slots, and
// Linux (drivers/ata/ahci.c over libahci) drives every port as a full
// libata port with NCQ across the slots. This driver enumerates every
// implemented port and reports what is on it -- that part is the
// hardware's real structure and skipping it would be a lie about what
// was found -- but only ONE drive becomes the block device, using slot
// 0 with one command outstanding. That is the honest ceiling of a
// kernel whose block layer has a singular blk_active() and no /dev:
// NCQ buys nothing until something can issue a second request while the
// first is in flight. docs/roadmap.md holds the rest.
//
// TRANSFERS GO THROUGH A BOUNCE BUFFER, like ata.c's, for the same
// reason: the PRDT then describes memory this driver allocated and
// knows the physical layout of, rather than whatever pointer a caller
// happened to have. The PRDT carries ONE ENTRY PER 4 KiB PAGE, which is
// what Linux's ahci_fill_sg() produces from a page-granular
// scatterlist -- so the multi-entry path is the ordinary path and not
// dead code waiting for a bigger transfer.
//
// NO SECTOR CACHE. ata_cache.c sits under ata_read_sectors() because
// PIO makes a 512-byte metadata read genuinely expensive; a DMA
// transfer into an already-mapped buffer is not, and virtio-blk is
// uncached for the same reason. BLK_CAP_FLUSH here therefore means a
// real FLUSH CACHE EXT reaching the drive, not a write-back queue.

#define AHCI_SECTOR_SIZE 512

// Finds the HBA on the PCI bus (class 0x01, subclass 0x06), maps BAR5,
// brings up every implemented port with a device on it, and IDENTIFYs
// the first SATA drive found. Safe on a machine with no controller at
// all -- every other ahci_* call is then a no-op or a zero, never a
// hang. Call after pmm_init() (the command list and bounce buffer come
// from pmm_alloc_contiguous()) and before anything mounts.

// 1 when a SATA drive answered IDENTIFY and transfers can be issued.
int ahci_present(void);

// The drive's capacity in 512-byte sectors, 0 when there is none.
// 32-bit like the rest of this kernel's block layer, so a drive larger
// than 2 TiB is CLAMPED and says so at init rather than wrapping.
uint32_t ahci_sector_count(void);

// `count` sectors from `lba`. Both return 1 on success, 0 on failure --
// a refused transfer, never a short one. `count` past
// ahci_max_sectors_per_xfer() is refused rather than split, matching
// the block layer's contract.
int ahci_read_sectors(uint32_t lba, int count, void *buf);
int ahci_write_sectors(uint32_t lba, int count, const void *buf);

// The bounce buffer's size in sectors -- what caps one command.
int ahci_max_sectors_per_xfer(void);

// DATA SET MANAGEMENT's TRIM: tells the drive `count` sectors from `lba`
// hold nothing worth keeping. Returns 1 when the drive acknowledged.
//
// WHAT IT IS ACTUALLY FOR HERE is the host image: `disk.img` is created
// sparse and sparseness is only ever LOST, so without this a block
// written once stays allocated on the host forever. Whether a real SSD
// does anything with it is between the drive and its firmware.
int ahci_trim(uint32_t lba, uint32_t count);
// Every run, packed 64 to a DATA SET MANAGEMENT command.
struct blk_range;
int ahci_trim_ranges(const struct blk_range *r, int n);

// Whether a TRIM issued right now would actually go out: a drive that
// answered IDENTIFY with word 169 bit 0 set. Asked at REGISTRATION by
// block_ahci.c, which is what lets BLK_CAP_TRIM mean what it says --
// IDENTIFY has completed by then, unlike ATA's, which has to advertise
// the bit blind.
int ahci_trim_supported(void);

// FLUSH CACHE EXT. Returns 1 when the drive acknowledged, 0 when it
// refused or there is no drive. A caller treating 0 as "probably fine"
// is the corruption bug ata_cache.h's header describes.
int ahci_flush(void);

// ---- diagnostics, for QUERY_AHCI / QUERY_AHCI_PORT ------------------
//
// Reported rather than acted on: this driver uses one port and one
// command slot, and says so, instead of hiding the ports it found.

int      ahci_controller_present(void); // an HBA was found and mapped
uint32_t ahci_version(void);            // the VS register
uint32_t ahci_capabilities(void);       // the CAP register, raw
int      ahci_command_slots(void);      // CAP.NCS + 1
int      ahci_active_port(void);        // the port carrying the drive, -1 if none
uint8_t  ahci_irq_line(void);           // PIC line, 0 when polled
int      ahci_irq_driven(void);         // completions arrive by interrupt
uint64_t ahci_cmd_sleeps(void);         // command waits that parked, not polled

// NCQ: how many tags this driver queues (0 = none -- the HBA or drive
// lacks it, or completions do not interrupt), and several transfers in
// flight at once as the block layer's submit_batch (block.h). The
// counters say how many rounds and commands went queued, and how many
// rounds failed and were replayed one command at a time.
struct blk_io;
int      ahci_ncq_depth(void);
int      ahci_submit_batch(struct blk_io *io, int n);
uint64_t ahci_ncq_rounds(void);
uint64_t ahci_ncq_cmds(void);
uint64_t ahci_ncq_fallbacks(void);
int      ahci_lba48(void);              // the drive's own addressing
const char *ahci_model(void);           // IDENTIFY's model string, "" if none

// One entry per IMPLEMENTED port, in port order -- so `index` is a
// position in that list and `port` is the hardware's own number, which
// differ whenever PI has a gap.
struct ahci_port_status {
    uint8_t  port;
    uint8_t  det;        // PxSSTS.DET: 3 = device present, link up
    uint8_t  ipm;        // PxSSTS.IPM: 1 = active
    uint8_t  speed;      // PxSSTS.SPD: 1/2/3 = 1.5/3/6 Gbps
    uint32_t signature;  // PxSIG: 0x00000101 is a SATA disk
    int      running;    // PxCMD.ST and .FRE are both set
    int      active;     // this is the port the block device sits on
};

int ahci_port_count(void);
int ahci_port_status(int index, struct ahci_port_status *out);

#endif
