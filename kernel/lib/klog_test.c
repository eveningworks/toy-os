// Log levels: the in-band marker, what reaches the ring, and what the
// console threshold lets past.
//
// These write into the LIVE ring, so nothing here asserts an absolute
// offset -- only the relationship between what was written and what
// comes back from the offset recorded just before it. Anything else is
// true until the next driver logs a line.
#include "ktest.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "scheduler.h"

// The ring from `from` to the end, as a NUL-terminated string.
static uint32_t slice(uint64_t from, char *out, uint32_t cap) {
    uint64_t first = 0;
    uint32_t n = 0;
    while (n + 1 < cap) {
        uint32_t got = klog_read(from + n, out + n, cap - 1 - n, &first);
        if (!got) break;
        n += got;
    }
    out[n] = 0;
    return n;
}

KTEST("klog", "a level marker sets the line's level and never reaches the ring") {
    char buf[256];
    scheduler_preempt_disable();
    uint64_t before = klog_total_bytes();
    klog_write(KLOG_ERR "klog-ktest: a failure\n");
    uint32_t n = slice(before, buf, sizeof buf);
    scheduler_preempt_enable();

    KTEST_ASSERT(n > 0);
    // THE MARKER IS THE ONE BYTE THAT MUST NOT SURVIVE: a raw \001 in
    // the ring corrupts the only copy of the evidence.
    KTEST_ASSERT(k_strchr(buf, '\001') == 0);
    KTEST_ASSERT(k_strstr(buf, "klog-ktest: a failure") != 0);
    // It comes back as text in the stamp instead.
    KTEST_ASSERT(k_strstr(buf, "<3> klog-ktest") != 0);
}

KTEST("klog", "an unmarked line is info, and says so") {
    char buf[256];
    scheduler_preempt_disable();
    uint64_t before = klog_total_bytes();
    klog_write("klog-ktest: ordinary\n");
    slice(before, buf, sizeof buf);
    scheduler_preempt_enable();

    KTEST_ASSERT(k_strstr(buf, "<6> klog-ktest: ordinary") != 0);
}

KTEST("klog", "a marker on a later fragment is stripped, not stored") {
    char buf[256];
    scheduler_preempt_disable();
    uint64_t before = klog_total_bytes();
    klog_write("klog-ktest: opened");
    klog_write(KLOG_ERR " and continued\n");   // wrong place for a level
    slice(before, buf, sizeof buf);
    scheduler_preempt_enable();

    KTEST_ASSERT(k_strchr(buf, '\001') == 0);
    KTEST_ASSERT(k_strstr(buf, "klog-ktest: opened and continued") != 0);
    // The level belongs to the line, which was already info by the time
    // the second fragment arrived.
    KTEST_ASSERT(k_strstr(buf, "<6> klog-ktest: opened") != 0);
}

KTEST("klog", "the level lasts one line") {
    char buf[256];
    scheduler_preempt_disable();
    uint64_t before = klog_total_bytes();
    klog_write(KLOG_WARN "klog-ktest: warned\n");
    klog_write("klog-ktest: after\n");
    slice(before, buf, sizeof buf);
    scheduler_preempt_enable();

    KTEST_ASSERT(k_strstr(buf, "<4> klog-ktest: warned") != 0);
    KTEST_ASSERT(k_strstr(buf, "<6> klog-ktest: after") != 0);
}

KTEST("klog", "the ring keeps what the console threshold drops") {
    char buf[256];
    int saved = klog_console_level();
    scheduler_preempt_disable();
    klog_set_console_level(KLOG_LEVEL_ERR);   // debug is well below this
    uint64_t before = klog_total_bytes();
    klog_write(KLOG_DEBUG "klog-ktest: chatter\n");
    slice(before, buf, sizeof buf);
    klog_set_console_level(saved);
    scheduler_preempt_enable();

    // THE THRESHOLD IS THE CONSOLE'S, NOT THE LOG'S. A level that
    // dropped bytes from the ring would be discarding the evidence a
    // fault needs, which is the opposite of why the ring exists.
    KTEST_ASSERT(k_strstr(buf, "<7> klog-ktest: chatter") != 0);
}

KTEST("klog", "a level outside the scale is clamped, not stored") {
    int saved = klog_console_level();
    klog_set_console_level(99);
    KTEST_ASSERT_EQ(klog_console_level(), KLOG_LEVEL_DEBUG);
    klog_set_console_level(-4);
    KTEST_ASSERT_EQ(klog_console_level(), KLOG_LEVEL_CRIT);
    klog_set_console_level(saved);
}

KTEST("klog", "loglevel= is read off a command line, and nothing else is") {
    int saved = klog_console_level();
    klog_apply_cmdline("nokaslr loglevel=4 novirtio");
    KTEST_ASSERT_EQ(klog_console_level(), KLOG_LEVEL_WARN);
    klog_apply_cmdline("nokaslr novirtio");         // no flag: unchanged
    KTEST_ASSERT_EQ(klog_console_level(), KLOG_LEVEL_WARN);
    klog_apply_cmdline("loglevel=x");               // not a digit: unchanged
    KTEST_ASSERT_EQ(klog_console_level(), KLOG_LEVEL_WARN);
    klog_set_console_level(saved);
}
