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
#include "block.h"
#include "string.h"
#include "kcrc.h"
#include "krandom.h" // GUIDs -- see guid_generate()
#include "klog.h"
#include "kfmt.h" // klog_printf
#include "heap.h" // a GPT header's whole block

// driver-none: MBR/GPT parsing, on a disk a driver already drives

// The block layer's unit, not ATA's (this file once read an IDE disk
// directly). Every read and write below is in these units.
//
// TWO UNITS MEET HERE ON A 4K-SECTOR DISK. The table's own LBAs -- the
// header's, every entry's -- count the DEVICE's logical blocks, per the
// UEFI spec, while `struct partition_table` carries 512-byte sectors
// like the rest of the kernel (Linux's sysfs `start`/`size` rule). So
// every value crossing the disk boundary is scaled by `spb`, and every
// sub-block read or write goes through blkdev_*_partial().
#define PART_SECTOR_SIZE 512

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

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void write_le64(uint8_t *p, uint64_t v) {
    write_le32(p, (uint32_t)v);
    write_le32(p + 4, (uint32_t)(v >> 32));
}

// A version-4 (random) GUID, per RFC 4122 -- the variant and version
// nibbles are forced, everything else is entropy.
//
// **krandom is NOT a CSPRNG here and this does not need it to be.** A
// GUID's job is to not collide, not to be unguessable: the quality
// tiers that matter to a stack canary do not matter to a disk
// identifier, and even KRANDOM_JITTER gives collision odds that round
// to zero for the handful of partitions one machine will ever have.
// Stated rather than assumed, because krandom.h's whole point is that
// a caller says how much it needs to trust its bytes.
//
// The byte order is GPT's, which is the one irregular thing about a
// GUID: the first three fields are little-endian integers and the last
// eight bytes are raw, so the "same" GUID prints differently depending
// on which half you are looking at. Generating all 16 bytes randomly
// makes that irrelevant here -- but guid_copy() above reads them back
// in the same layout, so a round trip is exact.
static void guid_generate(uint8_t *out) {
    krandom_bytes(out, 16);
    out[7] = (uint8_t)((out[7] & 0x0F) | 0x40); // version 4
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80); // RFC 4122 variant
}

