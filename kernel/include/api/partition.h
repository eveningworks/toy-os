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

enum partition_table_kind {
    PART_TABLE_NONE, // no 0x55AA signature at LBA 0 -- e.g. today's disk.img (raw TFS2)
    PART_TABLE_MBR,  // legacy MBR, no protective 0xEE entry
    PART_TABLE_GPT,  // protective MBR + a GPT header that passed its CRC32 check
};

struct partition_entry {
    // MBR fields, always valid for a PART_TABLE_MBR entry.
    uint8_t mbr_type;
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
};

// Reads LBA 0 (and LBA 1 + the partition entry array, if a protective
// MBR pointing at a GPT is found) and fills *out. Returns 1 if LBA 0
// was actually readable, 0 on a disk read failure -- out->kind is
// PART_TABLE_NONE either way if nothing valid was found there, so most
// callers only need to check out->kind, not this return value.
int partition_read_table(struct partition_table *out);

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

#endif
