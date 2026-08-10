// Persists the console's font size selection across reboots, the same
// way tz.c persists the timezone city -- a small file under /etc read
// once at boot (see kernel.c's kernel_main(): fs_mkdir("/etc") runs
// before either tz_init() or font_config_init()).
//
// As of the build that added etc_config.h (see CHANGELOG), this reads
// and writes a "font_size=<n>" key inside the shared /etc/toyos.conf
// every setting lives in by default, through the shared reader/writer
// in kernel/core/etc_config.c -- not its own hand-rolled parser
// against its own dedicated file anymore (that was /etc/fontsize,
// "font_size=<n>" with the same shape it still has, just its own
// file). This file used to note that a second setting wanting the
// same key=value treatment would be the point to factor out a shared
// parser (tz.c's /etc/timezone was bare text, not key=value, so it
// didn't count as that second caller by itself) -- that's exactly
// what happened once tz.c *also* wanted key=value framing to move into
// the same shared file, so `name_to_size()` is now the only bit of
// logic left here that isn't generic /etc plumbing.
//
// `<n>` is a point size (8/10/12/14/16/18/20/24, see font_ttf.h) as of
// build 347 -- it used to be a name (tiny/small/medium/large).
#include "font_config.h"
#include "gfx.h"
#include "fs.h"
#include "string.h"
#include "etc_config.h"

#define FONT_CONFIG_FILE "/etc/toyos.conf"
#define FONT_CONFIG_KEY "font_size"
#define LEGACY_FONT_CONFIG_FILE "/etc/fontsize" // pre-toyos.conf location -- see font_config_init()

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
    if (etc_config_get(FONT_CONFIG_FILE, FONT_CONFIG_KEY, value, sizeof(value))) {
        enum font_size want = name_to_size(value);
        if (want != FONT_SIZE_COUNT) gfx_set_font_size(want);
        return;
    }

    // Not in /etc/toyos.conf yet -- the old dedicated /etc/fontsize
    // file was already "font_size=<n>", the same shape, just its own
    // file -- so it can be read through the same etc_config_get()
    // rather than a separate bare-text path like tz.c's legacy
    // migration needs. Migrate it if present rather than silently
    // falling back to the compiled-in default.
    if (!etc_config_get(LEGACY_FONT_CONFIG_FILE, FONT_CONFIG_KEY, value, sizeof(value))) return;

    enum font_size want = name_to_size(value);
    if (want == FONT_SIZE_COUNT) return;
    gfx_set_font_size(want);
    etc_config_set(FONT_CONFIG_FILE, FONT_CONFIG_KEY, value);
    fs_delete(LEGACY_FONT_CONFIG_FILE);
}

void font_config_save(enum font_size size) {
    etc_config_set(FONT_CONFIG_FILE, FONT_CONFIG_KEY, gfx_font_size_name(size));
}
