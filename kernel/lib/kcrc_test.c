// Tests for kcrc.c. The one-shot vectors are the published check values
// for this CRC; the split-update case is the property partition.c
// depends on, since it CRCs a 16 KiB GPT entry array a sector at a time
// and has no buffer to hold the whole of it.
#include "ktest.h"
#include "kcrc.h"

KTEST("kcrc", "the published CRC-32 check values") {
    // The standard check vector for CRC-32/ISO-HDLC.
    KTEST_ASSERT_EQ(kcrc32("123456789", 9), 0xCBF43926u);
    // Empty input is the initial value, inverted.
    KTEST_ASSERT_EQ(kcrc32("", 0), 0u);
    KTEST_ASSERT_EQ(kcrc32("a", 1), 0xE8B7BE43u);
    // A NUL byte is data, not a terminator.
    KTEST_ASSERT_EQ(kcrc32("\0", 1), 0xD202EF8Du);
}

KTEST("kcrc", "a split update equals the one-shot") {
    uint32_t crc = KCRC32_INIT;
    crc = kcrc32_update(crc, "1234", 4);
    crc = kcrc32_update(crc, "5", 1);
    crc = kcrc32_update(crc, "6789", 4);
    KTEST_ASSERT_EQ(KCRC32_FINAL(crc), kcrc32("123456789", 9));

    // ...including when a chunk is empty, which is what a short read
    // off the end of a file looks like.
    crc = kcrc32_update(KCRC32_INIT, "123456789", 9);
    crc = kcrc32_update(crc, "", 0);
    KTEST_ASSERT_EQ(KCRC32_FINAL(crc), 0xCBF43926u);
}
