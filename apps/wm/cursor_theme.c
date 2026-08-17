// Cursor themes: loading the pointer's shapes from data files. See
// cursor_theme.h for the layering and why the masks carry coverage
// rather than colour.

#include "cursor_theme.h"
#include "kapi.h"
#include "setting.h" // not part of the kapi.h umbrella -- see kernel/include/README.md
#include <stddef.h>

#define CURSOR_DIR "/usr/share/cursors"
// The shared file every setting here lives in, per CLAUDE.md's rule
// that a setting joins /etc/toyos.conf unless it has enough keys of its
// own to be unwieldy there. Two is not enough.
#define CURSOR_CONFIG_FILE "/etc/toyos.conf"

static const char *const g_names[CURSOR_SHAPE_COUNT] = {
    "arrow", "resize-h", "resize-v", "resize-diag", "text", "wait",
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

static int load_one(const char *theme, int index) {
    char path[FS_PATH_MAX];
    if (!k_snprintf(path, sizeof path, "%s/%s/%s", CURSOR_DIR, theme,
                     g_names[index]))
        return 0;
    if (!fs_exists(path)) return 0;

    // fs_read() hands back a pointer into the backend's own staging
    // buffer, valid only until the next fs_read()/fs_write() -- so the
    // parse has to happen before anything else touches the filesystem,
    // which it does (cursor_shape_parse() is pure).
    uint32_t n = 0;
    const char *body = fs_read(path, &n);
    if (!body || n == 0) return 0;

    if (!cursor_shape_parse(body, n, &g_shapes[index])) {
        klog_printf("cursor: %s is malformed -- using the built-in shape\n", path);
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

    klog_printf("cursor: theme \"%s\" -- %d of %d shapes loaded\n",
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

// fs_list() is a callback walk with no per-call context pointer, so the
// index being looked for and the name found have to travel through
// these. Single-threaded and used only inside theme_choice(), which is
// the only reason that is acceptable.
static int g_want_index;
static int g_seen_index;
static char g_found_name[SETTING_VALUE_MAX];

static void theme_choice_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    if (!is_dir) return;
    if (g_seen_index == g_want_index) k_strlcpy(g_found_name, name,
                                                 sizeof g_found_name);
    g_seen_index++;
}

static int theme_choice(int index, char *out, uint32_t out_size) {
    // Computed from the directory, not a compiled-in list, so dropping a
    // theme in gives it a Control Panel row with no code change -- the
    // same rule the keyboard layouts and the Start menu already follow.
    g_want_index = index;
    g_seen_index = 0;
    g_found_name[0] = '\0';
    fs_list(CURSOR_DIR, theme_choice_cb);
    if (!g_found_name[0]) return 0;
    k_strlcpy(out, g_found_name, out_size);
    return 1;
}

static void theme_get(char *out, uint32_t out_size) {
    k_strlcpy(out, g_theme, out_size);
}

static const char *const g_sizes[] = { "normal", "large", "huge" };

static int size_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)(sizeof g_sizes / sizeof g_sizes[0])) return 0;
    k_strlcpy(out, g_sizes[index], out_size);
    return 1;
}

static void size_get(char *out, uint32_t out_size) {
    k_strlcpy(out, g_sizes[g_scale - 1 < 0 ? 0 :
                            (g_scale > 3 ? 2 : g_scale - 1)], out_size);
}

static const struct setting g_theme_setting = {
    .name = "cursor_theme",
    .label = "Cursor theme",
    .type = SETTING_TYPE_ENUM,
    .file = CURSOR_CONFIG_FILE,
    .choice = theme_choice,
    .get = theme_get,
    .apply = 0, // persist-only; picked up by cursor_theme_poll()
};

static const struct setting g_size_setting = {
    .name = "cursor_size",
    .label = "Cursor size",
    .type = SETTING_TYPE_ENUM,
    .file = CURSOR_CONFIG_FILE,
    .choice = size_choice,
    .get = size_get,
    .apply = 0,
};

// Reads both keys and applies them. Shared by init and poll so the
// startup path and the live-change path cannot interpret a value
// differently -- which is the drift that makes a setting "work until
// you reboot".
static void adopt_settings(void) {
    char val[SETTING_VALUE_MAX];

    int scale = 1;
    if (etc_config_get(CURSOR_CONFIG_FILE, "cursor_size", val, sizeof val)) {
        if (k_strcmp(val, "large") == 0) scale = 2;
        else if (k_strcmp(val, "huge") == 0) scale = 3;
    }
    g_scale = scale;

    if (!etc_config_get(CURSOR_CONFIG_FILE, "cursor_theme", val, sizeof val) || !val[0])
        k_strlcpy(val, "default", sizeof val);
    if (k_strcmp(val, g_theme) != 0 || !g_shapes[0].loaded) {
        k_strlcpy(g_theme, val, sizeof g_theme);
        cursor_theme_load(g_theme);
    }
}

void cursor_theme_poll(void) {
    uint32_t gen = setting_generation();
    if (gen == g_seen_generation) return; // one compare, no I/O
    g_seen_generation = gen;
    adopt_settings();
}

void cursor_theme_init(void) {
    setting_register(&g_theme_setting);
    setting_register(&g_size_setting);
    g_seen_generation = setting_generation();
    adopt_settings();
}
