// The pointer's two knobs -- speed and acceleration -- as registered
// settings.
//
// Same shape as cursor_config.c: read one key, hand the value to the
// subsystem that knows what it means, write it back on change.
// Everything that knows what speed and acceleration DO lives in
// kernel/drivers/mouse.c; this file only maps names to numbers.
//
// SPEED IS A NUMBER; ACCELERATION IS STILL NAMED. That split is the
// interesting part of this file.
//
// Speed used to be an ENUM of named levels, and the comment here said
// why: a choice list "is what lets a UI present it at all without
// inventing a slider widget", and it bounded the value, since a
// hand-edited 0 would freeze the pointer. Both of those were working
// around a missing registry feature rather than describing the setting,
// and SETTING_TYPE_INT (api/setting.h) provides them directly -- a range
// a UI can build a spinbox from, enforced by the registry itself. So
// speed is now a PERCENTAGE of normal, 25..300, which is what a pointer
// speed actually is and what every desktop exposes.
//
// Acceleration stays an ENUM because it genuinely is one: its values are
// device-count thresholds where LOWER means MORE acceleration, so the
// numbers run backwards from the effect. "high" is a better name than 3
// for the same reason `off` is a better name than 0 -- naming is doing
// real work there, not standing in for a range.
#include "mouse_config.h"
#include "etc_config.h"
#include "setting.h"
#include "mouse.h"
#include "string.h"
#include "kfmt.h"
#include "initcall.h"

#define MOUSE_CONFIG_FILE "/etc/toyos.conf"
#define MOUSE_SPEED_KEY "mouse_speed"
#define MOUSE_ACCEL_KEY "mouse_accel"
#define SCROLL_STEP_KEY "scroll_step"
#define SCROLL_DIR_KEY  "scroll_dir"

// The wheel's multiplier: 1 is one notch per notch. 5 is the useful
// ceiling -- past it a single click of the wheel jumps a whole page in
// a three-line-per-notch terminal.
#define SCROLL_STEP_MIN     1
#define SCROLL_STEP_MAX     5
#define SCROLL_STEP_DEFAULT 1

// The names and what they mean, in one table each, so the choice
// enumerator, the parser and the getter cannot drift.
struct named_level { const char *name; int value; };

// Speed as a PERCENTAGE of normal. 100 is exactly 1x, so a machine with
// nothing configured behaves as it always did.
//
// The bounds are the useful range rather than the representable one: 25
// is slow enough to cross a 1280px screen deliberately, 300 fast enough
// to be twitchy, and 0 -- which would freeze the pointer entirely -- is
// not reachable at all, from a spinbox, from `config set`, or from a
// hand-edited /etc file. setting.c enforces that, not this file.
#define SPEED_MIN     25
#define SPEED_MAX     300
#define SPEED_STEP    25
#define SPEED_DEFAULT 100

// THE OLD NAMES, STILL READ. An /etc/toyos.conf written before speed
// became a number says `mouse_speed=slow`, and a numeric parser would
// reject it and silently leave the pointer at the default -- a setting
// the user chose, quietly discarded on upgrade. Reading them costs four
// table rows; the file is rewritten as a number the next time anything
// sets it, so this is a one-way migration and not a second format to
// maintain.
static const struct named_level LEGACY_SPEEDS[] = {
    { "slow",      50 },
    { "normal",   100 },
    { "fast",     150 },
    { "veryfast", 200 },
};
#define LEGACY_SPEED_COUNT ((int)(sizeof LEGACY_SPEEDS / sizeof LEGACY_SPEEDS[0]))

// Threshold in device counts per packet; 0 is off. Lower threshold =
// acceleration kicks in sooner, so "high" is the SMALLEST number --
// which is why these are named rather than typed in.
static const struct named_level ACCELS[] = {
    { "off",    0 },
    { "low",    8 },
    { "medium", 5 },
    { "high",   3 },
};
#define ACCEL_COUNT ((int)(sizeof ACCELS / sizeof ACCELS[0]))

// Direction is an ENUM, not a bool the registry has not got:
// "inverted" is macOS's natural scrolling under the name Windows and
// KDE give it, and a name beats remembering which way `1` points.
static const struct named_level SCROLL_DIRS[] = {
    { "normal",   0 },
    { "inverted", 1 },
};
#define SCROLL_DIR_COUNT ((int)(sizeof SCROLL_DIRS / sizeof SCROLL_DIRS[0]))

