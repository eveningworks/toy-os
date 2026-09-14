// Persists the console's font size selection across reboots, the same
// way tz.c persists the timezone city -- a small "font_size=<n>" key
// inside the shared /etc/toyos.conf every setting lives in by default,
// read/applied once at boot (see kernel.c's kernel_main():
// fs_mkdir("/etc") runs before any INIT_CONFIG initcall),
// through the shared reader/writer in kernel/lib/etc_config.c.
//
// `<n>` is a point size. It used to have to be one of the eight sizes
// tools/genttf.py bakes in; with a face loadable from /usr/share/fonts
// (font_face.h) it can be ANY size, so this parses a number rather than
// matching a name against the baked set. A second key, `font_face`,
// records which face -- or the literal `builtin` for the baked glyphs.
#include "font_config.h"
#include "gfx.h"
#include "font_faces.h" // the directory listing -- ring 0 parses no font
#include "fs.h"
#include "knum.h"
#include "kfmt.h"
#include "string.h"
#include "etc_config.h"
#include "setting.h"
#include "win_role.h" // win_server_font_changed() -- clients cache the metrics
#include "initcall.h"

#define FONT_CONFIG_FILE "/etc/toyos.conf"
#define FONT_CONFIG_KEY "font_size"
#define FACE_CONFIG_KEY "font_face"
#define MONO_CONFIG_KEY "font_mono"

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
// **PROPORTIONAL, because this is the INTERFACE face.** It was
// dejavu-sans-mono for as long as there was only one face setting and
// the Terminal had to share it -- which made the whole desktop look
// like a terminal, for a reason that was never about taste. With
// `font_mono` beside it (below) the constraint is gone.
//
// Liberation Sans because it is metric-compatible with Arial, ships in
// this tree already, and has a real bold companion -- so the bold
// weight exercises the LOADED path on every boot rather than the
// smeared fallback.
#define FACE_DEFAULT "liberation-sans"

// The MONOSPACE family's default: the face a Linux terminal has looked
// like for twenty years. A default that exercises the rasterizer on
// every boot is one that cannot silently rot -- the same argument that
// keeps one test booting -vga virtio.
#define MONO_DEFAULT "dejavu-sans-mono"

// THE FACE IS APPLIED BEFORE THE SIZE, and the order is not cosmetic:
// an arbitrary size (13, 32) is only rasterizable once a face is loaded,
// so a boot that read the size first would refuse it, snap to the
// nearest baked size, and then load the face -- leaving the machine at
// 14px with a perfectly good rasterizer and a config file saying 13.
void font_config_init(void) {
    char value[FONT_FACE_NAME_LEN];

    // **RING 0 NO LONGER LOADS A FACE.** It records which one is
    // SELECTED and nothing more: /bin/fontd reads the same setting,
    // parses the file and publishes the atlas the desktop draws from.
    // The console keeps the baked tables whatever this says.
    if (etc_config_get(FONT_CONFIG_FILE, FACE_CONFIG_KEY, value, sizeof(value)))
        font_faces_set_selected(value);
    else
        font_faces_set_selected(FACE_DEFAULT);

    if (etc_config_get(FONT_CONFIG_FILE, MONO_CONFIG_KEY, value, sizeof(value)))
        font_faces_set_selected_mono(value);
    else
        font_faces_set_selected_mono(MONO_DEFAULT);

    // **A SELECTED FACE STILL HAS TO BE BUILT, AND THAT IS WHAT A
    // MISSING SIZE KEY USED TO SKIP.** font_face_select() only loads and
    // validates the file; the atlas is built by gfx_set_font_px(). So a
    // machine with a `font_face` key (or the compiled-in default) and NO
    // `font_size` key -- which is every freshly formatted disk --
    // returned here having selected a face and built nothing, and drew
    // with the BAKED font while `fontface` reported the face as active.
    //
    // It hid well because it is invisible on any disk that has ever had
    // a size set: the key persists, so a developer's image and every
    // test that sets a size explicitly took the other branch. What it
    // cost was the whole runtime-font feature on a fresh image --
    // proportional advances, kerning and bold all silently fell back,
    // and "bold looks the same as regular" is exactly how it surfaced.
    uint32_t px = 0;
    if (etc_config_get(FONT_CONFIG_FILE, FONT_CONFIG_KEY, value, sizeof(value))
        && k_parse_u32(value, &px) && px > 0) {
        gfx_set_font_px((int)px);
    } else {
        // No size on record: build the face at whatever size is already
        // in effect, so the choice takes effect rather than being a
        // stored preference nothing acts on.
        gfx_set_font_px(gfx_font_px());
    }
}
INITCALL(font_config_init, INIT_CONFIG);

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
    // REFUSED IF THE FILE IS NOT THERE, which is as much validation as
    // ring 0 can honestly do now: whether a `.ttf` is well formed is a
    // question only the parser can answer, and the parser is in ring 3.
    // A face that will not parse is fontd's to report.
    if (!builtin && !font_faces_have(name)) return SETTING_INVALID;
    font_faces_set_selected(builtin ? FACE_BUILTIN : name);
    int r = font_config_save_face(builtin ? "" : name);
    // The clients cache metrics, and fontd will republish when it
    // notices the setting -- this is what makes them look again.
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
    // **THIS ACCEPTS ANY NUMBER AND THE REGISTRY DOES NOT, so a size
    // outside the list above never reaches here.** `setting_set()`
    // gates an ENUM on `choice_valid()` first, so `config set
    // system.font_size 13` is refused with "try one of: 8 10 12 ..."
    // even though the rasterizer would manage 13 perfectly well.
    //
    // The comment here used to claim the opposite -- that the list was
    // only what Settings SHOWS -- which was the intent when faces
    // became loadable and was never true of the registry. See
    // docs/bugs.md; the fix is a type that means "an INT with
    // suggestions", which this setting wants and none exists.
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
    return font_faces_name(index - 1, out, out_size);
}

