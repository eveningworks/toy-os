// Persists the keyboard layout selection across reboots -- the same
// "small key=value entry in the shared /etc/toyos.conf, read once at
// boot" pattern tz.c and font_config.c already use (see etc_config.h).
// No legacy pre-toyos.conf file to migrate from here (unlike
// tz.c/font_config.c) -- this setting was added after toyos.conf
// already existed, so it never had an earlier standalone-file form.
#include "keyboard_config.h"
#include "fs.h"
#include "string.h"
#include "etc_config.h"

#define KEYBOARD_CONFIG_FILE "/etc/toyos.conf"
#define KEYBOARD_CONFIG_KEY "keyboard_layout"

void keyboard_config_init(void) {
    char value[8];
    if (!etc_config_get(KEYBOARD_CONFIG_FILE, KEYBOARD_CONFIG_KEY, value, sizeof(value))) return;

    if (k_strcmp(value, "se") == 0) keyboard_set_layout(KB_LAYOUT_SE);
    else if (k_strcmp(value, "us") == 0) keyboard_set_layout(KB_LAYOUT_US);
    // anything else (typo from hand-editing toyos.conf) -- keep the
    // compiled-in KB_LAYOUT_US default rather than guessing
}

void keyboard_config_save(enum keyboard_layout layout) {
    etc_config_set(KEYBOARD_CONFIG_FILE, KEYBOARD_CONFIG_KEY, keyboard_layout_name(layout));
}
