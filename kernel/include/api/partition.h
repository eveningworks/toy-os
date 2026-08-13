#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>

// MBR/GPT partition table parsing (Milestone 3, docs/roadmap.md) --
// read-only, diagnostic-only. TFS2 (kernel/fs/tfs.c) occupies the
// whole disk starting at LBA 0 today, no partition table at all, so
// this never gets consulted by the boot/mount path -- it exists purely
// so `disk.img` (or any other attached disk) CAN be inspected, same
// spirit as `lspci`. See docs/decisions.md for the full reasoning.

#define PART_MAX_ENTRIES 16 // sanity cap on how many entries are read/reported

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

#endif
