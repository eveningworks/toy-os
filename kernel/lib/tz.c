// WHICH TIMEZONE IS SELECTED, and nothing else.
//
// The city database, the DST rules and every conversion are RING 3's
// now (userland/libc/tz.c). What is left in the kernel is one
// registered setting whose value is a city NAME and whose choice list
// is `/etc/timezones` read as an opaque list of lines -- setting.h's
// `choice_file`, which enumerates a list the registry knows nothing
// about. Ring 0 does no time arithmetic with it.
//
// THE INVARIANT THAT REPLACED THE OLD ONE: the kernel's clock is UTC
// and every filesystem timestamp is a UTC epoch, so a local time is
// something a ring-3 caller computes and never something the kernel
// hands out. `rtc_read_local()` is gone with the rules it applied.
//
// Two files are still involved and it is worth keeping them straight:
// `/etc/timezones` is the DATABASE (shipped in data/etc, staged by
// `make iso`), and `/etc/toyos.conf`'s `timezone=<city>` key is the
// SELECTION. Only the second is written here.

#include "tz.h"
#include "setting.h"
#include "etc_config.h"
#include "string.h"

#define TZ_DB_FILE     "/etc/timezones"
#define TZ_CONFIG_FILE "/etc/toyos.conf"
#define TZ_CONFIG_KEY  "timezone"

// The value with no selection ever made. It is a real row of the
// database rather than an empty string, so a client always has
// something to show and the offset it implies is zero.
#define TZ_DEFAULT "utc"

static void tz_get(char *out, uint32_t out_size) {
    if (!etc_config_get(TZ_CONFIG_FILE, TZ_CONFIG_KEY, out, out_size))
        k_strlcpy(out, TZ_DEFAULT, out_size);
}

static int tz_apply(const char *value) {
    // NOT VALIDATED HERE. `setting_set()` refuses a value that is not
    // one of the choices before it reaches this, which is the whole
    // point of the list being declared rather than checked by hand.
    return etc_config_set(TZ_CONFIG_FILE, TZ_CONFIG_KEY, value)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static const struct setting g_tz_setting = {
    .name        = TZ_CONFIG_KEY,
    .label       = "Time zone",
    .type        = SETTING_TYPE_ENUM,
    .file        = TZ_CONFIG_FILE,
    .category    = "Time & Locale",
    .group       = "Date & time",
    .choice_file = TZ_DB_FILE,
    .get         = tz_get,
    .apply       = tz_apply,
};

void tz_setting_register(void) { setting_register(&g_tz_setting); }