// Fills out->entries[0..3] from the 4 legacy MBR entries in `mbr`
// (already known to have a valid 0x55AA signature), skipping unused
// (type 0) slots. Used both for a plain PART_TABLE_MBR result and, in
// the GPT case, only to have already scanned for the protective 0xEE
// entry -- the GPT path re-fills out->entries itself afterward.
static void parse_mbr_entries(const uint8_t *mbr, uint32_t spb, struct partition_table *out) {
    out->entry_count = 0;
    for (int i = 0; i < MBR_ENTRY_COUNT; i++) {
        const uint8_t *e = mbr + MBR_ENTRY_TABLE_OFFSET + i * MBR_ENTRY_SIZE;
        uint8_t type = e[4];
        if (type == 0) continue; // unused slot

        struct partition_entry *pe = &out->entries[out->entry_count++];
        k_memset(pe, 0, sizeof(*pe));
        pe->mbr_type = type;
        pe->mbr_active = (e[0] == 0x80);   // read back, so `parttable` can show it
        pe->mbr_lba_start = read_le32(e + 8) * spb;
        pe->mbr_num_sectors = read_le32(e + 12) * spb;
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

// Walks the GPT partition entry array. Its own function, and therefore
// its own stack frame, so its 512-byte sector buffer does not sit
// alongside the caller's -- this file is on the BOOT path now (vfs.c
// scans for a mountable partition) and the kernel frame budget is 1 KB.
//
// `noinline` is load-bearing, not decoration: each of these has a
// single caller, so at -O2 GCC inlines them straight back and the
// frames merge again -- which is exactly what the split was for. Same
// reasoning as syscalls.h's SYSCALL_HANDLER.
static __attribute__((noinline)) void parse_gpt_entries(const struct block_device *dev,
                              uint64_t entry_lba, uint32_t num_entries,
                              uint32_t entry_size, struct partition_table *out) {
    uint32_t spb = blkdev_block_sectors(dev);
    uint32_t entries_per_sector = PART_SECTOR_SIZE / entry_size;
    uint32_t sectors_needed = (num_entries + entries_per_sector - 1) / entries_per_sector;

    out->entry_count = 0;
    for (uint32_t s = 0; s < sectors_needed; s++) {
        uint8_t buf[PART_SECTOR_SIZE];
        if (!blkdev_read_partial(dev, (uint64_t)entry_lba * spb + s, 1, buf)) break;

        for (uint32_t i = 0; i < entries_per_sector && out->entry_count < PART_MAX_ENTRIES; i++) {
            const uint8_t *e = buf + i * entry_size;

            int all_zero = 1;
            for (int b = 0; b < 16; b++) if (e[b] != 0) { all_zero = 0; break; }
            if (all_zero) continue; // unused slot

            struct partition_entry *pe = &out->entries[out->entry_count++];
            k_memset(pe, 0, sizeof(*pe));
            guid_copy(pe->gpt_type_guid, e);
            guid_copy(pe->gpt_unique_guid, e + 16);
            pe->gpt_lba_start = read_le64(e + 32) * spb;
            pe->gpt_lba_end = (read_le64(e + 40) + 1) * spb - 1;   // inclusive
            gpt_name_to_ascii(e + 56, pe->gpt_name);
        }
    }
}

// Reads and validates the GPT header at LBA 1 and, if it checks out,
// fills *out from the entry array. Returns 1 on success, 0 if the
// caller should fall back to reporting the protective MBR's own
// entries -- which is what an unreadable, unsigned or CRC-failing
// header means, and is deliberately NOT the same as "no GPT here".
//
// Split from partition_read_table() for the stack: the header buffer
// and the MBR buffer no longer share a frame.
static __attribute__((noinline)) int parse_gpt(const struct block_device *dev,
                                               struct partition_table *out) {
    uint8_t hdr[PART_SECTOR_SIZE];
    if (!blkdev_read_partial(dev, GPT_HEADER_LBA * blkdev_block_sectors(dev), 1, hdr)) return 0;
    if (k_strncmp((const char *)hdr, GPT_SIGNATURE, 8) != 0) return 0;

    uint32_t header_size = read_le32(hdr + 12);
    uint32_t stored_crc = read_le32(hdr + 16);
    if (header_size > PART_SECTOR_SIZE) header_size = PART_SECTOR_SIZE; // sanity clamp -- always 92 in practice

    // Zeroed IN PLACE rather than into a second buffer: the spec's own
    // rule is that header_crc32 reads as zero for its own computation,
    // stored_crc is already saved above, and nothing below reads those
    // four bytes again. The copy cost 512 bytes of frame for nothing.
    hdr[16] = 0; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;
    if (kcrc32(hdr, header_size) != stored_crc) return 0;

    out->kind = PART_TABLE_GPT;
    out->gpt_header_valid = 1;
    guid_copy(out->disk_guid, hdr + 56);

    uint64_t entry_lba = read_le64(hdr + 72);
    uint32_t num_entries = read_le32(hdr + 80);
    uint32_t entry_size = read_le32(hdr + 84);
    if (entry_size == 0 || entry_size > PART_SECTOR_SIZE) entry_size = 128; // spec default -- guards a div-by-zero below
    if (num_entries > PART_MAX_ENTRIES) num_entries = PART_MAX_ENTRIES; // only read/report as many as we display

    parse_gpt_entries(dev, entry_lba, num_entries, entry_size, out);
    return 1;
}

// Reads the table on the ACTIVE disk -- the boot-time caller and every
// existing tool mean that one.
int partition_read_table(struct partition_table *out) {
    return partition_read_table_of(blk_root_disk(), out);
}

// ...and the same on ANY disk, which is what makes a second drive's
// partitions reachable at all. Every read below goes through `dev`.
int partition_read_table_of(const struct block_device *dev,
                            struct partition_table *out) {
    if (!dev) return 0;
    k_memset(out, 0, sizeof(*out));
    out->kind = PART_TABLE_NONE;

    uint8_t mbr[PART_SECTOR_SIZE];
    if (!blkdev_read_partial(dev, 0, 1, mbr)) return 0;
    uint32_t spb = blkdev_block_sectors(dev);

    if (mbr[MBR_SIGNATURE_OFFSET] != 0x55 || mbr[MBR_SIGNATURE_OFFSET + 1] != 0xAA) {
        return 1; // readable disk, just no MBR/GPT signature -- PART_TABLE_NONE stands
    }

    if (!mbr_has_gpt_protective_entry(mbr)) {
        out->kind = PART_TABLE_MBR;
        parse_mbr_entries(mbr, spb, out);
        return 1;
    }

    // Protective MBR present -- the GPT header at LBA 1 decides. Any
    // failure there falls back to reporting the protective MBR itself,
    // rather than silently claiming PART_TABLE_NONE.
    if (!parse_gpt(dev, out)) {
        out->kind = PART_TABLE_MBR;
        parse_mbr_entries(mbr, spb, out);
    }
    return 1;
}

// ---- writing a table -----------------------------------------------
//
// WHY THE KERNEL ENCODES THIS AND RING 3 DOES NOT. The alternative was
// a raw sector-write syscall with `mkpart` doing the encoding in ring
// 3, and it was rejected: this kernel has no privilege model at all
// (SYS_QUERY has no check either), so a general "write any sector"
// primitive is a way for any process to corrupt any filesystem, for
// the convenience of one rare command. A syscall that takes a table
// DESCRIPTION can be checked -- see partition_validate() -- and there
// is no primitive left over for anything else to misuse. Linux's
// BLKPG is shaped the same way, and for the same reason.
//
// The second win is that the encoder sits beside the decoder, so the
// CRC32, the GUID layout and the field offsets have exactly one
// implementation each and a round trip through both is a real test.

// GPT's fixed geometry. 128 entries of 128 bytes is what every tool
// writes and every tool expects; the header could declare otherwise,
// but nothing is gained by being the one disk that does.
//
// The array is 16 KiB whatever the block size, so its length IN BLOCKS
// is not a constant: 32 at 512 bytes, 4 at 4096 -- and the first usable
// LBA (34 or 6) and the backup's position follow from it. Both in the
// DEVICE's blocks, as the header records them.
#define GPT_ENTRY_COUNT 128
#define GPT_ENTRY_SIZE 128
#define GPT_ENTRY_BYTES (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
#define GPT_ENTRY_SECTORS (GPT_ENTRY_BYTES / PART_SECTOR_SIZE) // 32, in 512-byte units
#define GPT_PRIMARY_ENTRY_LBA 2
#define GPT_HEADER_SIZE 92
#define GPT_REVISION 0x00010000

static uint32_t gpt_entry_blocks(uint32_t spb) { return GPT_ENTRY_SECTORS / spb; }
static uint32_t gpt_first_usable(uint32_t spb) { return GPT_PRIMARY_ENTRY_LBA + gpt_entry_blocks(spb); }
// The Microsoft Basic Data type GUID --
// EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 in GPT's mixed-endian byte
// order, which is why the bytes below do not read left to right.
//
// toy-os has no type GUID of its own and does not want one: a
// partition type is a hint to OTHER systems about what is inside, and
// a private GUID would only make every other OS's partition tool
// describe a toy-os partition as unknown. This one is the most widely
// recognised "generic filesystem data" answer there is -- Windows,
// Linux and macOS all understand it, where the Linux-filesystem GUID
// (0FC63DAF-...) is understood by one of the three.
static const uint8_t GPT_TYPE_BASIC_DATA[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7,
};

// THE TWO TYPES THAT BELONG TO THE FIRMWARE, not to whatever OS is on
// the disk: a BIOS boot partition (21686148-...), which holds a
// bootloader image and no filesystem at all, and an EFI System
// Partition (C12A7328-...), which holds /boot. Both are on toy-os's own
// disk now -- GRUB's core.img lives in the first and the kernel in the
// second (tools/install_grub.py) -- and neither is ever somewhere to
// mount a root filesystem or, much worse, somewhere to FORMAT one.
//
// Linux installers make the same distinction and for the same reason;
// offering the ESP as a root target is how you destroy a machine's
// ability to boot in one command.
static const uint8_t GPT_TYPE_BIOS_BOOT[16] = {
    0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E,
    0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49,
};
static const uint8_t GPT_TYPE_ESP[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
};

int partition_is_firmware(const struct partition_entry *pe,
                          enum partition_table_kind kind) {
    if (!pe) return 0;
    if (kind == PART_TABLE_GPT) {
        return k_memcmp(pe->gpt_type_guid, GPT_TYPE_BIOS_BOOT, 16) == 0 ||
               k_memcmp(pe->gpt_type_guid, GPT_TYPE_ESP, 16) == 0;
    }
    return pe->mbr_type == 0xEF;   // MBR's EFI System type
}

int partition_is_esp(const struct partition_entry *pe,
                     enum partition_table_kind kind) {
    if (!pe) return 0;
    if (kind == PART_TABLE_GPT) return k_memcmp(pe->gpt_type_guid, GPT_TYPE_ESP, 16) == 0;
    return pe->mbr_type == 0xEF;
}

// The sectors a table needs for ITSELF, and which no partition may
// overlap, in 512-byte units. MBR: block 0. GPT: blocks 0 through the
// primary entry array at the front, and the backup array plus backup
// header at the back -- 34 and 33 sectors at 512 bytes, 48 and 40 at 4K.
static void reserved_span(enum partition_table_kind kind, uint32_t spb,
                          uint32_t *front, uint32_t *back) {
    if (kind == PART_TABLE_GPT) {
        *front = gpt_first_usable(spb) * spb;
        *back = (gpt_entry_blocks(spb) + 1) * spb;
    } else {
        *front = spb;
        *back = 0;
    }
}

// Every refusal the kernel can make on its own, before a byte is
// written. A parser REJECTS rather than guesses; a WRITER refuses
// rather than writing something it would then refuse to read.
//
// `*why` is set to a short reason for the caller to log. Returns 1 if
// the table is safe to write.
int partition_validate_on(const struct block_device *dev,
                          const struct partition_table *in, const char **why) {
    uint64_t disk = blkdev_sector_count(dev);
    if (!disk) { *why = "no disk"; return 0; }

    if (in->kind != PART_TABLE_MBR && in->kind != PART_TABLE_GPT) {
        *why = "not an MBR or GPT table"; return 0;
    }
    if (in->entry_count < 1 || in->entry_count > PART_WRITE_MAX_ENTRIES) {
        *why = "entry count out of range"; return 0;
    }
    // MBR has exactly four primary slots. Extended partitions are the
    // workaround for that and are deliberately not implemented: they
    // are a linked list of sectors scattered through the disk, and GPT
    // is the answer to wanting more than four.
    if (in->kind == PART_TABLE_MBR && in->entry_count > 4) {
        *why = "MBR holds at most 4 partitions -- use GPT"; return 0;
    }

    uint32_t spb = blkdev_block_sectors(dev);
    uint32_t front, back;
    reserved_span(in->kind, spb, &front, &back);
    if ((uint64_t)front + (uint64_t)back >= (uint64_t)disk) {
        *why = "disk too small for this table"; return 0;
    }

    for (int i = 0; i < in->entry_count; i++) {
        const struct partition_entry *a = &in->entries[i];
        uint64_t a_start, a_count;
        if (in->kind == PART_TABLE_GPT) {
            if (a->gpt_lba_end < a->gpt_lba_start) { *why = "partition ends before it starts"; return 0; }
            a_start = a->gpt_lba_start;
            a_count = a->gpt_lba_end - a->gpt_lba_start + 1; // GPT's range is INCLUSIVE
        } else {
            a_start = a->mbr_lba_start;
            a_count = a->mbr_num_sectors;
        }

        if (a_count == 0) { *why = "empty partition"; return 0; }
        if ((a_start | a_count) & (spb - 1)) { *why = "partition is not whole blocks of this disk"; return 0; }
        if (a_start < front) { *why = "partition overlaps the table itself"; return 0; }
        if (a_start + a_count > (uint64_t)disk - back) { *why = "partition runs past the end of the disk"; return 0; }

        for (int j = 0; j < i; j++) {
            const struct partition_entry *b = &in->entries[j];
            uint64_t b_start, b_count;
            if (in->kind == PART_TABLE_GPT) {
                b_start = b->gpt_lba_start;
                b_count = b->gpt_lba_end - b->gpt_lba_start + 1;
            } else {
                b_start = b->mbr_lba_start;
                b_count = b->mbr_num_sectors;
            }
            // Half-open overlap test. Written out rather than as a
            // clever one-liner because getting it backwards passes
            // every test with no partitions adjacent.
            if (a_start < b_start + b_count && b_start < a_start + a_count) {
                *why = "partitions overlap each other"; return 0;
            }
        }
    }
    return 1;
}

// LBA 0 for an MBR table. PRESERVES bytes 0..445 -- the table lives in
// bytes 446..511 and the signature in the last two, so everything a
// boot sector might hold in front of it survives.
//
// Nothing in this OS puts anything there: TFS3 never touches volume
// blocks 0-7, and the format that DID keep its superblock in bytes
// 0..4 (TFS2) is gone. Preserved anyway, because writing a partition
// table is not a licence to zero a sector this code does not own, and
// a disk written elsewhere may well have boot code in it.
// tools/mkpart_test.py does the same.
static __attribute__((noinline)) int write_mbr(const struct block_device *dev,
                                               const struct partition_table *in, int protective) {
    uint32_t spb = blkdev_block_sectors(dev);
    uint8_t sec[PART_SECTOR_SIZE];
    if (!blkdev_read_partial(dev, 0, 1, sec)) k_memset(sec, 0, sizeof(sec));

    k_memset(sec + MBR_ENTRY_TABLE_OFFSET, 0, MBR_ENTRY_SIZE * MBR_ENTRY_COUNT);

    if (protective) {
        // One entry covering the whole disk, type 0xEE, starting at LBA
        // 1 -- the marker that says "the real table is the GPT, do not
        // treat this disk as unpartitioned". Clamped to 0xFFFFFFFF
        // because that is all an MBR field can hold, which is exactly
        // why GPT exists.
        uint64_t n = (uint64_t)blkdev_sector_count(dev) / spb - 1;
        if (n > 0xFFFFFFFFull) n = 0xFFFFFFFFull;
        uint8_t *e = sec + MBR_ENTRY_TABLE_OFFSET;
        e[4] = MBR_TYPE_GPT_PROTECTIVE;
        write_le32(e + 8, 1);
        write_le32(e + 12, (uint32_t)n);
    } else {
        for (int i = 0; i < in->entry_count; i++) {
            uint8_t *e = sec + MBR_ENTRY_TABLE_OFFSET + i * MBR_ENTRY_SIZE;
            e[0] = in->entries[i].mbr_active ? 0x80 : 0x00;   // the boot indicator
            e[4] = in->entries[i].mbr_type ? in->entries[i].mbr_type : 0x83; // 0x83 = Linux data, the sane default
            write_le32(e + 8, in->entries[i].mbr_lba_start / spb);
            write_le32(e + 12, in->entries[i].mbr_num_sectors / spb);
            // CHS fields left zero. They are meaningless on any disk
            // this century and every LBA-aware reader ignores them;
            // faking a geometry would be inventing a fact.
        }
    }

    sec[MBR_SIGNATURE_OFFSET] = 0x55;
    sec[MBR_SIGNATURE_OFFSET + 1] = 0xAA;
    return blkdev_write_partial(dev, 0, 1, sec);
}

// Writes the 16 KiB entry array at block `lba` and returns its CRC32 --
// built and hashed one 512-byte sector at a time, because a kernel stack
// is 16 KiB too. The array is whole blocks at either size, so on a 4K
// disk the partial writes rewrite nothing but the array itself. Returns 0 on a write failure,
// which is indistinguishable from a legitimate CRC of 0; `*ok` carries
// the real answer.
static __attribute__((noinline)) uint32_t write_gpt_entries(const struct block_device *dev,
                                                            const struct partition_table *in,
                                                            uint64_t lba, int *ok) {
    uint32_t spb = blkdev_block_sectors(dev);
    uint32_t crc = KCRC32_INIT;
    *ok = 1;
    for (int s = 0; s < GPT_ENTRY_SECTORS; s++) {
        uint8_t sec[PART_SECTOR_SIZE];
        k_memset(sec, 0, sizeof(sec));

        const int per_sector = PART_SECTOR_SIZE / GPT_ENTRY_SIZE; // 4
        for (int i = 0; i < per_sector; i++) {
            int idx = s * per_sector + i;
            if (idx >= in->entry_count) break;
            const struct partition_entry *pe = &in->entries[idx];
            uint8_t *e = sec + i * GPT_ENTRY_SIZE;

            k_memcpy(e, pe->gpt_type_guid, 16);
            k_memcpy(e + 16, pe->gpt_unique_guid, 16);
            write_le64(e + 32, pe->gpt_lba_start / spb);
            write_le64(e + 40, (pe->gpt_lba_end + 1) / spb - 1);   // inclusive
            // attributes (e + 48) left zero: no required-partition
            // flag, no legacy-BIOS-bootable flag. A BIOS boot off one
            // of these reaches GRUB through the MBR gap and the BIOS
            // boot partition, neither of which reads this field.

            // The name, ASCII widened to UTF-16LE. The reverse of
            // gpt_name_to_ascii(); this kernel has no Unicode, so the
            // round trip is exact for what it can represent and there
            // is nothing it can represent that it cannot write.
            for (int c = 0; c < 36 && pe->gpt_name[c]; c++) {
                e[56 + c * 2] = (uint8_t)pe->gpt_name[c];
                e[56 + c * 2 + 1] = 0;
            }
        }

        crc = kcrc32_update(crc, sec, PART_SECTOR_SIZE);
        if (!blkdev_write_partial(dev, lba * spb + (uint32_t)s, 1, sec)) { *ok = 0; return 0; }
    }
    return KCRC32_FINAL(crc);
}

// One GPT header. `self` is the block it lives at, `other` its twin's,
// `entry_lba` where ITS copy of the entry array starts -- the primary
// and backup headers differ in exactly those three fields plus their
// own CRC, which is why this is one function called twice rather than
// two nearly-identical ones. All in the device's blocks.
//
// The WHOLE block is written, the header's sector followed by zeros:
// the spec reserves the rest of the header's block as zero, and a
// partial write would keep whatever the disk held there.
static __attribute__((noinline)) int write_gpt_header(const struct block_device *dev,
                                                      uint64_t self, uint64_t other,
                                                      uint64_t entry_lba, uint32_t entries_crc,
                                                      const uint8_t *disk_guid, uint64_t disk_blocks) {
    uint32_t spb = blkdev_block_sectors(dev);
    uint8_t *sec = kmalloc(spb * PART_SECTOR_SIZE);
    if (!sec) return 0;
    k_memset(sec, 0, spb * PART_SECTOR_SIZE);

    k_memcpy(sec, GPT_SIGNATURE, 8);
    write_le32(sec + 8, GPT_REVISION);
    write_le32(sec + 12, GPT_HEADER_SIZE);
    write_le32(sec + 16, 0); // header CRC, computed over this field as zero
    write_le64(sec + 24, self);
    write_le64(sec + 32, other);
    write_le64(sec + 40, gpt_first_usable(spb));
    write_le64(sec + 48, (uint64_t)disk_blocks - 1 - gpt_entry_blocks(spb) - 1); // last usable
    k_memcpy(sec + 56, disk_guid, 16);
    write_le64(sec + 72, entry_lba);
    write_le32(sec + 80, GPT_ENTRY_COUNT);
    write_le32(sec + 84, GPT_ENTRY_SIZE);
    write_le32(sec + 88, entries_crc);

    write_le32(sec + 16, kcrc32(sec, GPT_HEADER_SIZE));
    int ok = blkdev_write_sectors(dev, self * spb, (int)spb, sec);
    kfree(sec);
    return ok;
}

int partition_write_table_of(const struct block_device *dev, const struct partition_table *in) {
    const char *why = "";
    if (!dev) { klog_write(KLOG_ERR "partition: refusing to write -- no such disk\n"); return 0; }
    if (!partition_validate_on(dev, in, &why)) {
        klog_printf(KLOG_ERR "partition: refusing to write -- %s\n", why);
        return 0;
    }

    uint64_t disk = blkdev_sector_count(dev);

    if (in->kind == PART_TABLE_MBR) {
        if (!write_mbr(dev, in, 0)) { klog_write(KLOG_ERR "partition: MBR write failed\n"); return 0; }
        blkdev_flush(dev);
        return 1;
    }

    // GPT. ORDER MATTERS: the backup goes down first, then the primary,
    // then the protective MBR last. A power cut partway therefore
    // leaves a disk that still reads as UNPARTITIONED (no 0xEE entry at
    // LBA 0 yet) rather than one advertising a table whose header was
    // never written -- the same publish-last discipline the virtio
    // drivers follow with their interrupt enables.
    uint32_t spb = blkdev_block_sectors(dev);
    uint64_t disk_blocks = disk / spb;
    uint64_t backup_hdr = disk_blocks - 1;
    uint64_t backup_entries = backup_hdr - gpt_entry_blocks(spb);

    uint8_t disk_guid[16];
    guid_generate(disk_guid);

    int ok = 0;
    uint32_t crc_backup = write_gpt_entries(dev, in, backup_entries, &ok);
    if (!ok) { klog_write(KLOG_ERR "partition: GPT backup entry array write failed\n"); return 0; }
    uint32_t crc_primary = write_gpt_entries(dev, in, GPT_PRIMARY_ENTRY_LBA, &ok);
    if (!ok) { klog_write(KLOG_ERR "partition: GPT entry array write failed\n"); return 0; }
    // Same bytes, so the two CRCs must agree. If they ever did not, one
    // of the two arrays did not land the way it was built, and writing
    // headers claiming both is how a disk gets a backup that silently
    // does not match.
    if (crc_backup != crc_primary) {
        klog_write(KLOG_ERR "partition: GPT entry arrays disagree -- refusing to write the headers\n");
        return 0;
    }

    if (!write_gpt_header(dev, backup_hdr, GPT_HEADER_LBA, backup_entries, crc_primary, disk_guid, disk_blocks)) {
        klog_write(KLOG_ERR "partition: GPT backup header write failed\n");
        return 0;
    }
    if (!write_gpt_header(dev, GPT_HEADER_LBA, backup_hdr, GPT_PRIMARY_ENTRY_LBA, crc_primary, disk_guid, disk_blocks)) {
        klog_write(KLOG_ERR "partition: GPT header write failed\n");
        return 0;
    }
    if (!write_mbr(dev, in, 1)) { klog_write(KLOG_ERR "partition: protective MBR write failed\n"); return 0; }

    blkdev_flush(dev);
    return 1;
}

// THE BOOT DISK, which is what every caller meant before a table could
// be written anywhere else.
int partition_validate(const struct partition_table *in, const char **why) {
    return partition_validate_on(blk_root_disk(), in, why);
}

int partition_write_table(const struct partition_table *in) {
    return partition_write_table_of(blk_root_disk(), in);
}

// Fills in the fields a caller should not have to invent: a fresh
// unique GUID per entry, and the MBR type byte's sane default. Kept
// out of partition_write_table() so that a caller replaying an
// existing table (a future `parttable --restore`) keeps its GUIDs.
void partition_fill_defaults(struct partition_table *t) {
    for (int i = 0; i < t->entry_count; i++) {
        guid_generate(t->entries[i].gpt_unique_guid);
        // An all-zero type GUID is "unset", which is also what an unused
        // GPT slot looks like -- so filling it in here is what lets a
        // caller state a type without knowing sixteen bytes of it.
        int unset = 1;
        for (int b = 0; b < 16; b++) if (t->entries[i].gpt_type_guid[b]) { unset = 0; break; }
        if (unset) k_memcpy(t->entries[i].gpt_type_guid, GPT_TYPE_BASIC_DATA, 16);
        if (!t->entries[i].mbr_type) t->entries[i].mbr_type = 0x83;
    }
}

int partition_type_guid(enum partition_role role, uint8_t out[16]) {
    switch (role) {
    case PART_ROLE_DATA:      k_memcpy(out, GPT_TYPE_BASIC_DATA, 16); return 1;
    case PART_ROLE_BIOS_BOOT: k_memcpy(out, GPT_TYPE_BIOS_BOOT, 16); return 1;
    case PART_ROLE_ESP:       k_memcpy(out, GPT_TYPE_ESP, 16); return 1;
    }
    return 0;
}