static int g_speed_pct = SPEED_DEFAULT;
static int g_accel_index = 0; // "off"
static int g_scroll_dir_index = 0; // "normal"

// A percentage into what mouse.c wants: a numerator over
// MOUSE_SPEED_UNIT, so 100% is exactly the unit.
static void apply_speed_pct(int pct) {
    g_speed_pct = pct;
    mouse_set_speed(MOUSE_SPEED_UNIT * pct / 100);
}

// Parses a stored speed: a number, or one of the legacy names. Returns
// the percentage, or -1.
static int parse_speed(const char *value) {
    if (!value || !value[0]) return -1;
    int v = 0;
    const char *p = value;
    for (; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    if (!*p && p != value) return v;
    for (int i = 0; i < LEGACY_SPEED_COUNT; i++)
        if (k_strcmp(LEGACY_SPEEDS[i].name, value) == 0) return LEGACY_SPEEDS[i].value;
    return -1;
}

static int find_level(const struct named_level *tbl, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (k_strcmp(tbl[i].name, name) == 0) return i;
    return -1;
}

// Loaded ONCE and asked four questions (etc_config.h): the per-key
// getter re-reads the whole file per call. Static, not a local -- the
// buffer is 4 KiB against a 1 KiB frame budget.
static struct etc_config_buf g_cfg;

void mouse_config_init(void) {
    char value[16];
    etc_config_load(MOUSE_CONFIG_FILE, &g_cfg);   // a missing file: every get below says 0
    if (etc_config_buf_get(&g_cfg, MOUSE_SPEED_KEY, value, sizeof value)) {
        // An unparseable or out-of-range value leaves the default in
        // place rather than failing the boot -- the same tolerance every
        // other /etc reader here has. The RANGE is checked here as well
        // as in setting.c, because a hand-edited file reaches this
        // reader without going through setting_set() at all.
        int pct = parse_speed(value);
        if (pct >= SPEED_MIN && pct <= SPEED_MAX) apply_speed_pct(pct);
    }
    if (etc_config_buf_get(&g_cfg, MOUSE_ACCEL_KEY, value, sizeof value)) {
        int i = find_level(ACCELS, ACCEL_COUNT, value);
        if (i >= 0) { g_accel_index = i; mouse_set_accel_threshold(ACCELS[i].value); }
    }
    if (etc_config_buf_get(&g_cfg, SCROLL_STEP_KEY, value, sizeof value)) {
        int step = parse_speed(value); // a plain number; the legacy names miss
        if (step >= SCROLL_STEP_MIN && step <= SCROLL_STEP_MAX)
            mouse_set_scroll_step(step);
    }
    if (etc_config_buf_get(&g_cfg, SCROLL_DIR_KEY, value, sizeof value)) {
        int i = find_level(SCROLL_DIRS, SCROLL_DIR_COUNT, value);
        if (i >= 0) { g_scroll_dir_index = i; mouse_set_scroll_invert(SCROLL_DIRS[i].value); }
    }
}
INITCALL(mouse_config_init, INIT_CONFIG);

int mouse_config_speed_pct(void) { return g_speed_pct; }

// --- the registry descriptors (see setting.h) ------------------------

static void speed_get(char *out, uint32_t out_size) {
    k_snprintf(out, out_size, "%d", g_speed_pct);
}

static int speed_apply(const char *value) {
    // The range has ALREADY been checked by setting_set() for an INT
    // setting -- but not when the shell or a KTEST calls this directly,
    // so it is checked again rather than assumed. Two cheap comparisons
    // against the alternative of a code path that can freeze the
    // pointer.
    int pct = parse_speed(value);
    if (pct < SPEED_MIN || pct > SPEED_MAX) return SETTING_INVALID;
    apply_speed_pct(pct);
    // WRITTEN AS A NUMBER, always -- including when a legacy name came
    // in. That is what makes reading the old names a migration rather
    // than a format this file has to keep supporting.
    char buf[16];
    k_snprintf(buf, sizeof buf, "%d", pct);
    return etc_config_set(MOUSE_CONFIG_FILE, MOUSE_SPEED_KEY, buf)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static int accel_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= ACCEL_COUNT) return 0;
    k_strlcpy(out, ACCELS[index].name, out_size);
    return 1;
}

