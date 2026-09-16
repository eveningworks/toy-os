// Cursor themes: loading the pointer's shapes from data files. See
// cursor_theme.h for the layering and why the masks carry coverage
// rather than colour.

#include "cursor_theme.h"
#include "wm/wm_fs.h"
#include "kapi.h"
#include "setting.h" // not part of the kapi.h umbrella -- see kernel/include/README.md
#include <stddef.h>
#include "wm/wm_log.h"
#include "wm/wm_conf.h"

#define CURSOR_DIR "/usr/share/cursors"
// The shared file every setting here lives in, per CLAUDE.md's rule
// that a setting joins /etc/toyos.conf unless it has enough keys of its
// own to be unwieldy there. Two is not enough.
#define CURSOR_CONFIG_FILE "/etc/toyos.conf"

static const char *const g_names[CURSOR_SHAPE_COUNT] = {
    "arrow", "resize-h", "resize-v", "resize-diag", "text", "wait",
    "resize-diag2",
};

const char *cursor_shape_name(int index) {
    if (index < 0 || index >= CURSOR_SHAPE_COUNT) return 0;
    return g_names[index];
}

static struct cursor_shape g_shapes[CURSOR_SHAPE_COUNT];
static char g_theme[SETTING_VALUE_MAX] = "default";
static int  g_scale = 1;
static uint32_t g_seen_generation;

// enum wm_cursor_kind's order is the WM's; this file's is the file
// format's. Mapping them explicitly rather than assuming they match
// means adding a shape to either cannot silently re-point the other.
static int kind_to_index(enum wm_cursor_kind kind) {
    switch (kind) {
        case WM_CURSOR_H:    return 1;
        case WM_CURSOR_V:    return 2;
        case WM_CURSOR_DIAG: return 3;
        case WM_CURSOR_TEXT: return 4;
        case WM_CURSOR_WAIT: return 5;
        case WM_CURSOR_DIAG2: return 6;
        default:             return 0;
    }
}

// --- the parser ------------------------------------------------------

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c == '.') return 0;
    return -1;
}

// One coverage character -> 0..255. The file stores 16 levels (see
// tools/gen_cursors.py), so 'f' must land on exactly 255 rather than
// 240: a fully opaque pixel that comes out 94% opaque makes every
// cursor faintly translucent, which looks like a blending bug.
static int cov_to_alpha(char c, unsigned char *out) {
    int v = hex_val(c);
    if (v < 0) return 0;
    *out = (unsigned char)(v * 255 / 15);
    return 1;
}

// The longest line worth holding. Comment and header lines run to ~60
// characters; a grid row is at most CURSOR_SHAPE_MAX. Sized for the
// former, because the first version sized it for the latter and every
// file then failed on its own comment header -- with the built-in
// fallback quietly keeping the pointer working, so nothing looked
// broken.
#define CURSOR_LINE_MAX 128

// A whole shape file: a short header plus two grids of at most
// CURSOR_SHAPE_MAX rows of CURSOR_SHAPE_MAX characters and a newline
// each (2 * 32 * 33 = 2112), with room to spare. A file bigger than
// this is refused by fs_read_into() rather than truncated, which the
// parser then reports as malformed -- the correct outcome for a shape
// file this loader cannot represent anyway.
#define CURSOR_FILE_MAX 4096

// Copies one line out, returning its FULL length (which may exceed what
// fits) and advancing `*pos` past the newline. -1 at end of input.
//
// Returning the full length rather than the copied length is what lets
// the caller refuse a grid row that is too long instead of silently
// accepting its first 127 characters -- while an over-long COMMENT is
// harmlessly skipped. A parser here rejects rather than guesses, but
// only about the thing it is parsing.
static int next_line(const char *text, uint32_t len, uint32_t *pos,
                      char *out, uint32_t out_size) {
    if (*pos >= len) return -1;
    uint32_t n = 0, full = 0;
    while (*pos < len && text[*pos] != '\n') {
        if (n + 1 < out_size) out[n++] = text[*pos];
        full++;
        (*pos)++;
    }
    if (*pos < len) (*pos)++; // the newline
    out[n] = '\0';
    return (int)full;
}

static int parse_grid(const char *text, uint32_t len, uint32_t *pos,
                       int w, int h, unsigned char grid[][CURSOR_SHAPE_MAX]) {
    char line[CURSOR_LINE_MAX];
    for (int y = 0; y < h; y++) {
        int n = next_line(text, len, pos, line, sizeof line);
        if (n != w) return 0; // a short or long row is a malformed file
        for (int x = 0; x < w; x++)
            if (!cov_to_alpha(line[x], &grid[y][x])) return 0;
    }
    return 1;
}

