// The pointer's two knobs -- speed and acceleration -- as registered
// settings.
//
// Same shape as cursor_config.c: read one key, hand the value to the
// subsystem that knows what it means, write it back on change.
// Everything that knows what speed and acceleration DO lives in
// kernel/drivers/mouse.c; this file only maps names to numbers.
//
// NAMED CHOICES, not a number. `mouse_speed=slow` rather than
// `mouse_speed=50` because the setting is an ENUM in the registry, which
// is what gives it a choice list -- and a choice list is what lets a UI
// present it at all without inventing a slider widget. It also bounds
// the value: a hand-edited 0 would freeze the pointer, and the parse
// below simply does not accept one.
#include "mouse_config.h"
#include "etc_config.h"
#include "setting.h"
#include "mouse.h"
#include "string.h"

#define MOUSE_CONFIG_FILE "/etc/toyos.conf"
#define MOUSE_SPEED_KEY "mouse_speed"
#define MOUSE_ACCEL_KEY "mouse_accel"

// The names and what they mean, in one table each, so the choice
// enumerator, the parser and the getter cannot drift.
struct named_level { const char *name; int value; };

// Numerators over MOUSE_SPEED_UNIT (mouse.h), so "normal" is exactly 1x
// and the pointer behaves as it always did when nothing is configured.
static const struct named_level SPEEDS[] = {
    { "slow",     MOUSE_SPEED_UNIT / 2 },
    { "normal",   MOUSE_SPEED_UNIT },
    { "fast",     MOUSE_SPEED_UNIT * 3 / 2 },
    { "veryfast", MOUSE_SPEED_UNIT * 2 },
};
#define SPEED_COUNT ((int)(sizeof SPEEDS / sizeof SPEEDS[0]))

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

static int g_speed_index = 1; // "normal"
static int g_accel_index = 0; // "off"

static int find_level(const struct named_level *tbl, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (k_strcmp(tbl[i].name, name) == 0) return i;
    return -1;
}

void mouse_config_init(void) {
    char value[16];
    if (etc_config_get(MOUSE_CONFIG_FILE, MOUSE_SPEED_KEY, value, sizeof value)) {
        int i = find_level(SPEEDS, SPEED_COUNT, value);
        // An unrecognised name leaves the default in place rather than
        // failing the boot -- the same tolerance every other /etc reader
        // here has.
        if (i >= 0) { g_speed_index = i; mouse_set_speed(SPEEDS[i].value); }
    }
    if (etc_config_get(MOUSE_CONFIG_FILE, MOUSE_ACCEL_KEY, value, sizeof value)) {
        int i = find_level(ACCELS, ACCEL_COUNT, value);
        if (i >= 0) { g_accel_index = i; mouse_set_accel_threshold(ACCELS[i].value); }
    }
}

// --- the registry descriptors (see setting.h) ------------------------

static int speed_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= SPEED_COUNT) return 0;
    k_strlcpy(out, SPEEDS[index].name, out_size);
    return 1;
}

static void speed_get(char *out, uint32_t out_size) {
    k_strlcpy(out, SPEEDS[g_speed_index].name, out_size);
}

static int speed_apply(const char *value) {
    int i = find_level(SPEEDS, SPEED_COUNT, value);
    if (i < 0) return SETTING_INVALID;
    g_speed_index = i;
    mouse_set_speed(SPEEDS[i].value);
    return etc_config_set(MOUSE_CONFIG_FILE, MOUSE_SPEED_KEY, SPEEDS[i].name)
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

// BOTH declare group "Mouse", and so do the two cursor settings -- which
// is the whole point of groups: one page carrying every knob that is
// about the pointer, rather than four pages a user has to find.
static const struct setting g_speed_setting = {
    .name   = MOUSE_SPEED_KEY,
    .label  = "Pointer speed",
    .type   = SETTING_TYPE_ENUM,
    .file   = MOUSE_CONFIG_FILE,
    .category = "Input",
    .group  = "Mouse",
    .choice = speed_choice,
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

void mouse_config_setting_register(void) {
    setting_register(&g_speed_setting);
    setting_register(&g_accel_setting);
}
