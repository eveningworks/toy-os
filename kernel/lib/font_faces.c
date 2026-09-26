// Which faces exist, and which one is selected. See api/font_faces.h --
// this is a directory listing and a string, and ring 0 opens no font.
#include "font_faces.h"
#include "fs.h"
#include "string.h"
#include <stddef.h>

#define FACES_MAX 8
#define SUFFIX     ".ttf"
#define BOLD_TAIL  "-bold"

static char g_name[FACES_MAX][FONT_FACE_NAME_LEN];
static int  g_count;
static uint64_t g_scanned_gen = (uint64_t)-1;
static char g_selected[FONT_FACE_NAME_LEN] = "builtin";
static char g_selected_mono[FONT_FACE_NAME_LEN] = "builtin";

// A scan collects into the CALL's own struct and publishes it in one copy
// that does not yield: the walk itself can (fs.h's fs_list()), and two
// scans writing straight into the cache would interleave their results.
struct face_scan {
    char name[FACES_MAX][FONT_FACE_NAME_LEN];
    int count;
};

static void scan_cb(void *ctx, const char *name, uint32_t size, int is_dir) {
    struct face_scan *x = ctx;
    (void)size;
    if (is_dir || !name || x->count >= FACES_MAX) return;

    int len = (int)k_strlen(name);
    int slen = (int)k_strlen(SUFFIX);
    if (len <= slen || k_strcmp(name + len - slen, SUFFIX) != 0) return;

    char base[FONT_FACE_NAME_LEN];
    int keep = len - slen;
    if (keep >= (int)sizeof base) return;
    for (int i = 0; i < keep; i++) base[i] = name[i];
    base[keep] = '\0';

    // THE BOLD MEMBER IS NOT A FACE. `x-bold.ttf` is the bold weight of
    // the family `x`, so listing it would offer the same family twice
    // and let somebody select a weight as though it were a typeface.
    int blen = (int)k_strlen(BOLD_TAIL);
    if (keep > blen && k_strcmp(base + keep - blen, BOLD_TAIL) == 0) return;

    for (int i = 0; i < x->count; i++)
        if (k_strcmp(x->name[i], base) == 0) return;
    k_strlcpy(x->name[x->count++], base, FONT_FACE_NAME_LEN);
}

// RESCANS WHEN THE FILESYSTEM HAS MOVED, so a font copied in shows up
// without a reboot -- and costs one comparison when it has not.
static void refresh(void) {
    uint64_t gen = fs_generation();
    if (gen == g_scanned_gen) return;
    struct face_scan x;   // no initialiser: that is a memset, and there is none here
    x.count = 0;
    fs_list(FONT_FACE_DIR, scan_cb, &x);
    for (int i = 0; i < x.count; i++) k_strlcpy(g_name[i], x.name[i], FONT_FACE_NAME_LEN);
    g_count = x.count;
    g_scanned_gen = gen;   // last: a scan that never finished is rescanned
}

int font_faces_count(void) { refresh(); return g_count; }

int font_faces_name(int index, char *out, uint32_t cap) {
    refresh();
    if (index < 0 || index >= g_count || !out) return 0;
    k_strlcpy(out, g_name[index], cap);
    return 1;
}

int font_faces_have(const char *name) {
    refresh();
    if (!name || !name[0]) return 0;
    for (int i = 0; i < g_count; i++)
        if (k_strcmp(g_name[i], name) == 0) return 1;
    return 0;
}

const char *font_faces_selected(void) { return g_selected; }

void font_faces_set_selected(const char *name) {
    k_strlcpy(g_selected, (name && name[0]) ? name : "builtin", sizeof g_selected);
}

// The MONOSPACE family's selection, kept beside the UI one. Separate
// storage rather than an array indexed by family because these two are
// reached by name from a dozen places and `[0]`/`[1]` at every call site
// would say less than the two names do.
const char *font_faces_selected_mono(void) { return g_selected_mono; }

void font_faces_set_selected_mono(const char *name) {
    k_strlcpy(g_selected_mono, (name && name[0]) ? name : "builtin",
              sizeof g_selected_mono);
}
