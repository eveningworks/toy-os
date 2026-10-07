#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

struct pci_device;

// AHCI: SATA drives on a host bus adapter, the interface real hardware
// presents where ata.c's legacy IDE controller increasingly does not.
//
// THE SHAPE, Linux libahci's: an HBA owns up to 32 PORTS, and every port
// with a SATA disk on it becomes a DRIVE with its own command list, FIS
// area, tables, bounce buffer and lock, so two drives' commands run
// concurrently. Up to AHCI_MAX_HBAS controllers are driven. Drives are
// numbered 0.. across every HBA in probe order, ports in port order,
// and each becomes one block device (block_ahci.c: ahci0, ahci1, ...).
// Where it stops short of Linux: no port multipliers, no ATAPI, no
// hot-plug, and NCQ only for a batch (submit_batch).
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
#define AHCI_MAX_HBAS    4
#define AHCI_MAX_DRIVES  8

// The HBAs are found by PCI binding (class 0x01, subclass 0x06), which
// maps BAR5, enumerates every implemented port and IDENTIFYs every SATA
// disk. Safe on a machine with no controller at all -- every other
// ahci_* call is then a no-op or a zero, never a hang.

// How many drives answered IDENTIFY and can take transfers. Every call
// below taking a `drive` refuses (0) one that is not < this.
int ahci_drive_count(void);

// The drive's capacity in 512-byte sectors, 0 when there is no such drive.
uint64_t ahci_sector_count(int drive);

// `count` sectors from `lba`. Both return 1 on success, 0 on failure --
// a refused transfer, never a short one. `count` past
// ahci_max_sectors_per_xfer() is refused rather than split, matching
// the block layer's contract.
int ahci_read_sectors(int drive, uint64_t lba, int count, void *buf);
int ahci_write_sectors(int drive, uint64_t lba, int count, const void *buf);

// The drive's bounce buffer in sectors -- what caps one command.
int ahci_max_sectors_per_xfer(int drive);

// DATA SET MANAGEMENT's TRIM: tells the drive `count` sectors from `lba`
// hold nothing worth keeping. Returns 1 when the drive acknowledged.
//
// WHAT IT IS ACTUALLY FOR HERE is the host image: `disk.img` is created
// sparse and sparseness is only ever LOST, so without this a block
// written once stays allocated on the host forever. Whether a real SSD
// does anything with it is between the drive and its firmware.
int ahci_trim(int drive, uint64_t lba, uint32_t count);
// Every run, packed 64 to a DATA SET MANAGEMENT command.
struct blk_range;
int ahci_trim_ranges(int drive, const struct blk_range *r, int n);

// Whether a TRIM issued right now would actually go out: a drive that
// answered IDENTIFY with word 169 bit 0 set. Asked at REGISTRATION by
// block_ahci.c, which is what lets BLK_CAP_TRIM mean what it says --
// IDENTIFY has completed by then, unlike ATA's, which has to advertise
// the bit blind.
int ahci_trim_supported(int drive);

// FLUSH CACHE EXT. Returns 1 when the drive acknowledged, 0 when it
// refused or there is no drive. A caller treating 0 as "probably fine"
// is the corruption bug ata_cache.h's header describes.
int ahci_flush(int drive);

// NCQ: how many tags this driver queues on the drive (0 = none -- the
// HBA or drive lacks it, or completions do not interrupt), and several
// transfers in flight at once as the block layer's submit_batch
// (block.h).
struct blk_io;
int ahci_ncq_depth(int drive);
int ahci_submit_batch(int drive, struct blk_io *io, int n);

// ---- diagnostics, for QUERY_AHCI / QUERY_AHCI_PORT ------------------

struct ahci_drive_info {
    int      hba;            // index into the HBAs, probe order
    int      port;           // the hardware's port number
    uint64_t sectors;
    int      lba48;          // the drive's own addressing
    int      trim;
    int      ncq_depth;
    int      max_xfer;       // sectors per transfer
    int      irq_driven;     // completions arrive by interrupt
    // Command waits that parked rather than polled, and how many NCQ
    // rounds and commands went queued and how many rounds failed and
    // were replayed one command at a time.
    uint64_t cmd_sleeps, ncq_rounds, ncq_cmds, ncq_fallbacks;
    char     model[41];      // IDENTIFY's model string
};
int ahci_drive_info(int drive, struct ahci_drive_info *out);
const char *ahci_model(int drive);                   // "" for no such drive
const struct pci_device *ahci_drive_pci(int drive);  // its HBA, or NULL

struct ahci_hba_info {
    uint32_t version;        // the VS register
    uint32_t cap;            // the CAP register, raw
    int      command_slots;  // CAP.NCS + 1
    uint8_t  irq;            // PIC line, 0 when not on one
    int      irq_driven;     // an INTx line or an MSI vector
    int      port_count;     // implemented ports
    int      drives;         // of them, carrying a drive
    const struct pci_device *pci;
};
// Every HBA that was mapped, including one with no drive on it.
int ahci_hba_count(void);
int ahci_hba_info(int hba, struct ahci_hba_info *out);

// One entry per IMPLEMENTED port of an HBA, in port order -- so `index`
// is a position in that list and `port` is the hardware's own number,
// which differ whenever PI has a gap.
struct ahci_port_status {
    uint8_t  port;
    uint8_t  det;        // PxSSTS.DET: 3 = device present, link up
    uint8_t  ipm;        // PxSSTS.IPM: 1 = active
    uint8_t  speed;      // PxSSTS.SPD: 1/2/3 = 1.5/3/6 Gbps
    uint32_t signature;  // PxSIG: 0x00000101 is a SATA disk
    int      running;    // PxCMD.ST and .FRE are both set
    int      drive;      // the drive on this port, -1 if none
};

int ahci_port_status(int hba, int index, struct ahci_port_status *out);

#endif
