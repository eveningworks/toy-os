// Persists the console's font size selection across reboots, the same
// way tz.c persists the timezone city -- a small "font_size=<n>" key
// inside the shared /etc/toyos.conf every setting lives in by default,
// read/applied once at boot (see kernel.c's kernel_main():
// fs_mkdir("/etc") runs before either tz_init() or font_config_init()),
// through the shared reader/writer in kernel/lib/etc_config.c.
//
// `<n>` is a point size. It used to have to be one of the eight sizes
// tools/genttf.py bakes in; with a face loadable from /usr/share/fonts
// (font_face.h) it can be ANY size, so this parses a number rather than
// matching a name against the baked set. A second key, `font_face`,
// records which face -- or the literal `builtin` for the baked glyphs.
#include "font_config.h"
#include "gfx.h"
#include "font_face.h"
#include "knum.h"
#include "kfmt.h"
#include "string.h"
#include "etc_config.h"
#include "setting.h"
#include "win_server.h" // win_server_font_changed() -- clients cache the metrics

#define FONT_CONFIG_FILE "/etc/toyos.conf"
#define FONT_CONFIG_KEY "font_size"
#define FACE_CONFIG_KEY "font_face"

// The name a face-less machine reports and persists. Not a face name --
// no file may be called this -- so "the baked font" is a value the
// setting can hold and round-trip rather than an absence that reads as
// a missing key.
#define FACE_BUILTIN "builtin"

// What a machine with no `font_face` key uses, when the file is there.
//
// **A NAME, NOT A GUARANTEE.** If /usr/share/fonts/dejavu-sans-mono.ttf
// is missing -- a live boot, an image built from a tree with an empty
// data/fonts/ -- this quietly comes to nothing and the baked font
// draws, which is the whole point of the baked font. Compiled in rather
// than seeded into /etc because there is no seeded toyos.conf: every
// setting here gets its default from code, and a first boot has no
// config file to read.
//
// DejaVu Sans Mono because it is the face a Linux terminal has looked
// like for twenty years, and because a default that exercises the
// rasterizer on every boot is a default that cannot silently rot -- the
// same argument that keeps one test booting -vga virtio.
#define FACE_DEFAULT "dejavu-sans-mono"

// THE FACE IS APPLIED BEFORE THE SIZE, and the order is not cosmetic:
// an arbitrary size (13, 32) is only rasterizable once a face is loaded,
// so a boot that read the size first would refuse it, snap to the
// nearest baked size, and then load the face -- leaving the machine at
// 14px with a perfectly good rasterizer and a config file saying 13.
void font_config_init(void) {
    char value[FONT_FACE_NAME_LEN];

    font_face_init(); // scan /usr/share/fonts -- a listing, nothing opened

    if (etc_config_get(FONT_CONFIG_FILE, FACE_CONFIG_KEY, value, sizeof(value))) {
        // A face that has gone missing (an image reseeded without it) is
        // not an error worth stopping for: the baked font is right here.
        if (k_strcmp(value, FACE_BUILTIN) != 0) font_face_select(value);
    } else {
        font_face_select(FACE_DEFAULT); // absent key: the default, if it exists
    }

    if (!etc_config_get(FONT_CONFIG_FILE, FONT_CONFIG_KEY, value, sizeof(value))) return;
    uint32_t px = 0;
    if (k_parse_u32(value, &px) && px > 0) gfx_set_font_px((int)px);
}

int font_config_save(enum font_size size) {
    (void)size;
    return font_config_save_px(gfx_font_px());
}

