// Tests for the /etc config reader/writer and, more to the point, for
// the thing that turned out to be missing around it: a setting that
// fails to persist must SAY so.
//
// The bug these come from: `timezone Helsinki` on a filesystem with no
// /etc printed "Timezone set to helsinki." and wrote nothing, because
// the timezone saver ignored etc_config_set()'s return value and three
// sibling savers returned void. Nothing was broken about the writer --
// it correctly reported failure to a caller that did not look. So the
// assertion worth having is not "the writer works", it is "the failure
// is reportable and the four setters report it".

#include "ktest.h"
#include "tmppath.h"
#include "fs.h"
#include "etc_config.h"
#include "etc_config_cases.h"
#include "kfmt.h"   // klog_printf, to name the case that failed
#include "font_config.h"
#include "cursor_config.h"
#include "keyboard_config.h"
#include "keyboard_layout.h"
#include "gfx.h"
#include "vga.h"
#include "fs.h"
#include "string.h"

// Scratch, under /tmp -- deliberately NOT /etc/toyos.conf, which holds
// the live machine's real settings and which a test has no business
// rewriting (tests run inside the booted kernel, see ktest.h).
// Built from the configured directory rather than spelled out
// (api/tmppath.h), so moving scratch is a setting rather than a grep.
static const char *scratch_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "ktest_etc.conf");
    return p;
}
#define SCRATCH scratch_path()

static const char *missing_conf_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "ktest_no_such_file.conf");
    return p;
}
#define MISSING_CONF missing_conf_path()

KTEST("etc_config", "set then get round-trips a value") {
    fs_delete(SCRATCH); // may not exist; a failure here is not interesting
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH, "colour", "amber"), 1);

    char v[16];
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "colour", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "amber"), 0);

    // Replacing a key in place, which is the case the four settings
    // actually exercise -- they rewrite the same key over and over.
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH, "colour", "green"), 1);
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "colour", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "green"), 0);

    fs_delete(SCRATCH);
}

KTEST("etc_config", "unsetting a file's only key empties it rather than refusing") {
    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH, "only", "one"), 1);
    // The rewrite's length is 0 here, which used to read as "absent".
    KTEST_ASSERT_EQ(etc_config_unset(SCRATCH, "only"), 1);
    char v[16];
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "only", v, sizeof v), 0);
    // And a key that genuinely is not there is still refused.
    KTEST_ASSERT_EQ(etc_config_unset(SCRATCH, "only"), 0);
    fs_delete(SCRATCH);
}

KTEST("etc_config", "a write under a missing directory FAILS, and says so") {
    // The exact shape of the original bug: no directory, so the write
    // cannot land. What matters is that this returns 0 rather than
    // quietly doing nothing -- everything below depends on it.
    KTEST_ASSERT_EQ(etc_config_set("/no-such-dir-ktest/x.conf", "k", "v"), 0);
}

KTEST("etc_config", "the four setting savers report SETTING_SAVED on a good /etc") {
    // Each one re-saves the value that is already active, so a pass
    // changes nothing about the running machine -- but it does go all
    // the way through etc_config_set() to the real filesystem, which is
    // the only way to tell "returns a plausible constant" from
    // "actually wrote something".
    KTEST_ASSERT_EQ(font_config_save(gfx_font_size()), SETTING_SAVED);
    KTEST_ASSERT_EQ(cursor_config_save(vga_cursor_style()), SETTING_SAVED);
    KTEST_ASSERT_EQ(keyboard_config_save(keyboard_layout_current()), SETTING_SAVED);
}

// ---- read once, ask many --------------------------------------------

KTEST("etc_config", "a loaded buffer answers the same as a per-call read") {
    // EQUIVALENCE is the property worth pinning. etc_config_buf_get()
    // exists purely so a caller asking one file several questions stops
    // re-reading it (the desktop asked six keys per .desktop entry, so a
    // nine-entry reload did 54 whole-file reads) -- so the day it
    // answers differently from etc_config_get() is the day it is worse
    // than the thing it replaced. Both run one parser for that reason;
    // this is the assertion that they still do.
    fs_delete(SCRATCH);
    KTEST_ASSERT(etc_config_set(SCRATCH, "alpha", "one"));
    KTEST_ASSERT(etc_config_set(SCRATCH, "beta", "two"));

    // static, not automatic: this struct is a KiB, which is over the
    // kernel's per-function frame budget (Makefile's
    // -Wframe-larger-than), and a KTEST body is never reentered.
    static struct etc_config_buf buf;
    KTEST_ASSERT(etc_config_load(SCRATCH, &buf));

    char from_buf[16], from_disk[16];
    KTEST_ASSERT(etc_config_buf_get(&buf, "alpha", from_buf, sizeof from_buf));
    KTEST_ASSERT(etc_config_get(SCRATCH, "alpha", from_disk, sizeof from_disk));
    KTEST_ASSERT_EQ(k_strcmp(from_buf, from_disk), 0);

    KTEST_ASSERT(etc_config_buf_get(&buf, "beta", from_buf, sizeof from_buf));
    KTEST_ASSERT_EQ(k_strcmp(from_buf, "two"), 0);

    // An absent key is 0 with `out` emptied, exactly as the per-call
    // form does it, so callers can use `out` without a second check.
    char none[16];
    KTEST_ASSERT_EQ(etc_config_buf_get(&buf, "missing", none, sizeof none), 0);
    KTEST_ASSERT_EQ(none[0], '\0');

    fs_delete(SCRATCH);
}

