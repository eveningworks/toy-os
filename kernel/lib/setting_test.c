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
#include "tmppath.h"
#include "klog.h"
#include "kfmt.h"
#include "fs.h"
#include "setting.h"
#include "setting_abi.h"
#include "etc_config.h"
#include "mouse_config.h"
#include "kfmt.h"
#include "string.h"
#include "string.h"
#include "fs.h"
#include "config_file.h" // the namespace comes from a file descriptor
#include "gfx.h"
#include "font_config.h"

// Built from the configured directory rather than spelled out
// (api/tmppath.h), so moving scratch is a setting rather than a grep.
static const char *scratch_file_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "ktest_setting.conf");
    return p;
}
#define SCRATCH_FILE scratch_file_path()
static const char *scratch2_file_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "ktest_setting2.conf");
    return p;
}
#define SCRATCH2_FILE scratch2_file_path()
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

// Drives the `unavailable` half: 0 (the default) leaves the scratch
// setting changeable, non-zero makes it report a reason.
static int g_scratch_unavailable;

static const char *scratch_unavailable(void) {
    return g_scratch_unavailable ? "this machine has no such hardware" : 0;
}

// NOT const, and `.file` is filled in by scratch_begin(): the scratch
// path is built from a setting now, so it is not a compile-time
// constant. What it points at is scratch_file_path()'s own static
// buffer, which outlives the registration -- a stack local here would
// leave the registry holding a dangling pointer (api/setting.h).
static struct setting g_scratch = {
    .name   = SCRATCH_NAME,
    .label  = "Scratch colour",
    .type   = SETTING_TYPE_ENUM,
    .choice = scratch_choice,
    .get    = scratch_get,
    .apply  = scratch_apply,
    .unavailable = scratch_unavailable,
};

// Registration is per-test rather than once, because a KTEST must
// establish its own preconditions rather than inherit them from
// whatever ran before (ktest.h) -- and a `ktest setting` run of one
// test has to work as well as a run of all of them.
static struct setting g_scratch2;   // defined below; scratch_begin() fills its .file

static void scratch_begin(void) {
    setting_unregister(SCRATCH_NAME); // in case a previous test left it
    k_strlcpy(g_scratch_value, "amber", sizeof g_scratch_value);
    g_scratch_applies = 1;
    g_scratch_apply_calls = 0;
    g_scratch_unavailable = 0;
    // BOTH descriptors, here, because two tests register g_scratch2 and
    // a NULL .file makes the registry's duplicate check compare two
    // NULLs equal -- which reads as "the same file" and refuses a
    // registration that should have succeeded. It was a compile-time
    // constant before the path became a setting; this is the one place
    // that has to remember now.
    g_scratch.file = SCRATCH_FILE;
    g_scratch2.file = SCRATCH2_FILE;
    fs_delete(SCRATCH_FILE); // may not exist
    setting_register(&g_scratch);
}

