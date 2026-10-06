// The inside of a .zip as a folder -- "zip:<archive>[/<folder in it>]",
// a uui_fileview source over lib/uzip.h. Explorer's compressed folders
// and KDE's zip:/ worker: opening an archive lists it, its folders open
// in place, and it is READ-ONLY -- nothing is renamed, deleted or pasted
// inside; what is wanted comes out with Extract (a job, fm_jobs.c).
//
// An archive stores only full names ("a/b/c.txt"), so the folders of a
// level are DERIVED from the names below it, explicit folder records or
// not -- unzip(1) and every shell do the same.
#include "fm_internal.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <caltime.h>
#include "kpath.h"
#include "lib/uzip.h"
#include "lib/human.h"
#include "lib/udate.h"
#include "ui/uui_table.h"
#include "ui/ulog.h"

#define ZIP_MAX PANE_FILES

// What the current listing holds, by index -- one listing for both panes,
// as the bin and the results have.
static struct sys_dirent g_ent[ZIP_MAX];      // the rows, our own copy
static unsigned long g_packed[ZIP_MAX];
static char g_zpath[ZIP_MAX][PATH_MAX_LEN];   // each row's own "zip:..." name
static int g_n;

// "zip:/home/x.zip/a/b" -> archive "/home/x.zip", inner "a/b". The
// archive ends at the first ".zip" followed by '/' or the end.
int zip_split(const char *dir, char *archive, int acap, const char **inner) {
    if (strncmp(dir, FM_ZIP, sizeof FM_ZIP - 1)) return 0;
    const char *p = dir + sizeof FM_ZIP - 1;
    for (const char *s = p; *s; s++)
        if (!strncasecmp(s, ".zip", 4) && (s[4] == '/' || !s[4])) {
            int n = (int)(s + 4 - p);
            if (n >= acap) return 0;
            memcpy(archive, p, (size_t)n);
            archive[n] = '\0';
            *inner = s[4] ? s + 5 : s + 4;
            return 1;
        }
    return 0;
}

int is_zip_name(const char *path) {
    size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".zip");
}

struct fill {
    const char *inner;     // "" at the archive's top, else "a/b"
    size_t ilen;
    const char *dir;       // the view's own name, rows' paths start with it
    struct sys_dirent *out;
    int cap;
};

static void dos_to_rtc(unsigned date, unsigned time, struct rtc_time *t) {
    t->year = (uint16_t)(1980 + (date >> 9));
    t->month = (uint8_t)((date >> 5) & 15);
    t->day = (uint8_t)(date & 31);
    t->hour = (uint8_t)(time >> 11);
    t->minute = (uint8_t)((time >> 5) & 63);
    t->second = (uint8_t)((time & 31) * 2);
}

static int add(void *ctx, const char *name, unsigned long size, unsigned long packed,
               unsigned dos_date, unsigned dos_time) {
    struct fill *f = ctx;
    if (f->ilen) {
        if (strncmp(name, f->inner, f->ilen) || name[f->ilen] != '/') return 0;
        name += f->ilen + 1;
    }
    if (!name[0]) return 0;                       // the folder's own record
    const char *slash = strchr(name, '/');
    int len = slash ? (int)(slash - name) : (int)strlen(name);
    if (len <= 0 || len >= (int)sizeof f->out[0].name) return 0;
    for (int i = 0; i < g_n; i++)                 // a folder seen already
        if (f->out[i].is_dir && !strncmp(f->out[i].name, name, (size_t)len) && !f->out[i].name[len]) {
            g_packed[i] += packed;
            return 0;
        }
    if (g_n >= f->cap || g_n >= ZIP_MAX) return 1;
    struct sys_dirent *e = &f->out[g_n];
    memset(e, 0, sizeof *e);
    memcpy(e->name, name, (size_t)len);
    e->name[len] = '\0';
    e->is_dir = slash != 0;
    e->size = e->is_dir ? 0 : (uint32_t)size;
    dos_to_rtc(dos_date, dos_time, &e->modified);
    g_packed[g_n] = packed;
    snprintf(g_zpath[g_n], PATH_MAX_LEN, "%s/%s", f->dir, e->name);
    g_ent[g_n] = *e;
    g_n++;
    return 0;
}

static int z_list(void *ctx, const char *dir, struct sys_dirent *out, int cap) {
    (void)ctx;
    char archive[PATH_MAX_LEN], err[96];
    const char *inner;
    g_n = 0;
    if (!zip_split(dir, archive, sizeof archive, &inner)) return -EINVAL;
    // The view's name without a trailing '/', so rows join cleanly.
    char base[PATH_MAX_LEN];
    strlcpy(base, dir, sizeof base);
    size_t bl = strlen(base);
    if (bl && base[bl - 1] == '/') base[bl - 1] = '\0';
    struct fill f = { inner, strlen(inner), base, out, cap };
    if (f.ilen && inner[f.ilen - 1] == '/') f.ilen--;
    int rc = uzip_list(archive, add, &f, err, sizeof err);
    if (rc < 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "%s", err);
        ulogf("files: zip %s: %s\n", archive, err);
        return rc;
    }
    return g_n;
}

