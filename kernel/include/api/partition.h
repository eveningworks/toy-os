#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>

// MBR/GPT partition tables: reading them, and writing them.
//
// THIS IS ON THE BOOT PATH NOW. It was read-only and diagnostic-only
// for a long time -- this comment said so, and said the boot/mount path
// never consulted it -- because there was nothing that could mount a
// volume anywhere but LBA 0. kernel/fs/vfs.c's probe now scans this
// table for a mountable partition, so a wrong answer here is a machine
// that does not boot rather than a command that prints nonsense.
//
// Reading is safe on any disk. WRITING IS DESTRUCTIVE and lives behind
// partition_write_table() below, SYS_MKPART, and `/bin/mkpart`.
//
// The whole-disk case has not gone away: a disk with no table is still
// the ordinary shape here, and this repo's own disk.img is one. See
// docs/decisions/storage.md.

#define PART_MAX_ENTRIES 16 // sanity cap on how many entries are read/reported

// How many partitions this kernel will WRITE. Four, because that is
// MBR's hard limit and nothing on a toy-os disk wants more -- a GPT
// table still gets its full 128 on-disk slots, so a real tool can add
// to one this kernel wrote. Reading is capped separately and higher
// (PART_MAX_ENTRIES), since a disk arriving from elsewhere is not
// bound by what we would have written.
#define PART_WRITE_MAX_ENTRIES 4

// WHAT A PARTITION IS FOR, as far as this kernel is willing to name it.
// Three, because three is what a bootable toy-os disk has: GRUB's
// core.img in a BIOS boot partition, the kernel and grub.cfg in an ESP,
// and the root filesystem in a data partition.
//
// A ROLE rather than a raw GUID on purpose. A caller stating sixteen
// bytes can state any sixteen; a caller stating a role can only ask for
// something this kernel already knows how to recognise, which is what
// makes `partition_is_firmware()` and the boot scan keep agreeing with
// what `mkpart` writes. MBR gets the nearest equivalent type byte, and
// has no BIOS boot type at all -- there, core.img goes in the gap.
enum partition_role {
    PART_ROLE_DATA = 0,
    PART_ROLE_BIOS_BOOT,
    PART_ROLE_ESP,
};

// The GPT type GUID for a role. Returns 0 for a role this does not
// know, which is what stops a bad value becoming an all-zero type --
// the encoding for an UNUSED slot.
int partition_type_guid(enum partition_role role, uint8_t out[16]);

enum partition_table_kind {
    PART_TABLE_NONE, // no 0x55AA signature at LBA 0 -- a flat whole-disk volume
    PART_TABLE_MBR,  // legacy MBR, no protective 0xEE entry
    PART_TABLE_GPT,  // protective MBR + a GPT header that passed its CRC32 check
};

struct partition_entry {
    // MBR fields, always valid for a PART_TABLE_MBR entry.
    uint8_t mbr_type;
    // The boot indicator, 0x80 in the table and 1 here. A legacy BIOS
    // picks the ACTIVE partition to chain to, and a number of them
    // refuse a disk on which nothing is marked -- so this is not
    // decoration, it is whether the firmware will boot the disk at all.
    // Meaningless on GPT, whose protective entry stays 0 by spec.
    uint8_t mbr_active;
    // Which of LBA 0's four slots it came from, 1-based. entries[] skips
    // empty slots, so this and the entry's index differ after a gap --
    // and an MBR PARTUUID names the SLOT.
    uint8_t mbr_slot;
    uint32_t mbr_lba_start;
    uint32_t mbr_num_sectors;

    // GPT fields, always valid for a PART_TABLE_GPT entry. type_guid
    // all-zero means "unused slot" (already filtered out by
    // partition_read_table(), never appears in out->entries[]).
    uint8_t gpt_type_guid[16];
    uint8_t gpt_unique_guid[16];
    uint64_t gpt_lba_start;
    uint64_t gpt_lba_end;
    char gpt_name[37]; // UTF-16LE partition name, ASCII-truncated + NUL-terminated
};

struct partition_table {
    enum partition_table_kind kind;
    int entry_count; // how many of entries[] are filled (<= PART_MAX_ENTRIES)
    struct partition_entry entries[PART_MAX_ENTRIES];

    // GPT-only: the disk's own GUID from the header, and whether the
    // header's CRC32 actually checked out (kind is only ever GPT if
    // this is true -- kept as its own field so a caller can tell "found
    // a protective MBR but the GPT header itself was corrupt" apart
    // from "no GPT at all", if that distinction ever matters).
    uint8_t disk_guid[16];
    int gpt_header_valid;