// Parses straight INTO `out` rather than into a local. Two reasons, both
// practical: the struct is a couple of KB, so a local would put two of
// them on the kernel stack and make the copy out a `memcpy` call this
// kernel has no symbol for (it link-failed exactly that way once); and
// `loaded` is set only on the last line, so a failed parse leaves the
// destination unloaded, which is precisely the fall-back-to-built-in
// state the caller wants. The trap that follows: `out` is scribbled on
// during a parse that may still fail, so never hand this a shape that
// is currently in use.
int cursor_shape_parse(const char *text, uint32_t len, struct cursor_shape *out) {
    if (!text || !out) return 0;

    struct cursor_shape *sp = out;
    k_memset(sp, 0, sizeof *sp);
    int w = 0, h = 0, hx = 0, hy = 0;
    int have_outline = 0, have_fill = 0;

    uint32_t pos = 0;
    char line[CURSOR_LINE_MAX];
    for (;;) {
        int n = next_line(text, len, &pos, line, sizeof line);
        if (n < 0) break;
        if (n == 0 || line[0] == '#') continue;

        if (k_strncmp(line, "width=", 6) == 0) {
            if (!k_parse_u32(line + 6, (uint32_t *)&w)) return 0;
        } else if (k_strncmp(line, "height=", 7) == 0) {
            if (!k_parse_u32(line + 7, (uint32_t *)&h)) return 0;
        } else if (k_strncmp(line, "hotspot=", 8) == 0) {
            const char *comma = k_strchr(line + 8, ',');
            if (!comma) return 0;
            char xs[8];
            uint32_t xn = (uint32_t)(comma - (line + 8));
            if (xn >= sizeof xs) return 0;
            k_memcpy(xs, line + 8, xn);
            xs[xn] = '\0';
            if (!k_parse_u32(xs, (uint32_t *)&hx)) return 0;
            if (!k_parse_u32(comma + 1, (uint32_t *)&hy)) return 0;
        } else if (k_strcmp(line, "outline=") == 0 || k_strcmp(line, "fill=") == 0) {
            // Both grids come AFTER the dimensions, always -- a grid
            // read before them has no idea how wide a row should be, so
            // the only safe answer is to refuse.
            if (w <= 0 || h <= 0 || w > CURSOR_SHAPE_MAX || h > CURSOR_SHAPE_MAX)
                return 0;
            int is_outline = (line[0] == 'o');
            if (!parse_grid(text, len, &pos, w, h,
                             is_outline ? sp->outline : sp->fill))
                return 0;
            if (is_outline) have_outline = 1; else have_fill = 1;
        }
        // Unknown keys (`shape=`, and anything a later version adds) are
        // ignored rather than refused, so an older WM can still read a
        // newer theme's files.
    }

    if (!have_outline || !have_fill) return 0;
    if (hx < 0 || hx >= w || hy < 0 || hy >= h) return 0; // hotspot off the shape

    sp->w = w; sp->h = h; sp->hot_x = hx; sp->hot_y = hy;
    sp->loaded = 1; // last, so a failure above leaves it unloaded
    return 1;
}

// --- loading ---------------------------------------------------------

// Constructed, not caller-supplied: CURSOR_DIR "/<theme>/<shape>", so
// it is bounded by its own shape rather than by FS_PATH_MAX.
#define CURSOR_PATH_MAX 192

static int load_one(const char *theme, int index) {
    char path[CURSOR_PATH_MAX];
    if (!k_snprintf(path, sizeof path, "%s/%s/%s", CURSOR_DIR, theme,
                     g_names[index]))
        return 0;
    // A theme legitimately need not carry every shape, so "absent" is a
    // normal outcome and silent. A shape that is THERE and still fails
    // to load is not, and used to be equally silent -- which is how an
    // intermittent read failure hid behind the built-in fallback for as
    // long as it did. Say which step failed.
    if (!wm_fs_exists(path)) return 0;

    // Read into OUR OWN buffer, not fs_read()'s shared staging one.
    //
    // This used to parse straight out of fs_read()'s buffer, with a
    // comment reasoning that the parse happens before anything else
    // touches the filesystem. That is true of this function and not of
    // the machine: the kernel context is preemptible, so a ring-3
    // process reading a file mid-parse swapped the buffer's contents
    // and the parse walked somebody else's data. A shape that fails to
    // parse falls back to the built-in one, so the damage was invisible
    // -- it showed up only as "5 of 6 shapes loaded" on roughly one
    // boot in three under KVM.
    // static, not a 4 KiB stack frame in the WM's own context -- and
    // safe as a static precisely because this loader is the only thing
    // that touches it, which is the property fs_read()'s shared buffer
    // could not offer.
    static char body[CURSOR_FILE_MAX];
    uint32_t n = wm_fs_read_into(path, body, sizeof body);
    if (n == 0) {
        wm_logf("cursor: %s exists but READ FAILED (size %u) -- "
                    "using the built-in shape\n", path, (uint32_t)wm_fs_size(path));
        return 0;
    }

    if (!cursor_shape_parse(body, n, &g_shapes[index])) {
        wm_logf("cursor: %s is malformed -- using the built-in shape\n", path);
        return 0;
    }
    return 1;
}

