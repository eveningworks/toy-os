// The output volume as a registered setting -- mouse_config.c's shape:
// this file only maps the number to the subsystem that knows what it
// means (sound_set_volume()), and the registry provides the range, the
// System Settings row and `config set volume` for free.
//
// Registered whether or not a device exists: the SETTING is the user's
// stated preference, and a boot without hardware must not silently
// discard it. Applying with no device is a no-op; ac97_init() cannot
// have run yet when settings register.
#include "sound_config.h"
#include "sound.h"
#include "etc_config.h"
#include "setting.h"
#include "string.h"
#include "kfmt.h"
#include "knum.h"

#define SOUND_CONFIG_FILE "/etc/toyos.conf"
#define VOLUME_KEY "volume"

#define VOLUME_MIN     0
#define VOLUME_MAX     100
#define VOLUME_STEP    5
#define VOLUME_DEFAULT 100

static int g_volume = VOLUME_DEFAULT;

static int parse_pct(const char *value) {
    if (!value || !value[0]) return -1;
    int v = 0;
    const char *p = value;
    for (; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    if (*p || p == value) return -1;
    return v;
}

static void apply_pct(int pct) {
    g_volume = pct;
    sound_set_volume(pct);
}

void sound_config_init(void) {
    char value[16];
    if (etc_config_get(SOUND_CONFIG_FILE, VOLUME_KEY, value, sizeof value)) {
        int pct = parse_pct(value);
        if (pct >= VOLUME_MIN && pct <= VOLUME_MAX) apply_pct(pct);
    }
    // The default (or the stored value) reaches the hardware once the
    // driver is up -- this runs after ac97_init() (kernel_main's order).
    sound_set_volume(g_volume);
}

static void volume_get(char *out, uint32_t out_size) {
    k_snprintf(out, out_size, "%d", g_volume);
}

static int volume_apply(const char *value) {
    int pct = parse_pct(value);
    if (pct < VOLUME_MIN || pct > VOLUME_MAX) return SETTING_INVALID;
    apply_pct(pct);
    char buf[16];
    k_snprintf(buf, sizeof buf, "%d", pct);
    return etc_config_set(SOUND_CONFIG_FILE, VOLUME_KEY, buf)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static const struct setting g_volume_setting = {
    .name   = VOLUME_KEY,
    .label  = "Volume",
    .type   = SETTING_TYPE_INT,
    .file   = SOUND_CONFIG_FILE,
    .category = "Sound",
    .group  = "Output",
    .min    = VOLUME_MIN,
    .max    = VOLUME_MAX,
    .step   = VOLUME_STEP,
    .unit   = "%",
    .get    = volume_get,
    .apply  = volume_apply,
};

void sound_config_setting_register(void) {
    setting_register(&g_volume_setting);
}