KTEST("etc_config", "an unloaded buffer answers nothing rather than garbage") {
    // The failure path callers depend on: a load that fails must make
    // every later get a clean miss, so a caller can load once and check
    // once rather than guarding every key.
    // static, not automatic: this struct is a KiB, which is over the
    // kernel's per-function frame budget (Makefile's
    // -Wframe-larger-than), and a KTEST body is never reentered.
    static struct etc_config_buf buf;
    KTEST_ASSERT_EQ(etc_config_load(MISSING_CONF, &buf), 0);

    char out[16];
    KTEST_ASSERT_EQ(etc_config_buf_get(&buf, "alpha", out, sizeof out), 0);
    KTEST_ASSERT_EQ(out[0], '\0');
}

KTEST("etc_config", "an invalid argument is INVALID, not a failed save") {
    // The three-way answer is the point: "you gave me nonsense" and "I
    // could not write it down" are different outcomes and a caller that
    // merges them cannot word its message correctly.
    KTEST_ASSERT_EQ(cursor_config_save(VGA_CURSOR_STYLE_COUNT), SETTING_INVALID);
    KTEST_ASSERT_EQ(keyboard_config_save(""), SETTING_INVALID);
}

// ---- sections -------------------------------------------------------
//
// The table is shared with the ring-3 build (api/etc_config_cases.h);
// these three walk it here, and userland/tests/etc_config_test.c walks
// the same rows over libuapp.a's second compilation of the parser.
//
// FILE SCOPE, NOT LOCALS: a struct etc_config_buf is 4 KiB and the
// rewrite buffer another 4, which is over the kernel's frame budget on
// its own.
static struct etc_config_buf g_case_buf;
static char g_case_out[ETC_CONFIG_MAX];
static char g_case_got[256];

KTEST("etc_config", "the shared table: reading a section") {
    KTEST_ASSERT(etc_get_case_count >= 10); // an empty table asserts nothing
    for (int i = 0; i < etc_get_case_count; i++) {
        int ok = etc_get_case_run(&etc_get_cases[i], &g_case_buf,
                                  g_case_got, sizeof g_case_got);
        if (!ok) klog_printf("etc_config: get case \"%s\" gave \"%s\"\n",
                             etc_get_cases[i].name, g_case_got);
        KTEST_ASSERT(ok);
    }
}

KTEST("etc_config", "the shared table: writing into a section") {
    KTEST_ASSERT(etc_set_case_count >= 10);
    for (int i = 0; i < etc_set_case_count; i++) {
        int ok = etc_set_case_run(&etc_set_cases[i], g_case_out, sizeof g_case_out,
                                  g_case_got, sizeof g_case_got);
        if (!ok) klog_printf("etc_config: set case \"%s\" gave \"%s\"\n",
                             etc_set_cases[i].name, g_case_got);
        KTEST_ASSERT(ok);
    }
}

KTEST("etc_config", "the shared table: walking the sections") {
    KTEST_ASSERT(etc_sections_case_count >= 4);
    for (int i = 0; i < etc_sections_case_count; i++) {
        int ok = etc_sections_case_run(&etc_sections_cases[i], &g_case_buf,
                                       g_case_got, sizeof g_case_got);
        if (!ok) klog_printf("etc_config: sections case \"%s\" gave \"%s\"\n",
                             etc_sections_cases[i].name, g_case_got);
        KTEST_ASSERT(ok);
    }
}

// The cases above are buffer-to-buffer. This one goes through the FILE
// half, which is a different set of entry points and the one a setting
// actually persists through.
KTEST("etc_config", "a sectioned file round-trips through the disk") {
    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(etc_config_set_in(SCRATCH, "ipv4", "method", "dhcp"), 1);
    KTEST_ASSERT_EQ(etc_config_set_in(SCRATCH, "ipv4", "mtu", "1400"), 1);
    KTEST_ASSERT_EQ(etc_config_set_in(SCRATCH, "ipv6", "method", "off"), 1);
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH, "version", "1"), 1);

    char v[16];
    KTEST_ASSERT_EQ(etc_config_get_in(SCRATCH, "ipv4", "method", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "dhcp"), 0);
    KTEST_ASSERT_EQ(etc_config_get_in(SCRATCH, "ipv6", "method", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "off"), 0);
    // The top-level key is not in either section, and neither section's
    // key leaks to the top level.
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "version", v, sizeof v), 1);
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "method", v, sizeof v), 0);
    KTEST_ASSERT_EQ(etc_config_get_in(SCRATCH, "ipv4", "version", v, sizeof v), 0);

    // ...and removing one leaves its neighbour alone.
    KTEST_ASSERT_EQ(etc_config_unset_in(SCRATCH, "ipv4", "mtu"), 1);
    KTEST_ASSERT_EQ(etc_config_get_in(SCRATCH, "ipv4", "mtu", v, sizeof v), 0);
    KTEST_ASSERT_EQ(etc_config_get_in(SCRATCH, "ipv4", "method", v, sizeof v), 1);
    KTEST_ASSERT_EQ(etc_config_unset_in(SCRATCH, "ipv4", "mtu"), 0); // gone already

    fs_delete(SCRATCH);
}
