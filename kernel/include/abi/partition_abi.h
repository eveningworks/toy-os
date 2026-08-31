#ifndef PARTITION_ABI_H
#define PARTITION_ABI_H

#include <stdint.h>

// The kernel<->userland contract for SYS_MKPART -- writing a partition
// table to the disk.
//
// WHY A DESCRIPTION AND NOT A SECTOR. The obvious alternative was a
// raw "write any sector" syscall with /bin/mkpart doing the MBR/GPT
// encoding itself. It was rejected on three counts, and the first is
// the one that decided it:
//
//   1. This kernel has NO PRIVILEGE MODEL. There is no uid, and
//      SYS_QUERY has no check either -- so a general sector-write
//      primitive is a way for any ring-3 process to overwrite any
//      filesystem, permanently, for the convenience of one rare
//      command. A syscall that takes a TABLE can be checked; a syscall
//      that takes a sector cannot be, because "which sector is safe"
//      is exactly the question the table answers.
//   2. The encoder ends up beside the decoder, so the CRC32, the GPT
//      field offsets and the mixed-endian GUID layout have one
//      implementation each rather than two that can drift.
//   3. Some refusals are only possible in the kernel -- it is the only
//      side that knows the disk's real sector count and what is
//      currently mounted from it.
//
// Linux's BLKPG ioctl is shaped the same way (a partition description,
// not sectors), and Windows' IOCTL_DISK_SET_DRIVE_LAYOUT_EX likewise.
// Both also gate it on privilege, which is the half toy-os cannot copy
// yet -- MKPART_CONFIRM below is the stand-in, and it is a speed bump,
// not a permission check. Said plainly here so nobody mistakes it for
// one.

#define MKPART_KIND_MBR 1
#define MKPART_KIND_GPT 2

// At most four, matching MBR's hard limit -- see PART_WRITE_MAX_ENTRIES
// in api/partition.h.
#define MKPART_MAX_ENTRIES 4

#define MKPART_NAME_MAX 37 // 36 UTF-16 code units + NUL, GPT's limit

// The caller states that it knows this destroys whatever is on the
// disk. Without it, SYS_MKPART refuses while a persistent filesystem
// is mounted -- which, on this OS, is always.
//
// Deliberately a FLAG the caller sets rather than something the kernel
// infers: `fsformat tfs3 confirm` already established that a
// destructive storage operation in this OS is spelled out by the
// person asking for it.
#define MKPART_CONFIRM 0x1

struct mkpart_entry {
    uint64_t start_lba;
    uint64_t sectors;                 // NOT an end LBA -- GPT's inclusive
                                      // end is computed kernel-side, which
                                      // is where the off-by-one belongs
    uint8_t mbr_type;                 // MBR only; 0 means "pick the default"
    uint8_t reserved[7];
    char name[MKPART_NAME_MAX];       // GPT only; ASCII, NUL-terminated
    char pad[3];
};

// A device name as `lsblk` prints it (`ata0`, `virtio1`), matching
// BLK_NAME_MAX. Long enough for "virtio0p15" so one constant serves
// both, even though only a WHOLE DISK is legal here.
#define MKPART_DEVICE_MAX 16

struct mkpart_request {
    uint32_t kind;                    // MKPART_KIND_*
    uint32_t count;                   // 1..MKPART_MAX_ENTRIES
    uint32_t flags;                   // MKPART_*
    uint32_t reserved;

    // WHICH DISK. Empty means the boot disk, which is what this syscall
    // could only ever write before -- so a caller that predates the
    // field keeps working, and a zeroed request still cannot reach some
    // other machine's disk by accident.
    //
    // A WHOLE DISK ONLY. Naming a partition (`ata0p3`) is refused:
    // writing a table inside a partition produces one describing
    // windows into itself, and there is no reading of that which is
    // what anybody meant.
    char device[MKPART_DEVICE_MAX];
    char pad2[4];

    struct mkpart_entry entries[MKPART_MAX_ENTRIES];
};

#endif