static void scratch_end(void) {
    g_scratch_unavailable = 0;
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

KTEST("setting", "every registered setting reports a value and a home") {
    // The property a settings UI depends on: a row it can draw. A
    // setting with no current value renders as an empty control that
    // looks broken, and one with no home cannot answer "where is this
    // stored?" -- the question the registry exists to answer.
    //
    // "A HOME" RATHER THAN "A FILE", since tunables. This asserted
    // `s->file[0] == '/'` until kernel.heap_debug and its two siblings
    // arrived, which is the moment the invariant genuinely changed: a
    // setting's `file` is now either an absolute path OR the runtime
    // sentinel. Widened here rather than deleted, and paired with the
    // rule that makes the new case safe -- a tunable holds its value in
    // the kernel, so one with no `apply` would hold it nowhere at all.
    char v[SETTING_VALUE_MAX];
    for (int i = 0; i < setting_count(); i++) {
        const struct setting *s = setting_at(i);
        KTEST_ASSERT(s != 0);
        KTEST_ASSERT(s->file != 0);
        if (setting_persists(s)) {
            KTEST_ASSERT(s->file[0] == '/');
        } else {
            KTEST_ASSERT(k_strcmp(s->file, CONFIG_PATH_RUNTIME) == 0);
            KTEST_ASSERT(s->apply != 0);
        }
        v[0] = '\0';
        s->get(v, sizeof v);
        KTEST_ASSERT(v[0] != '\0');
    }
}

KTEST("setting", "a tunable is applied but never written") {
    // The defining property, asserted rather than assumed. A tunable
    // that quietly persisted would look identical at the prompt and
    // differ only after a reboot -- the slowest possible way to find
    // out, and the reason this is a test rather than a comment.
    const struct setting *s = setting_find("kernel.heap_debug");
    KTEST_ASSERT(s != 0);
    KTEST_ASSERT_EQ(setting_persists(s), 0);
    // Its namespace still resolves, which is the whole point of the
    // sentinel path: identity is (namespace, name), so a tunable with
    // no namespace would be addressable only as a bare name.
    KTEST_ASSERT(k_strcmp(setting_namespace(s), CONFIG_NAME_RUNTIME) == 0);

    // Round-trip through the registry, and put it back. Reading the
    // value through get() rather than trusting the setter is what makes
    // this a round trip instead of an echo.
    char before[SETTING_VALUE_MAX], after[SETTING_VALUE_MAX];
    s->get(before, sizeof before);
    KTEST_ASSERT(setting_set("kernel.heap_debug", "on") != SETTING_INVALID);
    s->get(after, sizeof after);
    KTEST_ASSERT(k_strcmp(after, "on") == 0);
    setting_set("kernel.heap_debug", before);

    // And a value it does not accept is REFUSED, not coerced to off --
    // a tunable that reads an unrecognised word as "off" silently
    // disarms itself.
    KTEST_ASSERT_EQ(setting_set("kernel.heap_debug", "yes"), SETTING_INVALID);
}

KTEST("setting", "a duplicate name IN THE SAME FILE is refused, not shadowed") {
    scratch_begin();
    int before = setting_count();
    KTEST_ASSERT_EQ(setting_register(&g_scratch), 0);
    KTEST_ASSERT_EQ(setting_count(), before);
    scratch_end();
}

// --- qualified names -------------------------------------------------
//
// A setting's identity is (namespace, name), the namespace being the
// registered name of its FILE -- so the same name in two different
// files is two settings, which is the whole point: two programs may
// each own a `theme`. Nothing in the kernel registers such a pair
// today, so these build one.
// Built from the configured directory rather than spelled out
// (api/tmppath.h), so moving scratch is a setting rather than a grep.


static char g_scratch2_value[SETTING_VALUE_MAX] = "blue";

static void scratch2_get(char *out, uint32_t out_size) {
    k_strlcpy(out, g_scratch2_value, out_size);
}

static int scratch2_apply(const char *value) {
    k_strlcpy(g_scratch2_value, value, sizeof g_scratch2_value);
    return etc_config_set(SCRATCH2_FILE, SCRATCH_NAME, value)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

// Same NAME as g_scratch, different FILE. Not const, and `.file` set
// before registration, for the same reason g_scratch is not.
static struct setting g_scratch2 = {
    .name   = SCRATCH_NAME,
    .label  = "Scratch two",
    .type   = SETTING_TYPE_STRING,
    .choice = 0,
    .get    = scratch2_get,
    .apply  = scratch2_apply,
};

KTEST("setting", "the same name in a DIFFERENT file is a second setting") {
    scratch_begin();
    config_file_register("ktestns1", SCRATCH_FILE, "scratch one", 0);
    config_file_register("ktestns2", SCRATCH2_FILE, "scratch two", 0);
    int before = setting_count();

    // The registration the old flat namespace refused outright.
    KTEST_ASSERT_EQ(setting_register(&g_scratch2), 1);
    KTEST_ASSERT_EQ(setting_count(), before + 1);

    // Each is reachable by its own qualified name, and they are
    // genuinely different objects -- not one entry found twice.
    const struct setting *a = setting_find("ktestns1." SCRATCH_NAME);
    const struct setting *b = setting_find("ktestns2." SCRATCH_NAME);
    KTEST_ASSERT(a != 0 && b != 0);
    KTEST_ASSERT(a != b);
    KTEST_ASSERT_EQ(k_strcmp(setting_namespace(a), "ktestns1"), 0);
    KTEST_ASSERT_EQ(k_strcmp(setting_namespace(b), "ktestns2"), 0);

    setting_unregister("ktestns2." SCRATCH_NAME);
    scratch_end();
}

KTEST("setting", "a bare name matching two settings is AMBIGUOUS, not the first") {
    scratch_begin();
    config_file_register("ktestns1", SCRATCH_FILE, "scratch one", 0);
    config_file_register("ktestns2", SCRATCH2_FILE, "scratch two", 0);
    KTEST_ASSERT_EQ(setting_register(&g_scratch2), 1);

    // THE assertion of the whole scheme. Returning either one would
    // make which setting a bare name means depend on registration
    // order, i.e. on boot sequence -- and the caller would never know
    // it had been guessed at.
    KTEST_ASSERT(setting_find(SCRATCH_NAME) == 0);
    KTEST_ASSERT_EQ(setting_matches(SCRATCH_NAME), 2);
    // ...while a qualified name is never ambiguous.
    KTEST_ASSERT_EQ(setting_matches("ktestns1." SCRATCH_NAME), 1);

    // And "no such setting" stays distinguishable from "say which one",
    // which is the difference a UI has to report differently.
    KTEST_ASSERT_EQ(setting_matches("ktest_nonexistent"), 0);

    setting_unregister("ktestns2." SCRATCH_NAME);
    scratch_end();
}

KTEST("setting", "a qualified name round-trips through setting_qualified()") {
    scratch_begin();
    config_file_register("ktestns1", SCRATCH_FILE, "scratch one", 0);

    const struct setting *s = setting_find("ktestns1." SCRATCH_NAME);
    KTEST_ASSERT(s != 0);
    char q[SETTING_QUALIFIED_MAX];
    KTEST_ASSERT_EQ(setting_qualified(s, q, sizeof q), 1);
    KTEST_ASSERT_EQ(k_strcmp(q, "ktestns1." SCRATCH_NAME), 0);
    // The name it produces is one it can find again -- the property a
    // client depends on when it prints a name for the user to type.
    KTEST_ASSERT(setting_find(q) == s);

    // A buffer too small must FAIL rather than truncate: a truncated
    // qualified name is a different setting's name, not a shorter one.
    char tiny[4];
    KTEST_ASSERT_EQ(setting_qualified(s, tiny, sizeof tiny), 0);

    scratch_end();
}

KTEST("setting", "a malformed descriptor is refused at registration") {
    // An ENUM with no choice enumerator would draw an empty picker --
    // a control that appears and cannot be used. Caught here rather
    // than discovered in a UI.
    // `.file` is assigned rather than initialised because the path is
    // built at runtime now. Neither descriptor is ever registered, so
    // the value plays no part in what is asserted -- it is set only so
    // that nothing here spells a directory out.
    static struct setting bad_enum = {
        .name = "ktest_bad_enum", .label = "Bad", .type = SETTING_TYPE_ENUM,
        .choice = 0, .get = scratch_get, .apply = 0,
    };
    static struct setting no_get = {
        .name = "ktest_no_get", .label = "Bad", .type = SETTING_TYPE_STRING,
        .choice = 0, .get = 0, .apply = 0,
    };
    bad_enum.file = SCRATCH_FILE;
    no_get.file = SCRATCH_FILE;
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

// --- a setting this machine cannot change ----------------------------
//
// The rule: get() returns what apply() last accepted, and a setting
// that cannot take effect HERE is refused by the REGISTRY -- not merely
// greyed out by whichever UI happened to ask. `config set` and a
// hand-edited /etc file reach setting_set() without passing through any
// control, so a check that lived in System Settings would be one the
// shell walked straight past. See api/setting.h's `unavailable`.

KTEST("setting", "an unavailable setting is refused by the registry, not just by a UI") {
    scratch_begin();
    g_scratch_unavailable = 1;

    int calls = g_scratch_apply_calls;
    enum setting_result r = setting_set(SCRATCH_NAME, "green");
    int calls_after = g_scratch_apply_calls;
    char v[SETTING_VALUE_MAX];
    scratch_get(v, sizeof v);
    // Capture, release, THEN assert -- a KTEST_ASSERT returns from the
    // body, and an assertion made while the scratch setting is still
    // registered leaks it into every later test.
    scratch_end();

    KTEST_ASSERT(r == SETTING_INVALID);
    // apply() must not even RUN: refusing after the subsystem has been
    // poked would leave the machine changed and the caller told no.
    KTEST_ASSERT(calls_after == calls);
    KTEST_ASSERT(k_strcmp(v, "amber") == 0);
}

KTEST("setting", "clearing unavailable() makes the same set succeed") {
    // The positive control for the test above. Without it, "refused"
    // would pass equally well against a setting that simply never
    // accepts anything -- which is what scratch_apply's own reject mode
    // does, and is a different failure.
    scratch_begin();
    g_scratch_unavailable = 1;
    enum setting_result refused = setting_set(SCRATCH_NAME, "green");
    g_scratch_unavailable = 0;
    enum setting_result allowed = setting_set(SCRATCH_NAME, "green");
    char v[SETTING_VALUE_MAX];
    scratch_get(v, sizeof v);
    scratch_end();

    KTEST_ASSERT(refused == SETTING_INVALID);
    KTEST_ASSERT(allowed == SETTING_SAVED || allowed == SETTING_UNSAVED);
    KTEST_ASSERT(k_strcmp(v, "green") == 0);
}

KTEST("setting", "the REASON reaches ring 3 on INFO") {
    // A disabled control is only an improvement when the user can see
    // why, so the sentence has to survive the ABI -- a boolean would
    // not have been enough. Found by INDEX, since that is what INFO
    // takes and the scratch setting's index is wherever it registered.
    scratch_begin();
    g_scratch_unavailable = 1;

    struct setting_msg m;
    int idx = -1, ok = 0;
    char reason[SETTING_ABI_DESC_MAX];
    reason[0] = '\0';
    for (int i = 0; i < setting_count(); i++) {
        k_memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (!setting_dispatch(&m)) continue;
        if (k_strcmp(m.name, SCRATCH_NAME) != 0) continue;
        idx = i;
        ok = 1;
        k_strlcpy(reason, m.unavailable, sizeof reason);
        break;
    }
    // ...and empty once it is available again, so a client that caches
    // the field is told when the answer changes.
    char cleared[SETTING_ABI_DESC_MAX];
    cleared[0] = '\0';
    if (idx >= 0) {
        g_scratch_unavailable = 0;
        k_memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = idx;
        if (setting_dispatch(&m)) k_strlcpy(cleared, m.unavailable, sizeof cleared);
    }
    scratch_end();

    KTEST_ASSERT(ok);
    KTEST_ASSERT(reason[0] != '\0');
    KTEST_ASSERT(cleared[0] == '\0');
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

KTEST("setting", "an INT setting is bounded by the REGISTRY, not by a widget") {
    // THE POINT OF THE TYPE. A spinbox knows the range and will not
    // offer a value outside it, but `config set system.mouse_speed 0`
    // and a hand-edited /etc file reach setting_set() without passing
    // through any UI -- and 0 freezes the pointer. Speed was an ENUM of
    // named levels precisely to avoid that (see mouse_config.c's own
    // comment), so this is the check that let the workaround go.
    char before[SETTING_VALUE_MAX];
    before[0] = 0;
    const struct setting *s = setting_find("system.mouse_speed");
    if (!s) return; // not registered on this build
    KTEST_ASSERT_EQ((int)s->type, SETTING_TYPE_INT);
    s->get(before, sizeof before);

    // The declared range is reported, so a client is not guessing it.
    KTEST_ASSERT(s->min > 0 && s->max > s->min && s->step > 0);

    // REFUSED, not clamped: a caller told "saved" whose value then reads
    // back different has been lied to.
    KTEST_ASSERT_EQ((int)setting_set("system.mouse_speed", "0"), SETTING_INVALID);
    KTEST_ASSERT_EQ((int)setting_set("system.mouse_speed", "-5"), SETTING_INVALID);
    KTEST_ASSERT_EQ((int)setting_set("system.mouse_speed", "99999"), SETTING_INVALID);
    KTEST_ASSERT_EQ((int)setting_set("system.mouse_speed", "abc"), SETTING_INVALID);
    KTEST_ASSERT_EQ((int)setting_set("system.mouse_speed", ""), SETTING_INVALID);
    // ...including the LEGACY NAMES, which the boot reader still accepts
    // for migration but the registry must not: they are not numbers, and
    // a setting that took both would have two spellings of one value.
    //
    // **THIS IS THE LINE THAT MAKES THE TEST DISCRIMINATING**, and it is
    // worth knowing why the numeric cases above are not. speed_apply()
    // re-checks the range itself (deliberately -- it is reachable from
    // callers that never pass through setting_set), so removing the
    // registry's gate leaves "0" and "99999" rejected anyway and every
    // numeric assertion here still passes. "slow" is the one value the
    // two layers disagree about: apply ACCEPTS it, so only the registry
    // can refuse it. Verified by removing that gate and watching exactly
    // this assertion redden.
    KTEST_ASSERT_EQ((int)setting_set("system.mouse_speed", "slow"), SETTING_INVALID);

    // A refusal must leave the live value ALONE -- the failure mode that
    // matters is "rejected the value and applied it anyway".
    char after[SETTING_VALUE_MAX];
    after[0] = 0;
    s->get(after, sizeof after);
    KTEST_ASSERT_EQ(k_strcmp(before, after), 0);

    // Both ends inclusive, and one step in from each.
    char buf[SETTING_VALUE_MAX];
    k_snprintf(buf, sizeof buf, "%d", s->min);
    KTEST_ASSERT(setting_set("system.mouse_speed", buf) != SETTING_INVALID);
    k_snprintf(buf, sizeof buf, "%d", s->max);
    KTEST_ASSERT(setting_set("system.mouse_speed", buf) != SETTING_INVALID);
    k_snprintf(buf, sizeof buf, "%d", s->min - 1);
    KTEST_ASSERT_EQ((int)setting_set("system.mouse_speed", buf), SETTING_INVALID);
    k_snprintf(buf, sizeof buf, "%d", s->max + 1);
    KTEST_ASSERT_EQ((int)setting_set("system.mouse_speed", buf), SETTING_INVALID);

    setting_set("system.mouse_speed", before);
}

KTEST("setting", "a legacy named speed in /etc still migrates") {
    // AN UPGRADE PATH, ASSERTED. A disk written before speed became a
    // number says `mouse_speed=slow`; a numeric parser alone would
    // reject it and silently leave the pointer at its default, throwing
    // away a choice the user made. The BOOT reader accepts the old names
    // (the registry does not -- see the test above), and the next write
    // stores a number, so this is a one-way migration.
    const struct setting *s = setting_find("system.mouse_speed");
    if (!s) return;
    char before[SETTING_VALUE_MAX];
    before[0] = 0;
    s->get(before, sizeof before);

    // Written straight to the file, bypassing the registry -- which is
    // exactly how an old disk or a hand edit presents it.
    if (!etc_config_set("/etc/toyos.conf", "mouse_speed", "slow")) {
        setting_set("system.mouse_speed", before);
        return; // no writable /etc on this image
    }
    mouse_config_init();
    KTEST_ASSERT_EQ(mouse_config_speed_pct(), 50); // "slow" was half speed

    // ...and the next set rewrites it as a number.
    KTEST_ASSERT(setting_set("system.mouse_speed", "125") != SETTING_INVALID);
    char on_disk[SETTING_VALUE_MAX];
    on_disk[0] = 0;
    KTEST_ASSERT(etc_config_get("/etc/toyos.conf", "mouse_speed", on_disk, sizeof on_disk));
    KTEST_ASSERT_EQ(k_strcmp(on_disk, "125"), 0);

    setting_set("system.mouse_speed", before);
}
