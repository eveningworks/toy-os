// See partition.h for the design writeup. Struct layout is read
// byte-by-byte from a raw sector buffer rather than cast through
// packed structs (unlike elf.c's approach) -- GPT's spec fields are
// all little-endian and this host is little-endian x86-64 too, so a
// packed-struct cast would work field-by-field just like elf.c's
// does, but the GPT header mixes fixed-width scalar fields with a
// 16-byte GUID whose own internal layout is itself mixed (three
// little-endian integers, then 8 raw bytes) -- reading everything
// explicitly by offset keeps that one irregularity contained in a
// single small helper (guid_copy()) instead of needing a second
// struct-packing convention just for GUIDs.
#include "partition.h"
#include "ata.h"
#include "string.h"

#define MBR_SIGNATURE_OFFSET 510
#define MBR_ENTRY_TABLE_OFFSET 446
#define MBR_ENTRY_SIZE 16
#define MBR_ENTRY_COUNT 4
#define MBR_TYPE_GPT_PROTECTIVE 0xEE

#define GPT_HEADER_LBA 1
#define GPT_SIGNATURE "EFI PART"

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    uint64_t lo = read_le32(p);
    uint64_t hi = read_le32(p + 4);
    return lo | (hi << 32);
}

static void guid_copy(uint8_t *out, const uint8_t *raw) {
    for (int i = 0; i < 16; i++) out[i] = raw[i];
}

// Standard CRC-32 (IEEE 802.3 / zlib polynomial, 0xEDB88320) -- no
// precomputed table, just the bit-at-a-time form. GPT headers are
// small (~92 bytes) and this runs once per `parttable` invocation, so
// the simpler code is worth more here than the table's speed.
static uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1));
            crc = (crc >> 1) ^ (0xEDB88320 & mask);
        }
    }
    return ~crc;
}

// Fills out->entries[0..3] from the 4 legacy MBR entries in `mbr`
// (already known to have a valid 0x55AA signature), skipping unused
// (type 0) slots. Used both for a plain PART_TABLE_MBR result and, in
// the GPT case, only to have already scanned for the protective 0xEE
// entry -- the GPT path re-fills out->entries itself afterward.
static void parse_mbr_entries(const uint8_t *mbr, struct partition_table *out) {
    out->entry_count = 0;
    for (int i = 0; i < MBR_ENTRY_COUNT; i++) {
        const uint8_t *e = mbr + MBR_ENTRY_TABLE_OFFSET + i * MBR_ENTRY_SIZE;
        uint8_t type = e[4];
        if (type == 0) continue; // unused slot

        struct partition_entry *pe = &out->entries[out->entry_count++];
        k_memset(pe, 0, sizeof(*pe));
        pe->mbr_type = type;
        pe->mbr_lba_start = read_le32(e + 8);
        pe->mbr_num_sectors = read_le32(e + 12);
    }
}

// True if any of the 4 MBR entries is a protective-GPT marker (type
// 0xEE) -- the spec-correct way to decide "read LBA 1 as a GPT header"
// instead of just always trying it.
static int mbr_has_gpt_protective_entry(const uint8_t *mbr) {
    for (int i = 0; i < MBR_ENTRY_COUNT; i++) {
        const uint8_t *e = mbr + MBR_ENTRY_TABLE_OFFSET + i * MBR_ENTRY_SIZE;
        if (e[4] == MBR_TYPE_GPT_PROTECTIVE) return 1;
    }
    return 0;
}

// Converts a GPT partition name (36 UTF-16LE code units, 72 bytes)
// into a NUL-terminated ASCII-ish string -- this kernel has no Unicode
// rendering at all (see docs/decisions.md's Latin-1-over-UTF-8 note),
// so anything outside printable ASCII becomes '?' rather than
// attempting real transcoding. Stops at the first NUL code unit, same
// as the name being a C string in spirit.
static void gpt_name_to_ascii(const uint8_t *utf16le, char *out /* [37] */) {
    int n = 0;
    for (int i = 0; i < 36; i++) {
        uint16_t unit = (uint16_t)utf16le[i * 2] | ((uint16_t)utf16le[i * 2 + 1] << 8);
        if (unit == 0) break;
        out[n++] = (unit >= 0x20 && unit < 0x7F) ? (char)unit : '?';
    }
    out[n] = '\0';
}

