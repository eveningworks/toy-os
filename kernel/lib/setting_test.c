// Tests for the settings registry (setting.h) and for the SYS_SETTING
// message it answers (abi/setting_abi.h).
//
// Almost everything here drives a SCRATCH setting registered by the
// test itself, persisting to /tmp rather than /etc/toyos.conf -- these
// run inside the live booted kernel (ktest.h), so a test that applied a
// real font size or timezone would be rewriting the running machine's
// own settings, which etc_config_test.c already declines to do for the
// same reason. The four real settings are only ever READ here.
//
// What that buys beyond politeness: the scratch setting can be made to
// FAIL on demand, which is the only way to assert that a rejected value
// changes nothing and that SETTING_UNSAVED survives the round trip --
// the real settings are all well-behaved, so against them "it applied"
// and "it was accepted" are indistinguishable.

#include "ktest.h"
#include "setting.h"
#include "setting_abi.h"
#include "etc_config.h"
#include "string.h"
#include "fs.h"
#include "gfx.h"
#include "font_config.h"

#define SCRATCH_FILE "/tmp/ktest_setting.conf"
#define SCRATCH_NAME "ktest_colour"

static const char *const g_scratch_choices[] = { "amber", "green", "white" };
#define SCRATCH_CHOICES 3

static char g_scratch_value[SETTING_VALUE_MAX] = "amber";
static int g_scratch_applies = 1; // 0 makes apply() reject everything
static int g_scratch_apply_calls;  // how many times apply() actually ran

static int scratch_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= SCRATCH_CHOICES) return 0;
    k_strlcpy(out, g_scratch_choices[index], out_size);
    return 1;
}

static void scratch_get(char *out, uint32_t out_size) {
    k_strlcpy(out, g_scratch_value, out_size);
}

static int scratch_apply(const char *value) {
    g_scratch_apply_calls++;
    if (!g_scratch_applies) return SETTING_INVALID;
    for (int i = 0; i < SCRATCH_CHOICES; i++) {
        if (k_strcmp(value, g_scratch_choices[i]) != 0) continue;
        k_strlcpy(g_scratch_value, value, sizeof g_scratch_value);
        return etc_config_set(SCRATCH_FILE, SCRATCH_NAME, value)
                   ? SETTING_SAVED : SETTING_UNSAVED;
    }
    return SETTING_INVALID;
}

static const struct setting g_scratch = {
    .name   = SCRATCH_NAME,
    .label  = "Scratch colour",
    .type   = SETTING_TYPE_ENUM,
    .file   = SCRATCH_FILE,
    .choice = scratch_choice,
    .get    = scratch_get,
    .apply  = scratch_apply,
};

// Registration is per-test rather than once, because a KTEST must
// establish its own preconditions rather than inherit them from
// whatever ran before (ktest.h) -- and a `ktest setting` run of one
// test has to work as well as a run of all of them.
static void scratch_begin(void) {
    setting_unregister(SCRATCH_NAME); // in case a previous test left it
    k_strlcpy(g_scratch_value, "amber", sizeof g_scratch_value);
    g_scratch_applies = 1;
    g_scratch_apply_calls = 0;
    fs_delete(SCRATCH_FILE); // may not exist
    setting_register(&g_scratch);
}

static void scratch_end(void) {
    setting_unregister(SCRATCH_NAME);
    fs_delete(SCRATCH_FILE);
}

// --- registration ----------------------------------------------------

KTEST("setting", "the kernel's own settings are registered") {
    // Not an exact count: the registry is meant to grow, and a test
    // that pins the number turns every future registration into a
    // failure in an unrelated file.
    KTEST_ASSERT(setting_count() >= 4);
    KTEST_ASSERT(setting_find("timezone") != 0);
    KTEST_ASSERT(setting_find("font_size") != 0);
    KTEST_ASSERT(setting_find("cursor_style") != 0);
    KTEST_ASSERT(setting_find("keyboard_layout") != 0);
    KTEST_ASSERT(setting_find("no_such_setting") == 0);
}

