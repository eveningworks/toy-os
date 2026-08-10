// Persists the console's font size selection across reboots, the same
// way tz.c persists the timezone city -- a small file under /etc read
// once at boot (see kernel.c's kernel_main(): fs_mkdir("/etc") runs
// before either tz_init() or font_config_init()).
//
// Unlike /etc/timezone (bare text, just the city name), /etc/fontsize
// is "font_size=<n>" -- the user asked for a config file that reads
// like one (`cat /etc/fontsize` showing `font_size=12` rather than
// just `12`), anticipating more settings living under /etc someday.
// `<n>` is a point size (8/10/12/14/16/18/20/24, see font_ttf.h) as of
// build 347 -- it used to be a name (tiny/small/medium/large); the file
// format and this parser didn't need to change for that, only the set
// of values gfx_font_size_name() can return did (see name_to_size()).
// This is deliberately still a single-purpose parser, not a shared
// key=value config format every setting reads/writes through: the
// smallest change that gets the requested format for this one setting.
// If a second setting wants the same treatment, that's the point to
// factor out a real line-based key=value reader shared across files --
// not before, per this codebase's usual "wait for a second real caller"
// rule (see apps/widgets.h's/apps/theme.h's top comments for the same
// reasoning applied elsewhere).
#include "font_config.h"
#include "gfx.h"
#include "fs.h"
#include "string.h"

#define FONT_CONFIG_FILE "/etc/fontsize"
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
    uint32_t size = 0;
    // fs_write() always NUL-terminates at data[size] (see fs.c), so
    // `data` is safe to scan as a plain C string here, same as tz.c
    // does with /etc/timezone.
    const char *data = fs_read(FONT_CONFIG_FILE, &size);
    if (!data || size == 0) return; // no config yet -- keep the compiled-in default

    const char *eq = 0;
    for (const char *p = data; *p; p++) {
        if (*p == '=') { eq = p; break; }
    }
    if (!eq) return; // malformed -- ignore rather than guess

    char value[16];
    unsigned i = 0;
    for (const char *p = eq + 1; *p && *p != '\n' && *p != '\r' && i < sizeof(value) - 1; p++, i++) {
        value[i] = *p;
    }
    value[i] = '\0';

    enum font_size want = name_to_size(value);
    if (want != FONT_SIZE_COUNT) gfx_set_font_size(want);
}

void font_config_save(enum font_size size) {
    const char *name = gfx_font_size_name(size);

    char buf[32];
    k_strcpy(buf, FONT_CONFIG_KEY "=");
    unsigned klen = (unsigned)k_strlen(buf);
    unsigned i = 0;
    for (; name[i] && klen + i < sizeof(buf) - 2; i++) buf[klen + i] = name[i];
    buf[klen + i] = '\n';
    buf[klen + i + 1] = '\0';

    fs_write(FONT_CONFIG_FILE, buf, 0);
}
