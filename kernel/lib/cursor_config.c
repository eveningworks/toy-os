// See cursor_config.h. Deliberately the thinnest possible copy of
// font_config.c's shape -- read one key, parse it through the owning
// subsystem's own parser, apply; write it back on change. Everything
// that knows what a cursor style IS lives in vga.c.
#include "cursor_config.h"
#include "etc_config.h"
#include "setting.h"
#include "string.h"
#include "initcall.h"

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
INITCALL(cursor_config_init, INIT_CONFIG);

int cursor_config_save(enum vga_cursor_style style) {
    if (style >= VGA_CURSOR_STYLE_COUNT) return SETTING_INVALID;
    return etc_config_set(CURSOR_CONFIG_FILE, CURSOR_CONFIG_KEY, VGA_CURSOR_STYLE_NAMES[style])
               ? SETTING_SAVED : SETTING_UNSAVED;
}

// --- the registry descriptor (see setting.h) -------------------------
//
// Still true of this file that everything knowing what a cursor style
// IS lives in vga.c: the choice list is VGA_CURSOR_STYLE_NAMES and the
// parse is vga_cursor_style_parse(), so adding a style needs no edit
// here and gains a Control Panel option for free.

static int cursor_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)VGA_CURSOR_STYLE_COUNT) return 0;
    k_strlcpy(out, VGA_CURSOR_STYLE_NAMES[index], out_size);
    return 1;
}

static void cursor_get(char *out, uint32_t out_size) {
    k_strlcpy(out, VGA_CURSOR_STYLE_NAMES[vga_cursor_style()], out_size);
}

static int cursor_apply(const char *value) {
    enum vga_cursor_style want;
    if (!vga_cursor_style_parse(value, &want)) return SETTING_INVALID;
    vga_set_cursor_style(want);
    return cursor_config_save(want);
}

static const struct setting g_cursor_setting = {
    .name   = CURSOR_CONFIG_KEY,
    .label  = "Cursor",
    .type   = SETTING_TYPE_ENUM,
    .file   = CURSOR_CONFIG_FILE,
    .category = "Appearance",
    .group = "Console",
    .choice = cursor_choice,
    .get    = cursor_get,
    .apply  = cursor_apply,
};

void cursor_config_setting_register(void) { setting_register(&g_cursor_setting); }