static void face_get(char *out, uint32_t out_size) {
    k_strlcpy(out, font_faces_selected(), out_size);
}

static int face_apply(const char *value) {
    return font_config_apply_face(value);
}

static void mono_get(char *out, uint32_t out_size) {
    k_strlcpy(out, font_faces_selected_mono(), out_size);
}

// **REFUSES A PROPORTIONAL FACE? NO -- ring 0 cannot tell.** Whether
// every advance is equal is a question only the rasterizer can answer
// and the rasterizer is in ring 3, so this validates existence exactly
// as the UI face does and fontd reports the rest (its published header
// carries a `monospace` flag for precisely this).
static int mono_apply(const char *value) {
    int builtin = !value || !value[0] || k_strcmp(value, FACE_BUILTIN) == 0;
    if (!builtin && !font_faces_have(value)) return SETTING_INVALID;
    font_faces_set_selected_mono(builtin ? FACE_BUILTIN : value);
    int r = etc_config_set(FONT_CONFIG_FILE, MONO_CONFIG_KEY,
                           builtin ? FACE_BUILTIN : value)
                ? SETTING_SAVED : SETTING_UNSAVED;
    win_server_font_changed();
    return r;
}

static const struct setting g_mono_setting = {
    .name   = MONO_CONFIG_KEY,
    .label  = "Monospace",
    .type   = SETTING_TYPE_ENUM,
    .file   = FONT_CONFIG_FILE,
    .category = "Appearance",
    .group = "Fonts",
    .choice = face_choice,   // the same list; a face is a face
    .get    = mono_get,
    .apply  = mono_apply,
};

static const struct setting g_face_setting = {
    .name   = FACE_CONFIG_KEY,
    // "Interface face", not "Font face": with a monospace face beside
    // it the unqualified name says nothing about which of the two it
    // is. GNOME's Interface Text / Monospace Text pair.
    .label  = "Interface",
    .type   = SETTING_TYPE_ENUM,
    .file   = FONT_CONFIG_FILE,
    .category = "Appearance",
    .group = "Fonts",
    .choice = face_choice,
    .get    = face_get,
    .apply  = face_apply,
};

static const struct setting g_font_setting = {
    .name   = FONT_CONFIG_KEY,
    .label  = "Size",
    .type   = SETTING_TYPE_ENUM,
    .file   = FONT_CONFIG_FILE,
    .category = "Appearance",
    .group = "Fonts",
    .choice = font_choice,
    .get    = font_get,
    .apply  = font_apply,
};

void font_config_setting_register(void) {
    setting_register(&g_font_setting);
    setting_register(&g_face_setting);
    setting_register(&g_mono_setting);
}