static int z_path(void *ctx, int i, char *out, int cap) {
    (void)ctx;
    return i >= 0 && i < g_n && (int)strlcpy(out, g_zpath[i], (size_t)cap) < cap;
}

static const struct uui_table_column zip_cols[] = {
    { "Name",     0,  UUI_TALIGN_LEFT  },
    { "Size",     9,  UUI_TALIGN_RIGHT },
    { "Modified", 20, UUI_TALIGN_LEFT  },
    { "Packed",   9,  UUI_TALIGN_RIGHT },
};

static void z_cell(void *ctx, int i, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (i < 0 || i >= g_n) return;
    const struct sys_dirent *e = &g_ent[i];
    if (col == 1 && !e->is_dir) human_size(out, (unsigned long)cap, e->size);
    else if (col == 2) udate_format(out, (unsigned long)cap, &e->modified, UDATE_DATE | UDATE_TIME);
    else if (col == 3) human_size(out, (unsigned long)cap, g_packed[i]);
}

static int z_compare(void *ctx, int a, int b, int col) {
    (void)ctx;
    const struct sys_dirent *x = &g_ent[a], *y = &g_ent[b];
    if (col == 1) return x->size < y->size ? -1 : x->size > y->size;
    if (col == 3) return g_packed[a] < g_packed[b] ? -1 : g_packed[a] > g_packed[b];
    uint64_t tx = cal_rtc_to_epoch(&x->modified), ty = cal_rtc_to_epoch(&y->modified);
    return tx < ty ? -1 : tx > ty;
}

// Up inside an archive: the folder above in it, and from its top the
// folder the archive is in -- "zip:/home" names nothing.
static int z_up(void *ctx, const char *dir, char *out, int cap) {
    (void)ctx;
    char archive[PATH_MAX_LEN];
    const char *inner;
    if (!zip_split(dir, archive, sizeof archive, &inner)) return 0;
    if (!inner[0]) return k_path_dirname(archive, out, (size_t)cap);
    if (snprintf(out, (size_t)cap, "%s", dir) >= cap) return 0;
    char *s = strrchr(out, '/');
    if (!s || s < out + sizeof FM_ZIP - 1 + strlen(archive)) return 0;
    *s = '\0';
    return 1;
}

static const struct uui_fileview_source g_src = {
    .list = z_list, .path = z_path,
    .cols = zip_cols, .ncols = 4,
    .cell = z_cell, .compare = z_compare,
    .descends = 1, .up = z_up,
};

const struct uui_fileview_source *zip_source(void) { return &g_src; }
int in_zip(const struct uui_fileview *fv) { return uui_fileview_source(fv) == &g_src; }

// Read-only: nothing is made, renamed, cut, pasted or deleted inside.
// Copy to the other pane (F5) EXTRACTS there, so it stays.
int zip_item_flags(int code, unsigned *out) {
    if (!in_zip(active())) return 0;
    switch (code) {
    case CMD_MENU_NEW: case CMD_MKDIR: case CMD_NEW_FILE: case CMD_CLIP_PASTE:
    case CMD_CLIP_CUT: case CMD_CLIP_COPY: case CMD_RENAME: case CMD_MOVE:
    case CMD_DELETE: case CMD_DELETE_FOREVER: case CMD_EDIT: case CMD_PROPERTIES:
        *out = UUI_MI_DISABLED;
        return 1;
    case CMD_EXTRACT_SEL:
        *out = operand_count() > 0 ? 0 : UUI_MI_DISABLED;
        return 1;
    default:
        return 0;
    }
}

// Where Extract puts things by default: beside the archive, in a folder
// named for it -- "x.zip" -> "x", then "x (2)" if that is taken.
// Explorer's and Ark's default.
int zip_default_dest(const char *archive, char *out, int cap) {
    char dir[PATH_MAX_LEN], stem[64];
    if (!k_path_dirname(archive, dir, sizeof dir)) return 0;
    strlcpy(stem, k_path_basename(archive), sizeof stem);
    size_t l = strlen(stem);
    if (l > 4) stem[l - 4] = '\0';
    struct sys_stat st;
    for (int n = 1; n < 100; n++) {
        char name[80];
        if (n == 1) snprintf(name, sizeof name, "%s", stem);
        else snprintf(name, sizeof name, "%s (%d)", stem, n);
        if (!k_path_join(dir, name, out, (size_t)cap)) return 0;
        if (sys_stat(out, &st) != 0) return 1;
    }
    return 0;
}