KTEST("setting", "every registered setting reports a value and a file") {
    // The property a settings UI depends on: a row it can draw. A
    // setting with no current value renders as an empty control that
    // looks broken, and one with no file cannot answer "where is this
    // stored?" -- the question the registry exists to answer.
    char v[SETTING_VALUE_MAX];
    for (int i = 0; i < setting_count(); i++) {
        const struct setting *s = setting_at(i);
        KTEST_ASSERT(s != 0);
        KTEST_ASSERT(s->file != 0 && s->file[0] == '/');
        v[0] = '\0';
        s->get(v, sizeof v);
        KTEST_ASSERT(v[0] != '\0');
    }
}

KTEST("setting", "a duplicate name is refused, not shadowed") {
    scratch_begin();
    int before = setting_count();
    KTEST_ASSERT_EQ(setting_register(&g_scratch), 0);
    KTEST_ASSERT_EQ(setting_count(), before);
    scratch_end();
}

KTEST("setting", "a malformed descriptor is refused at registration") {
    // An ENUM with no choice enumerator would draw an empty picker --
    // a control that appears and cannot be used. Caught here rather
    // than discovered in a UI.
    static const struct setting bad_enum = {
        .name = "ktest_bad_enum", .label = "Bad", .type = SETTING_TYPE_ENUM,
        .file = SCRATCH_FILE, .choice = 0, .get = scratch_get, .apply = 0,
    };
    static const struct setting no_get = {
        .name = "ktest_no_get", .label = "Bad", .type = SETTING_TYPE_STRING,
        .file = SCRATCH_FILE, .choice = 0, .get = 0, .apply = 0,
    };
    KTEST_ASSERT_EQ(setting_register(&bad_enum), 0);
    KTEST_ASSERT_EQ(setting_register(&no_get), 0);
    KTEST_ASSERT(setting_find("ktest_bad_enum") == 0);
    KTEST_ASSERT(setting_find("ktest_no_get") == 0);
}

// --- get / set -------------------------------------------------------

KTEST("setting", "set applies, persists, and bumps the generation") {
    scratch_begin();
    uint32_t gen = setting_generation();

    KTEST_ASSERT_EQ(setting_set(SCRATCH_NAME, "green"), SETTING_SAVED);

    // All three halves, separately: the live value moved, the FILE has
    // it, and the generation moved. Asserting only the first is
    // satisfied by a setting that applies and never persists, which is
    // precisely the bug enum setting_result exists to expose.
    char v[SETTING_VALUE_MAX];
    KTEST_ASSERT_EQ(setting_get(SCRATCH_NAME, v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "green"), 0);
    v[0] = '\0';
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH_FILE, SCRATCH_NAME, v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "green"), 0);
    KTEST_ASSERT(setting_generation() != gen);

    scratch_end();
}

KTEST("setting", "a rejected value changes nothing at all") {
    scratch_begin();
    KTEST_ASSERT_EQ(setting_set(SCRATCH_NAME, "green"), SETTING_SAVED);
    uint32_t gen = setting_generation();

    KTEST_ASSERT_EQ(setting_set(SCRATCH_NAME, "puce"), SETTING_INVALID);

    // The generation must NOT move on a refusal, or every client
    // re-reads the whole registry each time someone mistypes.
    KTEST_ASSERT_EQ(setting_generation(), gen);
    char v[SETTING_VALUE_MAX];
    KTEST_ASSERT_EQ(setting_get(SCRATCH_NAME, v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "green"), 0);
    scratch_end();
}

KTEST("setting", "an unregistered name is invalid, not a silent success") {
    char v[SETTING_VALUE_MAX];
    KTEST_ASSERT_EQ(setting_set("no_such_setting", "x"), SETTING_INVALID);
    KTEST_ASSERT_EQ(setting_get("no_such_setting", v, sizeof v), 0);
    KTEST_ASSERT_EQ(v[0], '\0'); // documented: emptied, not left stale
}

KTEST("setting", "an over-long value is refused rather than truncated") {
    // A parser rejects rather than guesses, and a formatter that does
    // not fit writes nothing (CLAUDE.md's toolkit conventions). A
    // truncated setting value is a DIFFERENT setting, silently.
    scratch_begin();
    char big[SETTING_VALUE_MAX + 8];
    for (unsigned i = 0; i < sizeof big - 1; i++) big[i] = 'a';
    big[sizeof big - 1] = '\0';
    KTEST_ASSERT_EQ(setting_set(SCRATCH_NAME, big), SETTING_INVALID);
    scratch_end();
}

