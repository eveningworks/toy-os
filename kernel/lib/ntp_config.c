// The three network-time settings. See api/ntp_config.h for why the
// kernel holds them while `/bin/ntpd` does the work.
//
// All three are PERSISTED AND NOT APPLIED, in the sense that nothing in
// this kernel acts on a change -- `ntpd` re-reads them each cycle. They
// still declare an `apply` rather than leaving it NULL, because the
// registry writes an unvalidated string for a persist-only setting and
// `config set system.ntp banana` would then be stored and read back as
// neither on nor off by the one program that cares.
#include "ntp_config.h"
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "knum.h"
#include "kfmt.h"

#define NTP_CONFIG_FILE "/etc/toyos.conf"
#define NTP_CATEGORY    "Time & Locale"
// A GROUP OF ITS OWN, not the timezone's page. A timezone is where this
// machine is; network time is where its clock comes from. They share a
// category because both are about the clock, and separating the pages
// is what keeps `system.timezone` a single-control page -- which is the
// shape System Settings gives a setting that declares no group.
#define NTP_GROUP       "Network Time"

// ---- system.ntp ------------------------------------------------------

static int onoff_choice(int index, char *out, uint32_t cap) {
    if (index == 0) { k_strlcpy(out, "on", cap); return 1; }
    if (index == 1) { k_strlcpy(out, "off", cap); return 1; }
    return 0;
}

static void ntp_get(char *out, uint32_t cap) {
    // OFF IS THE DEFAULT, and it is a deliberate choice rather than an
    // absent one: turning it on makes this machine send a packet to a
    // host on the internet, and nothing here should start doing that
    // because somebody installed it. The same reasoning ships telnetd
    // and tftpd disabled.
    if (!etc_config_get(NTP_CONFIG_FILE, "ntp", out, cap)) k_strlcpy(out, "off", cap);
}

static int ntp_apply(const char *value) {
    if (k_strcmp(value, "on") != 0 && k_strcmp(value, "off") != 0) return SETTING_INVALID;
    return etc_config_set(NTP_CONFIG_FILE, "ntp", value) ? SETTING_SAVED : SETTING_UNSAVED;
}

// On the Date & time page, beside the manual Change... it switches off
// (Windows 11's "Set time automatically"); the server and the interval
// keep the Network Time page.
static const struct setting g_ntp_setting = {
    .name = "ntp",
    .label = "Set the time automatically",
    .type = SETTING_TYPE_ENUM,
    .file = NTP_CONFIG_FILE,
    .category = NTP_CATEGORY,
    .group = "Date & time",
    .choice = onoff_choice,
    .get = ntp_get,
    .apply = ntp_apply,
};

// ---- system.ntp_server -----------------------------------------------

static void server_get(char *out, uint32_t cap) {
    if (!etc_config_get(NTP_CONFIG_FILE, "ntp_server", out, cap))
        k_strlcpy(out, NTP_DEFAULT_SERVER, cap);
}

static int server_apply(const char *value) {
    // A HOST, WHICH IS EITHER A NAME OR A DOTTED QUAD, and this refuses
    // neither shape -- resolving is `/bin/ntpd`'s job and a name this
    // kernel cannot check today may resolve tomorrow. What it does
    // refuse is the empty string and anything carrying whitespace: the
    // /etc parser is key=value to end of line, so an embedded space
    // would be stored and read back as a different string than was
    // typed, which is worse than a refusal.
    if (!value[0]) return SETTING_INVALID;
    for (const char *p = value; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '\n') return SETTING_INVALID;
    return etc_config_set(NTP_CONFIG_FILE, "ntp_server", value) ? SETTING_SAVED
                                                                : SETTING_UNSAVED;
}

static const struct setting g_server_setting = {
    .name = "ntp_server",
    .label = "Time server",
    .type = SETTING_TYPE_STRING,
    .file = NTP_CONFIG_FILE,
    .category = NTP_CATEGORY,
    .group = NTP_GROUP,
    .get = server_get,
    .apply = server_apply,
};

// ---- system.ntp_interval ---------------------------------------------

static void interval_get(char *out, uint32_t cap) {
    if (!etc_config_get(NTP_CONFIG_FILE, "ntp_interval", out, cap))
        k_snprintf(out, cap, "%d", NTP_DEFAULT_INTERVAL);
}

static int interval_apply(const char *value) {
    // The registry has already bounded this against min/max below -- an
    // INT setting is range-checked before apply() is reached. What is
    // left is rejecting a value that is not a number at all, which the
    // range check reads as zero and would then refuse for the wrong
    // stated reason.
    for (const char *p = value; *p; p++)
        if (*p < '0' || *p > '9') return SETTING_INVALID;
    if (!value[0]) return SETTING_INVALID;
    return etc_config_set(NTP_CONFIG_FILE, "ntp_interval", value) ? SETTING_SAVED
                                                                  : SETTING_UNSAVED;
}

static const struct setting g_interval_setting = {
    .name = "ntp_interval",
    .label = "Sync every",
    .type = SETTING_TYPE_INT,
    .file = NTP_CONFIG_FILE,
    .category = NTP_CATEGORY,
    .group = NTP_GROUP,
    .min = NTP_INTERVAL_MIN,
    .max = NTP_INTERVAL_MAX,
    .step = 15,
    .unit = "min",
    .get = interval_get,
    .apply = interval_apply,
};

void ntp_setting_register(void) {
    setting_register(&g_ntp_setting);
    setting_register(&g_server_setting);
    setting_register(&g_interval_setting);
}