// Persists whatever size is actually in effect, as a number. The old
// entry point took a baked enum and is kept because Settings and the
// setting registry both speak in choices; this one is what the shell's
// `fontsize <n>` reaches, and both write the same key.
int font_config_save_px(int px) {
    char buf[16];
    k_snprintf(buf, sizeof buf, "%d", px);
    return etc_config_set(FONT_CONFIG_FILE, FONT_CONFIG_KEY, buf)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

// --- the one place a font change is APPLIED for the machine ---------
//
// Set it, persist it, and tell every GUI client its cached metrics are
// stale (WIN_EV_FONT). Both entry points that change the font for real
// -- the shell's `fontsize`/`fontface` and the two registered settings
// -- go through here, so there is ONE place that can forget the
// notification rather than one per caller. A KTEST that changes the font
// and puts it back deliberately does NOT come through here: it has no
// business repainting the user's desktop twice.
int font_config_apply_px(int px) {
    if (!gfx_set_font_px(px)) return SETTING_INVALID;
    int r = font_config_save_px(gfx_font_px());
    win_server_font_changed();
    return r;
}

int font_config_apply_face(const char *name) {
    int builtin = !name || !name[0] || k_strcmp(name, FACE_BUILTIN) == 0;
    if (!font_face_select(builtin ? "" : name)) return SETTING_INVALID;
    // Re-rasterize at the size already in effect. A face swap that left
    // the old face's atlas in use would look like the setting had
    // silently failed.
    gfx_set_font_px(gfx_font_px());
    int r = font_config_save_face(builtin ? "" : name);
    win_server_font_changed();
    return r;
}

int font_config_save_face(const char *name) {
    return etc_config_set(FONT_CONFIG_FILE, FACE_CONFIG_KEY,
                          (name && name[0]) ? name : FACE_BUILTIN)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

// --- the registry descriptor (see setting.h) -------------------------
//
// The choice list is derived from gfx_font_size_name() for every baked
// size, for the same reason name_to_size() above matches against it: a
// second list of size names would need keeping in step with
// font_ttf_variants[] and would silently stop matching if one moved.

static int font_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)FONT_SIZE_COUNT) return 0;
    k_strlcpy(out, gfx_font_size_name((enum font_size)index), out_size);
    return 1;
}

static void font_get(char *out, uint32_t out_size) {
    k_strlcpy(out, gfx_font_size_name(gfx_font_size()), out_size);
}

static int font_apply(const char *value) {
    // Any number is legal now, not only a baked one -- the choice list
    // above is what Settings SHOWS, not what the setting accepts, and
    // `config set system.font_size 13` is a reasonable thing to type
    // once a face can be rasterized at 13.
    uint32_t px = 0;
    if (!k_parse_u32(value, &px) || px == 0) return SETTING_INVALID;
    return font_config_apply_px((int)px);
}

// --- the font FACE setting -------------------------------------------
//
// Choice 0 is always the baked font, so a machine with no font files
// still has a valid two-state... one-state list, and so "go back to the
// built-in" is reachable from Settings rather than only by editing
// /etc/toyos.conf.

static int face_choice(int index, char *out, uint32_t out_size) {
    if (index == 0) { k_strlcpy(out, FACE_BUILTIN, out_size); return 1; }
    struct font_face_info info;
    if (!font_face_info(index - 1, &info)) return 0;
    k_strlcpy(out, info.name, out_size);
    return 1;
}

static void face_get(char *out, uint32_t out_size) {
    const char *active = font_face_active();
    k_strlcpy(out, active[0] ? active : FACE_BUILTIN, out_size);
}

static int face_apply(const char *value) {
    return font_config_apply_face(value);
}

static const struct setting g_face_setting = {
    .name   = FACE_CONFIG_KEY,
    .label  = "Font face",
    .type   = SETTING_TYPE_ENUM,
    .file   = FONT_CONFIG_FILE,
    .category = "Appearance",
    .choice = face_choice,
    .get    = face_get,
    .apply  = face_apply,
};

static const struct setting g_font_setting = {
    .name   = FONT_CONFIG_KEY,
    .label  = "Font size",
    .type   = SETTING_TYPE_ENUM,
    .file   = FONT_CONFIG_FILE,
    .category = "Appearance",
    .choice = font_choice,
    .get    = font_get,
    .apply  = font_apply,
};

void font_config_setting_register(void) {
    setting_register(&g_font_setting);
    setting_register(&g_face_setting);
}