// --- reload ----------------------------------------------------------

KTEST("setting", "reload re-applies what a hand edit put in the file") {
    // The whole point of settings staying plain text: someone edits
    // /etc with `edit`, and the live value has to catch up. Simulated
    // by writing the file behind the registry's back, which is exactly
    // what a text editor does to it.
    scratch_begin();
    KTEST_ASSERT_EQ(setting_set(SCRATCH_NAME, "green"), SETTING_SAVED);

    KTEST_ASSERT_EQ(etc_config_set(SCRATCH_FILE, SCRATCH_NAME, "white"), 1);
    char v[SETTING_VALUE_MAX];
    KTEST_ASSERT_EQ(setting_get(SCRATCH_NAME, v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "green"), 0); // stale, as expected

    settings_reload();
    KTEST_ASSERT_EQ(setting_get(SCRATCH_NAME, v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "white"), 0);
    scratch_end();
}

KTEST("setting", "reload reports a hand edit the owner rejects") {
    scratch_begin();
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH_FILE, SCRATCH_NAME, "puce"), 1);

    // Counted and reported, not silently ignored -- a typo in a file
    // someone just edited must not look like the setting simply not
    // working. And the previous working value survives.
    KTEST_ASSERT(settings_reload() >= 1);
    char v[SETTING_VALUE_MAX];
    KTEST_ASSERT_EQ(setting_get(SCRATCH_NAME, v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "amber"), 0);
    scratch_end();
}

// --- the SYS_SETTING message ----------------------------------------

KTEST("setting", "dispatch enumerates count, info and choices") {
    scratch_begin();
    struct setting_msg m;

    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    KTEST_ASSERT_EQ(setting_dispatch(&m), 1);
    KTEST_ASSERT_EQ(m.count, setting_count());

    // Find the scratch row by name rather than assuming its index --
    // registration order is stable but the four real settings sit in
    // front of it, and pinning that is a test that breaks on an
    // unrelated addition.
    int row = -1;
    for (int i = 0; i < setting_count(); i++) {
        if (k_strcmp(setting_at(i)->name, SCRATCH_NAME) == 0) { row = i; break; }
    }
    KTEST_ASSERT(row >= 0);

    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_INFO;
    m.index = row;
    KTEST_ASSERT_EQ(setting_dispatch(&m), 1);
    KTEST_ASSERT_EQ(k_strcmp(m.name, SCRATCH_NAME), 0);
    KTEST_ASSERT_EQ(k_strcmp(m.label, "Scratch colour"), 0);
    KTEST_ASSERT_EQ(k_strcmp(m.file, SCRATCH_FILE), 0);
    KTEST_ASSERT_EQ(m.type, (uint32_t)SETTING_ABI_TYPE_ENUM);
    KTEST_ASSERT_EQ(m.count, SCRATCH_CHOICES);
    KTEST_ASSERT_EQ(k_strcmp(m.value, "amber"), 0);

    for (int i = 0; i < SCRATCH_CHOICES; i++) {
        k_memset(&m, 0, sizeof m);
        m.op = SETTING_OP_CHOICE;
        m.index = row;
        m.choice = i;
        KTEST_ASSERT_EQ(setting_dispatch(&m), 1);
        KTEST_ASSERT_EQ(k_strcmp(m.value, g_scratch_choices[i]), 0);
    }

    // One past the end fails, which is how a client that ignored
    // `count` still terminates.
    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_CHOICE;
    m.index = row;
    m.choice = SCRATCH_CHOICES;
    KTEST_ASSERT_EQ(setting_dispatch(&m), 0);

    scratch_end();
}

KTEST("setting", "dispatch set reports its outcome in result, not the return") {
    scratch_begin();
    struct setting_msg m;

    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_SET;
    k_strlcpy(m.name, SCRATCH_NAME, sizeof m.name);
    k_strlcpy(m.value, "white", sizeof m.value);
    KTEST_ASSERT_EQ(setting_dispatch(&m), 1);
    KTEST_ASSERT_EQ(m.result, (uint32_t)SETTING_SAVED);

    // A refused value is still a SUCCESSFUL call -- returning 0 here
    // would leave a client unable to tell "bad ABI" from "bad value".
    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_SET;
    k_strlcpy(m.name, SCRATCH_NAME, sizeof m.name);
    k_strlcpy(m.value, "puce", sizeof m.value);
    KTEST_ASSERT_EQ(setting_dispatch(&m), 1);
    KTEST_ASSERT_EQ(m.result, (uint32_t)SETTING_INVALID);

    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GET;
    k_strlcpy(m.name, SCRATCH_NAME, sizeof m.name);
    KTEST_ASSERT_EQ(setting_dispatch(&m), 1);
    KTEST_ASSERT_EQ(k_strcmp(m.value, "white"), 0);

    scratch_end();
}