int cursor_theme_load(const char *theme) {
    for (int i = 0; i < CURSOR_SHAPE_COUNT; i++) g_shapes[i].loaded = 0;
    if (!theme || !theme[0]) return 0;

    int loaded = 0;
    for (int i = 0; i < CURSOR_SHAPE_COUNT; i++)
        if (load_one(theme, i)) loaded++;

    wm_logf("cursor: theme \"%s\" -- %d of %d shapes loaded\n",
                theme, loaded, CURSOR_SHAPE_COUNT);
    return loaded;
}

const struct cursor_shape *cursor_theme_shape(enum wm_cursor_kind kind) {
    const struct cursor_shape *s = &g_shapes[kind_to_index(kind)];
    return s->loaded ? s : 0;
}

int cursor_theme_scale(void) { return g_scale; }

// --- the two settings -------------------------------------------------
//
// Both are PERSIST-ONLY (`apply` is NULL): the registry writes the file
// and this file notices via setting_generation(). That is the shape
// `setting.h` documents for a setting owned outside the kernel, and it
// is what the ring-3 WM will need after Milestone 41 -- registering an
// apply callback here would have to be undone then.

// The two settings' DESCRIPTORS live in the kernel now
// (kernel/lib/cursor_theme_config.c). setting_register() takes function
// pointers and a ring-3 process cannot supply one, so a setting owned
// here would need the kernel to call back into ring 3 -- the inversion
// this whole milestone exists to avoid.
//
// It does not need to, because both are persist-only: the registry
// validates and writes to /etc, and this file notices by watching
// setting_generation() below. The kernel owns the DESCRIPTION, the
// compositor owns the BEHAVIOUR.

// Reads both keys and applies them. Shared by init and poll so the
// startup path and the live-change path cannot interpret a value
// differently -- which is the drift that makes a setting "work until
// you reboot".
static void adopt_settings(void) {
    char val[SETTING_VALUE_MAX];

    // ONE read for BOTH keys. wm_conf_get() re-reads the whole document
    // per call, and this runs on every settings generation bump -- which
    // moves for any setting anywhere, so the common case is two whole-file
    // reads to discover that nothing about the cursor changed.
    //
    // static, not a 4 KiB stack frame: a ring-3 stack is 16 KiB with one
    // guard page below it, so a local of this size steps clean over the
    // guard (etc_config.h says so). Safe as a static because the WM is
    // one event loop and this does not recurse -- the same property the
    // shape loader's `body` relies on.
    static struct etc_config_buf conf;
    int have = wm_conf_load(CURSOR_CONFIG_FILE, &conf);

    int scale = 1;
    if (have && etc_config_buf_get(&conf, "cursor_size", val, sizeof val)) {
        if (k_strcmp(val, "large") == 0) scale = 2;
        else if (k_strcmp(val, "huge") == 0) scale = 3;
    }
    g_scale = scale;

    if (!have || !etc_config_buf_get(&conf, "cursor_theme", val, sizeof val) || !val[0])
        k_strlcpy(val, "default", sizeof val);
    if (k_strcmp(val, g_theme) != 0 || !g_shapes[0].loaded) {
        k_strlcpy(g_theme, val, sizeof g_theme);
        cursor_theme_load(g_theme);
    }
    // The plane's sprite is built FROM these shapes at this scale --
    // whatever it shows is stale the moment either changed.
    wm_hwcursor_invalidate();
}

void cursor_theme_poll(void) {
    uint32_t gen = wm_setting_generation();
    if (gen == g_seen_generation) return; // one compare, no I/O
    g_seen_generation = gen;
    adopt_settings();
}

void cursor_theme_init(void) {
    g_seen_generation = wm_setting_generation();
    adopt_settings();
}