static void accel_get(char *out, uint32_t out_size) {
    k_strlcpy(out, ACCELS[g_accel_index].name, out_size);
}

static int accel_apply(const char *value) {
    int i = find_level(ACCELS, ACCEL_COUNT, value);
    if (i < 0) return SETTING_INVALID;
    g_accel_index = i;
    mouse_set_accel_threshold(ACCELS[i].value);
    return etc_config_set(MOUSE_CONFIG_FILE, MOUSE_ACCEL_KEY, ACCELS[i].name)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static void scroll_step_get(char *out, uint32_t out_size) {
    k_snprintf(out, out_size, "%d", mouse_scroll_step());
}

static int scroll_step_apply(const char *value) {
    int step = parse_speed(value);
    if (step < SCROLL_STEP_MIN || step > SCROLL_STEP_MAX) return SETTING_INVALID;
    mouse_set_scroll_step(step);
    char buf[16];
    k_snprintf(buf, sizeof buf, "%d", step);
    return etc_config_set(MOUSE_CONFIG_FILE, SCROLL_STEP_KEY, buf)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static int scroll_dir_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= SCROLL_DIR_COUNT) return 0;
    k_strlcpy(out, SCROLL_DIRS[index].name, out_size);
    return 1;
}

static void scroll_dir_get(char *out, uint32_t out_size) {
    k_strlcpy(out, SCROLL_DIRS[g_scroll_dir_index].name, out_size);
}

static int scroll_dir_apply(const char *value) {
    int i = find_level(SCROLL_DIRS, SCROLL_DIR_COUNT, value);
    if (i < 0) return SETTING_INVALID;
    g_scroll_dir_index = i;
    mouse_set_scroll_invert(SCROLL_DIRS[i].value);
    return etc_config_set(MOUSE_CONFIG_FILE, SCROLL_DIR_KEY, SCROLL_DIRS[i].name)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

// BOTH declare group "Mouse", and so do the two cursor settings -- which
// is the whole point of groups: one page carrying every knob that is
// about the pointer, rather than four pages a user has to find.
static const struct setting g_speed_setting = {
    .name   = MOUSE_SPEED_KEY,
    .label  = "Pointer speed",
    .type   = SETTING_TYPE_INT,
    .file   = MOUSE_CONFIG_FILE,
    .category = "Input",
    .group  = "Mouse",
    .min    = SPEED_MIN,
    .max    = SPEED_MAX,
    .step   = SPEED_STEP,
    .unit   = "%",
    .get    = speed_get,
    .apply  = speed_apply,
};

static const struct setting g_accel_setting = {
    .name   = MOUSE_ACCEL_KEY,
    .label  = "Pointer acceleration",
    .type   = SETTING_TYPE_ENUM,
    .file   = MOUSE_CONFIG_FILE,
    .category = "Input",
    .group  = "Mouse",
    .choice = accel_choice,
    .get    = accel_get,
    .apply  = accel_apply,
};

static const struct setting g_scroll_step_setting = {
    .name   = SCROLL_STEP_KEY,
    .label  = "Scroll speed",
    .type   = SETTING_TYPE_INT,
    .file   = MOUSE_CONFIG_FILE,
    .category = "Input",
    .group  = "Mouse",
    .min    = SCROLL_STEP_MIN,
    .max    = SCROLL_STEP_MAX,
    .step   = 1,
    .unit   = "x",
    .get    = scroll_step_get,
    .apply  = scroll_step_apply,
};

static const struct setting g_scroll_dir_setting = {
    .name   = SCROLL_DIR_KEY,
    .label  = "Scroll direction",
    .type   = SETTING_TYPE_ENUM,
    .file   = MOUSE_CONFIG_FILE,
    .category = "Input",
    .group  = "Mouse",
    .choice = scroll_dir_choice,
    .get    = scroll_dir_get,
    .apply  = scroll_dir_apply,
};

void mouse_config_setting_register(void) {
    setting_register(&g_speed_setting);
    setting_register(&g_accel_setting);
    setting_register(&g_scroll_step_setting);
    setting_register(&g_scroll_dir_setting);
}
