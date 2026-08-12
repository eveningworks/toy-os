// Persists the console's font size selection across reboots, the same
// way tz.c persists the timezone city -- a small "font_size=<n>" key
// inside the shared /etc/toyos.conf every setting lives in by default,
// read/applied once at boot (see kernel.c's kernel_main():
// fs_mkdir("/etc") runs before either tz_init() or font_config_init()),
// through the shared reader/writer in kernel/core/etc_config.c.
// `name_to_size()` is the only bit of logic here that isn't generic
// /etc plumbing.
//
// `<n>` is a point size (8/10/12/14/16/18/20/24, see font_ttf.h) as of
// build 347 -- it used to be a name (tiny/small/medium/large).
#include "font_config.h"
#include "gfx.h"
#include "string.h"
#include "etc_config.h"

#define FONT_CONFIG_FILE "/etc/toyos.conf"
#define FONT_CONFIG_KEY "font_size"

// Matches against gfx_font_size_name() for every baked size rather than
// a hand-maintained list of names -- since build 347 those names are
// just point-size numbers ("8".."24", see font_ttf.h/tools/genttf.py),
// so this stays correct automatically if a size is ever added/removed
// without needing a second list kept in sync with font_ttf_variants[].
static enum font_size name_to_size(const char *name) {
    for (enum font_size i = 0; i < FONT_SIZE_COUNT; i++) {
        if (k_strcmp(name, gfx_font_size_name(i)) == 0) return i;
    }
    return FONT_SIZE_COUNT; // sentinel: not a recognized size name
}

void font_config_init(void) {
    char value[16];
    if (!etc_config_get(FONT_CONFIG_FILE, FONT_CONFIG_KEY, value, sizeof(value))) return;
    enum font_size want = name_to_size(value);
    if (want != FONT_SIZE_COUNT) gfx_set_font_size(want);
}

void font_config_save(enum font_size size) {
    etc_config_set(FONT_CONFIG_FILE, FONT_CONFIG_KEY, gfx_font_size_name(size));
}
