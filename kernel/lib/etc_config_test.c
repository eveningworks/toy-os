// Tests for the /etc config reader/writer and, more to the point, for
// the thing that turned out to be missing around it: a setting that
// fails to persist must SAY so.
//
// The bug these come from: `timezone Helsinki` on a filesystem with no
// /etc printed "Timezone set to helsinki." and wrote nothing, because
// tz_set_index() ignored etc_config_set()'s return value and three
// sibling savers returned void. Nothing was broken about the writer --
// it correctly reported failure to a caller that did not look. So the
// assertion worth having is not "the writer works", it is "the failure
// is reportable and the four setters report it".

#include "ktest.h"
#include "etc_config.h"
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
#define SCRATCH "/tmp/ktest_etc.conf"

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

    struct etc_config_buf buf;
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
    struct etc_config_buf buf;
    KTEST_ASSERT_EQ(etc_config_load("/tmp/ktest_no_such_file.conf", &buf), 0);

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
