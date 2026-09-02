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
#include "initcall.h"

#define SOUND_CONFIG_FILE "/etc/toyos.conf"
#define VOLUME_KEY "volume"
#define DEVICE_KEY "audio_device"

// The first choice, and what a machine nobody has configured uses: the
// newest registered device wins, so plugging in a USB card switches to
// it. A person picking one from the list replaces this with a name.
#define DEVICE_AUTO "auto"

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
    char value[SOUND_NAME_MAX];
    if (etc_config_get(SOUND_CONFIG_FILE, DEVICE_KEY, value, sizeof value))
        sound_select(value);

    char vol[16];
    if (etc_config_get(SOUND_CONFIG_FILE, VOLUME_KEY, vol, sizeof vol)) {
        int pct = parse_pct(vol);
        if (pct >= VOLUME_MIN && pct <= VOLUME_MAX) apply_pct(pct);
    }
    // The default (or the stored value) reaches the hardware once the
    // driver is up -- this runs after ac97_init() (kernel_main's order).
    sound_set_volume(g_volume);
}
INITCALL(sound_config_init, INIT_CONFIG);

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

// --- the output device ------------------------------------------------
//
// An ENUM whose choices are DATA -- the registered drivers -- so it
// supplies its own labels rather than /etc/settings.d carrying a list
// that would have to be regenerated whenever a card was plugged in.
// See api/setting.h's choice_label().

static void device_get(char *out, uint32_t out_size) {
    k_strlcpy(out, sound_preference(), out_size);
}

static int device_choice(int index, char *out, uint32_t out_size) {
    if (index == 0) { k_strlcpy(out, DEVICE_AUTO, out_size); return 1; }
    if (index - 1 >= sound_device_count()) return 0;
    k_strlcpy(out, sound_device_name(index - 1), out_size);
    return 1;
}

static int device_choice_label(int index, char *out, uint32_t out_size) {
    if (index == 0) {
        // Naming what auto WOULD PICK -- the first device discovered --
        // rather than what is active now. Those differ exactly when
        // somebody has chosen the other device, which is the one moment
        // a row reading "Automatic (ac97)" beside a ticked `ac97` would
        // be describing the tick instead of itself.
        const char *first = sound_device_count() ? sound_device_name(0) : "";
        if (first[0]) k_snprintf(out, out_size, "Automatic (%s)", first);
        else          k_strlcpy(out, "Automatic", out_size);
        return 1;
    }
    if (index - 1 >= sound_device_count()) return 0;
    k_strlcpy(out, sound_device_label(index - 1), out_size);
    return 1;
}

static int device_apply(const char *value) {
    if (!value || !value[0]) return SETTING_INVALID;
    if (k_strcmp(value, DEVICE_AUTO) != 0) {
        int known = 0;
        for (int i = 0; i < sound_device_count(); i++)
            if (k_strcmp(sound_device_name(i), value) == 0) { known = 1; break; }
        // A device that is not plugged in is refused HERE and accepted
        // by sound_select() -- the difference is who is asking. A person
        // typing a name wants to be told it is not a device; a name
        // restored from /etc at boot may name a card that arrives later.
        if (!known) return SETTING_INVALID;
    }
    sound_select(value);
    return etc_config_set(SOUND_CONFIG_FILE, DEVICE_KEY, value)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static const char *device_unavailable(void) {
    return sound_device_count() ? 0 : "No sound device was found on this machine";
}

static const struct setting g_device_setting = {
    .name   = DEVICE_KEY,
    .label  = "Output device",
    .type   = SETTING_TYPE_ENUM,
    .file   = SOUND_CONFIG_FILE,
    .category = "Sound",
    .group  = "Output",
    .choice = device_choice,
    .choice_label = device_choice_label,
    .get    = device_get,
    .apply  = device_apply,
    .unavailable = device_unavailable,
};

void sound_config_setting_register(void) {
    setting_register(&g_volume_setting);
    setting_register(&g_device_setting);
}