    // LBA 0's NT disk signature (bytes 440-443), read whatever the kind
    // -- an MBR partition's PARTUUID is built from it.
    uint32_t mbr_disk_signature;
};

// Reads LBA 0 (and LBA 1 + the partition entry array, if a protective
// MBR pointing at a GPT is found) and fills *out. Returns 1 if LBA 0
// was actually readable, 0 on a disk read failure -- out->kind is
// PART_TABLE_NONE either way if nothing valid was found there, so most
// callers only need to check out->kind, not this return value.
int partition_read_table(struct partition_table *out);

// The same, on a NAMED device rather than the active one. A machine has
// more than one disk now (block.h's table), and a partition table that
// can only be read off the active one leaves every other disk's
// partitions unreachable -- unnameable, and therefore unmountable.
struct block_device;
int partition_read_table_of(const struct block_device *dev,
                            struct partition_table *out);

// Is this partition the FIRMWARE's rather than an OS's? A GPT BIOS boot
// partition or an EFI System Partition (MBR: type 0xEF). Both are on
// toy-os's own disk -- GRUB's core.img and /boot/kernel.bin live in
// them -- and the boot-time scan neither mounts from one nor offers one
// to `fsformat`. See partition.c for the GUIDs and the reasoning.
int partition_is_firmware(const struct partition_entry *pe,
                          enum partition_table_kind kind);

// Entry `index`'s PARTUUID as Linux's `root=PARTUUID=` and GRUB's
// `probe --part-uuid` spell it: a GPT entry's unique GUID in the usual
// lowercase 8-4-4-4-12 form, an MBR entry as `<disk signature>-<nn>`
// with nn its mbr_slot. Returns 0 (writing nothing usable) for no
// table, an index past the end, or a buffer too small -- 37 bytes fits
// both.
int partition_partuuid(const struct partition_table *t, int index, char *out, uint32_t size);

// Is it specifically an EFI System Partition? A NARROWER question than
// partition_is_firmware(), and the two are asked by different passes
// for different reasons: nothing may become the ROOT from a firmware
// partition, but the ESP is exactly what mounts at /boot -- it is FAT,
// it holds the bootloader and the kernel image, and reading it is the
// point. A BIOS boot partition is not this: it holds a raw core.img
// with no filesystem in it at all.
int partition_is_esp(const struct partition_entry *pe,
                     enum partition_table_kind kind);

// ---- writing --------------------------------------------------------
//
// Writing a table is DESTRUCTIVE and this half of the API does not
// pretend otherwise -- see partition_write_table()'s comment in
// partition.c for why the kernel encodes the table rather than exposing
// a raw sector write to ring 3.

// Every check the kernel can make without writing anything: bounds,
// overlap between partitions, overlap with the table's own reserved
// sectors, and MBR's four-slot limit. Sets *why to a short reason.
// Returns 1 if the table is safe to write.
//
// Split out from the writer so a caller can offer a dry run, and so a
// KTEST can assert the refusals without touching the disk.
int partition_validate(const struct partition_table *in, const char **why);

// Fills each entry's unique GUID (fresh and random), its type GUID
// (generic filesystem data) and its MBR type byte if unset. Call
// before partition_write_table() when creating a NEW table; skip it
// when rewriting one whose identities should survive.
void partition_fill_defaults(struct partition_table *t);

// Writes `in` to the disk. Validates first and refuses rather than
// writing a partial table. For GPT this writes the backup structures
// FIRST and the protective MBR LAST, so an interrupted write leaves a
// disk that reads as unpartitioned rather than one advertising a table
// that is not there.
//
// Does NOT remount anything: the volume in use is unaffected, and the
// new table takes effect at the next boot. Linux behaves the same way
// (the kernel refuses to re-read a table on a busy disk).
//
// Returns 1 on success, 0 on refusal or a write failure -- the reason
// is logged either way.
int partition_write_table(const struct partition_table *in);

// The same, on a NAMED disk. `dev` must be a WHOLE DISK -- a partition
// device would put a table inside a partition, describing windows into
// itself. This is what an installer writes its target's table with, and
// what SYS_MKPART's `device` field reaches; the two calls above are the
// boot disk's, which is what every caller meant before a table could be
// written anywhere else.
int partition_write_table_of(const struct block_device *dev,
                             const struct partition_table *in);
int partition_validate_on(const struct block_device *dev,
                          const struct partition_table *in, const char **why);

#endif
