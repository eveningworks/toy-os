// Persists the keyboard layout selection across reboots -- the same
// "small key=value entry in the shared /etc/toyos.conf, read once at
// boot" pattern tz.c and font_config.c already use (see etc_config.h).
// No legacy pre-toyos.conf file to migrate from here (unlike
// tz.c/font_config.c) -- this setting was added after toyos.conf
// already existed, so it never had an earlier standalone-file form.
#include "keyboard_config.h"
#include "keyboard_layout.h"
#include "fs.h"
#include "string.h"
#include "etc_config.h"

#define KEYBOARD_CONFIG_FILE "/etc/toyos.conf"
#define KEYBOARD_CONFIG_KEY "keyboard_layout"

void keyboard_config_init(void) {
    char value[KB_LAYOUT_NAME_MAX];
    // Always call keyboard_layout_load() -- even with no persisted
    // value -- so the layout tables are actually populated by the
    // time this returns. keyboard_layout_load()'s own fallback chain
    // (requested name -> /etc/kbs/us -> compiled-in US) means passing
    // "us" here when nothing's persisted yet does exactly the right
    // thing either way.
    if (!etc_config_get(KEYBOARD_CONFIG_FILE, KEYBOARD_CONFIG_KEY, value, sizeof(value))) {
        keyboard_layout_load("us");
        return;
    }
    keyboard_layout_load(value);
}

void keyboard_config_save(const char *name) {
    etc_config_set(KEYBOARD_CONFIG_FILE, KEYBOARD_CONFIG_KEY, name);
}
