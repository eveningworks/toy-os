// Tests for blk_dsm_pack() (block.h): the DATA SET MANAGEMENT payload
// ata.c and ahci.c both send. A packing mistake is silent -- the drive
// acknowledges a range list it reads differently, and discards the
// wrong sectors or none -- so the layout is checked byte for byte.
#include "ktest.h"
#include "block.h"

static uint32_t entry_lba(const uint8_t *b, int e) {
    const uint8_t *p = b + e * 8;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t entry_count(const uint8_t *b, int e) {
    return (uint32_t)b[e * 8 + 6] | ((uint32_t)b[e * 8 + 7] << 8);
}

KTEST("block", "DSM packing splits a long run at 65535 sectors") {
    static uint8_t blk[512];
    struct blk_range r[1] = { { 1000, 70000 } };
    int ri = 0;
    uint32_t done = 0;
    int e = blk_dsm_pack(blk, r, 1, &ri, &done);
    KTEST_ASSERT_EQ(e, 2);
    KTEST_ASSERT_EQ(entry_lba(blk, 0), 1000u);
    KTEST_ASSERT_EQ(entry_count(blk, 0), 65535u);
    KTEST_ASSERT_EQ(entry_lba(blk, 1), 1000u + 65535u);
    KTEST_ASSERT_EQ(entry_count(blk, 1), 70000u - 65535u);
    KTEST_ASSERT_EQ(entry_count(blk, 2), 0u);   // a zero count ends the list
    KTEST_ASSERT_EQ(blk_dsm_pack(blk, r, 1, &ri, &done), 0);
}

KTEST("block", "DSM packing fills 64 entries a block and resumes where it stopped") {
    static uint8_t blk[512];
    static struct blk_range r[70];
    for (int i = 0; i < 70; i++) r[i] = (struct blk_range){ (uint32_t)i * 100u, 8 };
    int ri = 0;
    uint32_t done = 0;
    KTEST_ASSERT_EQ(blk_dsm_pack(blk, r, 70, &ri, &done), BLK_DSM_ENTRIES);
    KTEST_ASSERT_EQ(entry_lba(blk, 63), 6300u);
    KTEST_ASSERT_EQ(blk_dsm_pack(blk, r, 70, &ri, &done), 6);
    KTEST_ASSERT_EQ(entry_lba(blk, 0), 6400u);   // the 65th run, not a repeat
    KTEST_ASSERT_EQ(entry_lba(blk, 5), 6900u);
    KTEST_ASSERT_EQ(entry_count(blk, 6), 0u);
    KTEST_ASSERT_EQ(blk_dsm_pack(blk, r, 70, &ri, &done), 0);
}