KTEST("setting", "dispatch reports the generation on every op") {
    // What a client polls to notice someone else's change. It has to
    // ride an op the client was making anyway, or it is a poll of its
    // own and costs a syscall per frame.
    scratch_begin();
    struct setting_msg m;

    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    KTEST_ASSERT_EQ(setting_dispatch(&m), 1);
    uint32_t before = m.generation;

    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_SET;
    k_strlcpy(m.name, SCRATCH_NAME, sizeof m.name);
    k_strlcpy(m.value, "green", sizeof m.value);
    KTEST_ASSERT_EQ(setting_dispatch(&m), 1);
    // The SETTING call itself must report the POST-change generation,
    // or the caller that made the change concludes nothing happened.
    KTEST_ASSERT(m.generation != before);

    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    KTEST_ASSERT_EQ(setting_dispatch(&m), 1);
    KTEST_ASSERT(m.generation != before);

    scratch_end();
}

KTEST("setting", "dispatch refuses a bad op and a bad index") {
    struct setting_msg m;

    k_memset(&m, 0, sizeof m);
    m.op = 999;
    KTEST_ASSERT_EQ(setting_dispatch(&m), 0);

    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_INFO;
    m.index = setting_count(); // one past the last
    KTEST_ASSERT_EQ(setting_dispatch(&m), 0);

    k_memset(&m, 0, sizeof m);
    m.op = SETTING_OP_INFO;
    m.index = -1;
    KTEST_ASSERT_EQ(setting_dispatch(&m), 0);
}

KTEST("setting", "a real setting's value matches its subsystem") {
    // Read-only, against the live machine: the registry must report
    // what the subsystem actually has, not what the file says. Those
    // agree until someone hand-edits, and reporting the file would make
    // a settings UI show a value that is not in effect.
    char v[SETTING_VALUE_MAX];
    KTEST_ASSERT_EQ(setting_get("font_size", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, gfx_font_size_name(gfx_font_size())), 0);
}

KTEST("setting", "setting a value it already has does no work at all") {
    // Why this matters far beyond tidiness: everything watching
    // setting_generation() does REAL work when it moves -- the desktop
    // re-reads and re-parses every .desktop file, a compositor reloads
    // its cursor theme. So a UI that over-reports a change turns into
    // disk I/O and a desktop-wide reload, and one that does it per
    // pointer-motion event freezes the machine for seconds. That is not
    // hypothetical; it is the bug this check was written after.
    scratch_begin();

    // First set is real: the live value already matches ("amber"), but
    // the FILE has no key yet, so it must still be written. Skipping on
    // the live value alone would leave a fresh disk with nothing saved.
    KTEST_ASSERT_EQ(setting_set(SCRATCH_NAME, "amber"), SETTING_SAVED);
    KTEST_ASSERT(g_scratch_apply_calls >= 1);

    int applies = g_scratch_apply_calls;
    uint32_t gen = setting_generation();

    // Second set of the same value: still SAVED (it IS saved), but
    // nothing ran and nobody was told.
    KTEST_ASSERT_EQ(setting_set(SCRATCH_NAME, "amber"), SETTING_SAVED);
    KTEST_ASSERT_EQ(g_scratch_apply_calls, applies);
    KTEST_ASSERT_EQ((int)setting_generation(), (int)gen);

    // The control: a DIFFERENT value must still apply and still
    // announce itself, or this optimisation has broken settings
    // entirely rather than made them cheap.
    KTEST_ASSERT_EQ(setting_set(SCRATCH_NAME, "green"), SETTING_SAVED);
    KTEST_ASSERT_EQ(g_scratch_apply_calls, applies + 1);
    KTEST_ASSERT(setting_generation() != gen);

    scratch_end();
}
