// Cursor themes: loading the pointer's shapes from data files. See
// cursor_theme.h for the layering and the two kinds of shape.

#include "cursor_theme.h"
#include "wm/wm_fs.h"
#include "kapi.h"
#include "setting.h" // not part of the kapi.h umbrella -- see kernel/include/README.md
#include <stddef.h>
#include "wm/wm_log.h"
#include "wm/wm_conf.h"

// The shared file every setting here lives in, per CLAUDE.md's rule
// that a setting joins /etc/toyos.conf unless it has enough keys of its
// own to be unwieldy there. Two is not enough.
#define CURSOR_CONFIG_FILE "/etc/toyos.conf"

static const char *const g_names[CURSOR_SHAPE_COUNT] = {
    "arrow", "resize-h", "resize-v", "resize-diag", "text", "wait",
    "resize-diag2", "hand", "move", "not-allowed",
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
        case WM_CURSOR_HAND: return 7;
        case WM_CURSOR_MOVE: return 8;
        case WM_CURSOR_NOT_ALLOWED: return 9;
        default:             return 0;
    }
}

// --- loading ---------------------------------------------------------

// Constructed, not caller-supplied: UCURSOR_DIR "/<theme>/<shape>", so
// it is bounded by its own shape rather than by FS_PATH_MAX.
#define CURSOR_PATH_MAX 192

static int load_one(const char *theme, int index) {
    char path[CURSOR_PATH_MAX];
    if (!k_snprintf(path, sizeof path, "%s/%s/%s", UCURSOR_DIR, theme,
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

    struct cursor_shape *s = &g_shapes[index];
    if (!cursor_shape_parse(body, n, s)) {
        wm_logf("cursor: %s is malformed -- using the built-in shape\n", path);
        return 0;
    }
    char why[CURSOR_PATH_MAX + 64];
    if (s->is_image && !ucursor_decode(theme, s, g_scale, why, sizeof why)) {
        wm_logf("cursor: %s -- %s\n", path, why);
        s->loaded = 0;
        return 0;
    }
    return 1;
}

int cursor_theme_load(const char *theme) {
    for (int i = 0; i < CURSOR_SHAPE_COUNT; i++) ucursor_release(&g_shapes[i]);
    if (!theme || !theme[0]) return 0;

    int loaded = 0, images = 0, native = 0;
    for (int i = 0; i < CURSOR_SHAPE_COUNT; i++) {
        if (!load_one(theme, i)) continue;
        loaded++;
        if (g_shapes[i].is_image) {
            images++;
            if (g_shapes[i].baked == g_scale) native++;
        }
    }

    // How many images came RENDERED for this size rather than scaled up
    // -- what tells a test the 2x file was used, not the 1x doubled.
    if (images)
        wm_logf("cursor: theme \"%s\" -- %d of %d shapes loaded, %d of %d images "
                "rendered for %dx\n", theme, loaded, CURSOR_SHAPE_COUNT, native, images, g_scale);
    else
        wm_logf("cursor: theme \"%s\" -- %d of %d shapes loaded\n",
                theme, loaded, CURSOR_SHAPE_COUNT);
    return loaded;
}

const struct cursor_shape *cursor_theme_shape(enum wm_cursor_kind kind) {
    const struct cursor_shape *s = &g_shapes[kind_to_index(kind)];
    if (s->loaded) return s;
    if (kind == WM_CURSOR_HAND || kind == WM_CURSOR_MOVE || kind == WM_CURSOR_NOT_ALLOWED)
        return g_shapes[0].loaded ? &g_shapes[0] : 0;
    return 0;
}

int cursor_theme_scale(void) { return g_scale; }

int cursor_shape_step(const struct cursor_shape *s) {
    int b = s->baked > 0 ? s->baked : 1;
    int st = g_scale / b;
    return st > 0 ? st : 1;
}

uint32_t cursor_shape_argb(const struct cursor_shape *s, int dx, int dy,
                            uint32_t fill_rgb) {
    return ucursor_pixel(s, dx, dy, cursor_shape_step(s), fill_rgb);
}

// --- the two settings -------------------------------------------------
//
// Both are PERSIST-ONLY (`apply` is NULL): the registry writes the file
// and this file notices via setting_generation(). That is the shape
// `setting.h` documents for a setting owned outside the kernel, and it
// is what the ring-3 WM will need after Milestone 41 -- registering an
// apply callback here would have to be undone then.

// The two settings' DESCRIPTORS are data files
// (/etc/settings.d/system.cursor_theme and system.cursor_size). Both
// are persist-only: the registry validates and writes to /etc, and this
// file notices by watching setting_generation() below. The registry
// owns the DESCRIPTION, the compositor owns the BEHAVIOUR.

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
    int rescaled = scale != g_scale;
    g_scale = scale;

    if (!have || !etc_config_buf_get(&conf, "cursor_theme", val, sizeof val) || !val[0])
        k_strlcpy(val, "default", sizeof val);
    // A new SIZE reloads too: an image theme's rendering for 2x is a
    // different file from its 1x one.
    if (k_strcmp(val, g_theme) != 0 || !g_shapes[0].loaded || rescaled) {
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
