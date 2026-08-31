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

// WHAT A PARTITION IS FOR. A role rather than a raw type GUID: a caller
// stating sixteen bytes can state any sixteen, and the boot scan's
// "this is the firmware's, never a root" test only works if what
// `mkpart` writes is something it recognises. See enum partition_role
// in api/partition.h, whose values these mirror.
#define MKPART_ROLE_DATA      0
#define MKPART_ROLE_BIOS_BOOT 1   // GRUB's core.img; GPT only
#define MKPART_ROLE_ESP       2   // the kernel and grub.cfg -- mounted at /boot

struct mkpart_entry {
    uint64_t start_lba;
    uint64_t sectors;                 // NOT an end LBA -- GPT's inclusive
                                      // end is computed kernel-side, which
                                      // is where the off-by-one belongs
    uint8_t mbr_type;                 // MBR only; 0 means "pick the default"
    uint8_t role;                     // MKPART_ROLE_*
    uint8_t reserved[6];
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

// ---- SYS_INSTALL_BOOT ------------------------------------------------
//
// Making a disk BOOT, which on a BIOS machine is two images in two
// places plus two patches that depend on where they landed:
//
//   LBA 0            the 512-byte boot sector. Bytes 0x1b8-0x200 are the
//                    disk signature and partition table and belong to
//                    the DISK, not to the boot code being written over
//                    them -- a sector that forgets them boots
//                    beautifully and describes an empty disk. The LBA of
//                    the core image is patched in at 0x5c.
//   BIOS boot        the core image, contiguous. Its first sector finds
//   partition        the rest of itself through a block list in its own
//                    last 12 bytes -- (start LBA at 0x1f4, sector count
//                    at 0x1fc) -- and a core image written without that
//                    patch loads one sector and jumps into nothing.
//
// Both patches are the KERNEL's, because both are facts about where the
// images landed and nothing else in the request states them.
//
// WHAT THIS IS NOT. It is not "write these sectors": the destination is
// derived from the target's own partition table, so a caller cannot
// point a core image at a filesystem. Same reasoning as `struct
// mkpart_request` above, and the same missing half -- there is no
// privilege model, so INSTALL_BOOT_CONFIRM is a speed bump.
//
// WHAT IT DOES NOT KNOW. Anything about GRUB. It never parses either
// image, so a caller handing it two files of the wrong kind gets a disk
// that does not boot rather than an error -- checking would mean this
// kernel carrying a second implementation of somebody else's format.
// `/bin/install` reads both out of /boot, where the host's
// tools/install_grub.py put them.

#define INSTALL_BOOT_CONFIRM 0x1

#define INSTALL_BOOT_SECTOR_BYTES 512

struct install_boot_request {
    char device[MKPART_DEVICE_MAX];   // a WHOLE DISK; empty = the boot disk
    uint32_t flags;                   // INSTALL_BOOT_*
    uint32_t reserved;

    uint64_t boot_img;                // user pointer, exactly 512 bytes
    uint64_t boot_size;
    uint64_t core_img;                // user pointer
    uint64_t core_size;               // rounded up to a sector by the kernel
};

#endif
