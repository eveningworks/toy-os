// The two settings `/bin/netheal` reads. See api/netheal_config.h for
// what it does and why it is not a fix.
//
// PERSISTED AND NOT APPLIED, like the network-time settings beside
// them: nothing in this kernel acts on a change, and `netheal` reads
// them once at start. They still declare an `apply`, for the reason
// ntp_config.c gives -- a persist-only setting stores an unvalidated
// string, and `config set system.net_recover banana` would then read
// back as neither on nor off to the one program that cares.
#include "netheal_config.h"
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "knum.h"
#include "kfmt.h"

#define NETHEAL_CONFIG_FILE "/etc/toyos.conf"
#define NETHEAL_CATEGORY    "Network"
#define NETHEAL_GROUP       "Recovery"

static int onoff_choice(int index, char *out, uint32_t cap) {
    if (index == 0) { k_strlcpy(out, "on", cap); return 1; }
    if (index == 1) { k_strlcpy(out, "off", cap); return 1; }
    return 0;
}

static void net_recover_get(char *out, uint32_t cap) {
    // OFF BY DEFAULT, and deliberately: a machine that reboots itself
    // unasked is alarming on a desktop, and this is for a headless test
    // machine whose only fault symptom is being unreachable.
    if (!etc_config_get(NETHEAL_CONFIG_FILE, "net_recover", out, cap))
        k_strlcpy(out, "off", cap);
}

static int net_recover_apply(const char *value) {
    if (k_strcmp(value, "on") != 0 && k_strcmp(value, "off") != 0) return SETTING_INVALID;
    return etc_config_set(NETHEAL_CONFIG_FILE, "net_recover", value)
           ? SETTING_SAVED : SETTING_UNSAVED;
}

static const struct setting g_net_recover = {
    .name  = "net_recover",
    .label = "Reboot if the network never comes up",
    .type  = SETTING_TYPE_ENUM,
    .file  = NETHEAL_CONFIG_FILE,
    .category = NETHEAL_CATEGORY,
    .group    = NETHEAL_GROUP,
    .choice = onoff_choice,
    .get   = net_recover_get,
    .apply = net_recover_apply,
};

static void net_recover_wait_get(char *out, uint32_t cap) {
    if (!etc_config_get(NETHEAL_CONFIG_FILE, "net_recover_wait", out, cap))
        k_strlcpy(out, "45", cap);
}

static int net_recover_wait_apply(const char *value) {
    uint32_t v = 0;
    // LONG ENOUGH FOR DHCP, short enough to be worth having. The
    // measured lease on the test machine arrives ~10 s after boot, so
    // the floor is well clear of a healthy boot being called a failure.
    if (!k_parse_u32(value, &v) || v < 5 || v > 600) return SETTING_INVALID;
    return etc_config_set(NETHEAL_CONFIG_FILE, "net_recover_wait", value)
           ? SETTING_SAVED : SETTING_UNSAVED;
}

static const struct setting g_net_recover_wait = {
    .name  = "net_recover_wait",
    .label = "Seconds to wait for an address",
    .type  = SETTING_TYPE_INT,
    .min   = 5,
    .max   = 600,
    .unit  = "s",
    .file  = NETHEAL_CONFIG_FILE,
    .category = NETHEAL_CATEGORY,
    .group    = NETHEAL_GROUP,
    .get   = net_recover_wait_get,
    .apply = net_recover_wait_apply,
};

void netheal_setting_register(void) {
    setting_register(&g_net_recover);
    setting_register(&g_net_recover_wait);
}
