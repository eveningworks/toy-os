// See cursor_config.h. Deliberately the thinnest possible copy of
// font_config.c's shape -- read one key, parse it through the owning
// subsystem's own parser, apply; write it back on change. Everything
// that knows what a cursor style IS lives in vga.c.
#include "cursor_config.h"
#include "etc_config.h"

#define CURSOR_CONFIG_FILE "/etc/toyos.conf"
#define CURSOR_CONFIG_KEY "cursor_style"

void cursor_config_init(void) {
    char value[16];
    if (!etc_config_get(CURSOR_CONFIG_FILE, CURSOR_CONFIG_KEY, value, sizeof(value))) return;
    enum vga_cursor_style want;
    // An unrecognised name leaves the default in place rather than
    // failing the boot -- same tolerance every other /etc reader here
    // has for a value it doesn't understand.
    if (vga_cursor_style_parse(value, &want)) vga_set_cursor_style(want);
}

void cursor_config_save(enum vga_cursor_style style) {
    if (style >= VGA_CURSOR_STYLE_COUNT) return;
    etc_config_set(CURSOR_CONFIG_FILE, CURSOR_CONFIG_KEY, VGA_CURSOR_STYLE_NAMES[style]);
}