int partition_read_table(struct partition_table *out) {
    k_memset(out, 0, sizeof(*out));
    out->kind = PART_TABLE_NONE;

    uint8_t mbr[ATA_SECTOR_SIZE];
    if (!ata_read_sector(0, mbr)) return 0;

    if (mbr[MBR_SIGNATURE_OFFSET] != 0x55 || mbr[MBR_SIGNATURE_OFFSET + 1] != 0xAA) {
        return 1; // readable disk, just no MBR/GPT signature -- PART_TABLE_NONE stands
    }

    if (!mbr_has_gpt_protective_entry(mbr)) {
        out->kind = PART_TABLE_MBR;
        parse_mbr_entries(mbr, out);
        return 1;
    }

    // Protective MBR present -- read and validate the GPT header at LBA 1.
    uint8_t hdr[ATA_SECTOR_SIZE];
    if (!ata_read_sector(GPT_HEADER_LBA, hdr)) {
        // Disk read failure, not "no GPT" -- fall back to reporting the
        // protective MBR's own (single, type-0xEE) entry rather than
        // silently claiming PART_TABLE_NONE.
        out->kind = PART_TABLE_MBR;
        parse_mbr_entries(mbr, out);
        return 1;
    }

    if (k_strncmp((const char *)hdr, GPT_SIGNATURE, 8) != 0) {
        out->kind = PART_TABLE_MBR;
        parse_mbr_entries(mbr, out);
        return 1;
    }

    uint32_t header_size = read_le32(hdr + 12);
    uint32_t stored_crc = read_le32(hdr + 16);
    if (header_size > ATA_SECTOR_SIZE) header_size = ATA_SECTOR_SIZE; // sanity clamp -- always 92 in practice

    uint8_t hdr_for_crc[ATA_SECTOR_SIZE];
    k_memcpy(hdr_for_crc, hdr, header_size);
    hdr_for_crc[16] = 0; hdr_for_crc[17] = 0; hdr_for_crc[18] = 0; hdr_for_crc[19] = 0; // header_crc32 field zeroed for its own check
    uint32_t computed_crc = crc32(hdr_for_crc, header_size);

    if (computed_crc != stored_crc) {
        // Signature matched but the header itself is corrupt -- same
        // fallback as an unreadable/missing header above.
        out->kind = PART_TABLE_MBR;
        parse_mbr_entries(mbr, out);
        return 1;
    }

    out->kind = PART_TABLE_GPT;
    out->gpt_header_valid = 1;
    guid_copy(out->disk_guid, hdr + 56);

    uint64_t entry_lba = read_le64(hdr + 72);
    uint32_t num_entries = read_le32(hdr + 80);
    uint32_t entry_size = read_le32(hdr + 84);
    if (entry_size == 0 || entry_size > ATA_SECTOR_SIZE) entry_size = 128; // spec default -- guards a div-by-zero below
    if (num_entries > PART_MAX_ENTRIES) num_entries = PART_MAX_ENTRIES; // only read/report as many as we display

    uint32_t entries_per_sector = ATA_SECTOR_SIZE / entry_size;
    uint32_t sectors_needed = (num_entries + entries_per_sector - 1) / entries_per_sector;

    out->entry_count = 0;
    for (uint32_t s = 0; s < sectors_needed; s++) {
        uint8_t buf[ATA_SECTOR_SIZE];
        if (!ata_read_sector((uint32_t)entry_lba + s, buf)) break;

        for (uint32_t i = 0; i < entries_per_sector && out->entry_count < PART_MAX_ENTRIES; i++) {
            const uint8_t *e = buf + i * entry_size;

            int all_zero = 1;
            for (int b = 0; b < 16; b++) if (e[b] != 0) { all_zero = 0; break; }
            if (all_zero) continue; // unused slot

            struct partition_entry *pe = &out->entries[out->entry_count++];
            k_memset(pe, 0, sizeof(*pe));
            guid_copy(pe->gpt_type_guid, e);
            guid_copy(pe->gpt_unique_guid, e + 16);
            pe->gpt_lba_start = read_le64(e + 32);
            pe->gpt_lba_end = read_le64(e + 40);
            gpt_name_to_ascii(e + 56, pe->gpt_name);
        }
    }

    return 1;
}
