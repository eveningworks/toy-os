// What the kernel log still puts on the console, as a registered
// setting -- Linux's console_loglevel, persisted.
//
// NAMED RATHER THAN NUMBERED, unlike the `loglevel=` boot flag, which
// takes Linux's digit because that is what someone types at a GRUB
// prompt from memory. A settings page is the opposite situation: the
// reader is choosing, not recalling, and `warnings` says what `4` does
// not. Both ends meet at klog_set_console_level().
//
// PRECEDENCE: the boot flag applies before the first line is logged,
// this applies at INIT_CONFIG, so a stored value wins for everything
// after early boot -- which is the order Linux has too (`loglevel=`
// then sysctl). A flag typed to debug a boot still covers the boot.
#include "klog_config.h"
#include "klog.h"
#include "etc_config.h"
#include "setting.h"
#include "string.h"
#include "initcall.h"

#define KLOG_CONFIG_FILE "/etc/toyos.conf"
#define KLOG_LEVEL_KEY   "loglevel"

// The names a person picks from, and what each one lets through. Every
// level at or below the value reaches the console.
struct named_level { const char *name; int value; };

static const struct named_level LEVELS[] = {
    { "errors",   KLOG_LEVEL_ERR   },  // and the crit line before a panic
    { "warnings", KLOG_LEVEL_WARN  },
    { "normal",   KLOG_LEVEL_INFO  },  // the default
    { "debug",    KLOG_LEVEL_DEBUG },
};
#define LEVEL_COUNT ((int)(sizeof LEVELS / sizeof LEVELS[0]))

static int index_of_value(int value) {
    for (int i = 0; i < LEVEL_COUNT; i++)
        if (LEVELS[i].value == value) return i;
    return 2;  // "normal" -- a level with no name is reported as the default
}

static int find_level(const char *name) {
    for (int i = 0; i < LEVEL_COUNT; i++)
        if (k_strcmp(LEVELS[i].name, name) == 0) return i;
    return -1;
}

static struct etc_config_buf g_cfg;

void klog_config_init(void) {
    char value[16];
    etc_config_load(KLOG_CONFIG_FILE, &g_cfg);
    if (etc_config_buf_get(&g_cfg, KLOG_LEVEL_KEY, value, sizeof value)) {
        int i = find_level(value);
        if (i >= 0) klog_set_console_level(LEVELS[i].value);
    }
}
INITCALL(klog_config_init, INIT_CONFIG);

// --- the registry descriptor (see setting.h) -------------------------

static int level_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= LEVEL_COUNT) return 0;
    k_strlcpy(out, LEVELS[index].name, out_size);
    return 1;
}

static void level_get(char *out, uint32_t out_size) {
    k_strlcpy(out, LEVELS[index_of_value(klog_console_level())].name, out_size);
}

static int level_apply(const char *value) {
    int i = find_level(value);
    if (i < 0) return SETTING_INVALID;
    klog_set_console_level(LEVELS[i].value);
    return etc_config_set(KLOG_CONFIG_FILE, KLOG_LEVEL_KEY, LEVELS[i].name)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static const struct setting g_level_setting = {
    .name   = KLOG_LEVEL_KEY,
    .label  = "Kernel messages on the console",
    .type   = SETTING_TYPE_ENUM,
    .file   = KLOG_CONFIG_FILE,
    .category = "System",
    .group  = "Logging",
    .choice = level_choice,
    .get    = level_get,
    .apply  = level_apply,
};

void klog_config_setting_register(void) {
    setting_register(&g_level_setting);
}
