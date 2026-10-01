// Image Viewer -- a ring-3 client that shows a decoded picture.
//
// IT BROWSES A DIRECTORY RATHER THAN OPENING A FILE. A file-open dialog
// belongs to the application in this system, and a picture viewer is
// used by stepping through a folder -- so the window IS the browser: the
// pictures of one directory along a filmstrip at the bottom, the
// selected one filling the stage above it. Windows Photos' filmstrip and
// Gwenview's thumbnail bar; Left/Right step, a click jumps.
//
// THE STAGE IS TINTED BY THE PICTURE (YouTube's "ambient mode"): the
// average and the darker quarter of the picture's own colours make a
// radial ground behind it, the filmstrip a dark shade of the same, and
// the chrome a faint wash of it. Computed from a 16x9 sample when a
// decode lands, so it costs nothing per frame.
//
// It is handed a directory (or a file, whose directory it lists) on the
// command line, defaulting to /usr/share/wallpapers -- which is also
// where "Set as wallpaper" points the desktop.
//
// THE DECODE IS THIS PROCESS'S, on a worker thread (lib/uimg.h says why
// a decoder is a ring-3 library); the filmstrip's thumbnails come from
// lib/uthumb.h, the File Manager's cache, with its copy on disk.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "rt/sys.h"
#include "lib/uimg.h"
#include "lib/ufile.h"
#include "lib/usetting.h"
#include "lib/uthumb.h"
#include "lib/dirsort.h"
#include "kpath.h"   // k_path_join/_dirname -- the KERNEL's, linked into ring 3
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_thumbstrip.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_button.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "keyboard.h"

#define PATH_MAX_LEN 64          // NOT FS_PATH_MAX (4096 since
                                 // 2026-09-15) -- this app's own
                                 // buffers, still the old bound.
                                 // See docs/roadmap.md.
#define MAX_FILES 64
#define WALLPAPER_DIR "/usr/share/wallpapers"
#define DEFAULT_DIR WALLPAPER_DIR

enum { ID_MENU = 1, ID_TOOLBAR, ID_STRIP, ID_IMAGE, ID_WP_FILL, ID_WP_FIT };

enum {
    CMD_OPEN = 1, CMD_RELOAD, CMD_EXIT,
    CMD_PREV, CMD_NEXT,
    CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_FIT, CMD_ACTUAL,
    CMD_ROT_L, CMD_ROT_R,
    CMD_WALLPAPER, CMD_WALLPAPER_FIT, CMD_WALLPAPER_FILL, CMD_WALLPAPER_NONE,
    CMD_SLIDESHOW, CMD_PROPS,
};

enum { USER_DECODED = 1, USER_THUMB };

static struct uapp *g_app;
static char g_dir[PATH_MAX_LEN] = DEFAULT_DIR;
static char g_want[PATH_MAX_LEN];   // the file named on the command line

// The folder's pictures, sorted as /bin/ls sorts (lib/dirsort.h).
static struct sys_dirent g_entries[MAX_FILES];
static int g_count, g_cur = -1;

static struct uimg g_img;      // the decoded picture, owned here
static struct uimg g_rimg;     // ...turned, when g_rot is not 0
static int g_have_img, g_rot;

static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
static struct uui_thumbstrip g_strip;
static struct uui_image g_view;
static struct uui_button g_wp_fill, g_wp_fit;
static struct uui_statusbar g_status;

static char g_stat_name[PATH_MAX_LEN];
static char g_stat_dims[48];
static char g_stat_size[80];    // the dimensions and the file size, one pane
static char g_stat_file[24];
static char g_stat_note[96];
static char g_stat_zoom[16];     // "Fit  36%", "150%"
static char g_info_detail[80];   // "JPEG, baseline, 4:2:0"
static unsigned long long g_decode_ms;

static int g_props;              // the Properties panel is open
static int g_slide, g_paused;    // the slideshow
static unsigned long long g_slide_t0;
static int g_slide_ms = 5000;

// Where things were drawn this frame -- the stage for pan and wheel, the
// slideshow pill's buttons for its clicks.
static int g_sx, g_sy, g_sw, g_sh;
static int g_px0, g_py0, g_pw, g_ph;     // the properties panel
struct hit { int x, y, w, h; };
static struct hit g_pill_prev, g_pill_play, g_pill_next, g_pill_every;
static int g_drag, g_drag_x, g_drag_y, g_dragged, g_mx = -1, g_my = -1;

// --- the ambient colours ------------------------------------------------

struct ambient { uint32_t centre, edge, strip, chrome, chrome_line, panel; };
static struct ambient g_amb;

static void ambient_default(void) {
    g_amb.centre = ugfx_rgb(52, 52, 58);
    g_amb.edge   = ugfx_rgb(22, 22, 26);
    g_amb.strip  = ugfx_rgb(38, 38, 43);
    g_amb.chrome = UTHEME_BAR_BG;
    g_amb.chrome_line = UTHEME_SEPARATOR;
    g_amb.panel  = UTHEME_PANEL_BG;
}

static int lum(uint32_t c) {
    return (int)(((c >> 16) & 0xFF) * 3 + ((c >> 8) & 0xFF) * 6 + (c & 0xFF)) / 10;
}

// The picture's average, and the average of its darkest quarter, from a
// 16x9 sample. The stage runs from a slightly darkened average at the
// centre to a deep shade of the dark quarter at the edge -- so a sunset
// sits in warm dusk and a seascape in deep teal.
static void ambient_from(const struct uimg *im) {
    struct uimg s;
    if (!im || uimg_scale(im, 16, 9, &s) != 0) { ambient_default(); return; }
    enum { N = 16 * 9 };
    uint32_t px[N];
    long r = 0, g = 0, b = 0;
    for (int i = 0; i < N; i++) {
        px[i] = s.px[i];
        r += (px[i] >> 16) & 0xFF; g += (px[i] >> 8) & 0xFF; b += px[i] & 0xFF;
    }
    uimg_free(&s);
    // The darkest quarter, by an insertion sort on luminance: 144 values.
    for (int i = 1; i < N; i++) {
        uint32_t v = px[i];
        int j = i - 1;
        while (j >= 0 && lum(px[j]) > lum(v)) { px[j + 1] = px[j]; j--; }
        px[j + 1] = v;
    }
    long dr = 0, dg = 0, db = 0;
    for (int i = 0; i < N / 4; i++) {
        dr += (px[i] >> 16) & 0xFF; dg += (px[i] >> 8) & 0xFF; db += px[i] & 0xFF;
    }
    uint32_t avg = ugfx_rgb((uint8_t)(r / N), (uint8_t)(g / N), (uint8_t)(b / N));
    uint32_t dark = ugfx_rgb((uint8_t)(dr * 4 / N), (uint8_t)(dg * 4 / N), (uint8_t)(db * 4 / N));
    uint32_t black = ugfx_rgb(0, 0, 0), white = ugfx_rgb(255, 255, 255);
    g_amb.centre = ugfx_blend(avg, black, 40);
    g_amb.edge   = ugfx_blend(dark, black, 140);
    g_amb.strip  = ugfx_blend(avg, black, 170);
    // A FAINT wash on light chrome: dark text has to keep its contrast.
    g_amb.chrome = ugfx_blend(ugfx_rgb(232, 232, 234), avg, 26);
    g_amb.chrome_line = ugfx_blend(g_amb.chrome, black, 34);
    g_amb.panel  = ugfx_blend(g_amb.chrome, white, 110);
}

static void apply_chrome(void) {
    g_menu.bar_bg = g_amb.chrome;
    g_tb.bg = g_amb.chrome;
    g_status.bg = g_amb.chrome;
    g_strip.bg = g_amb.strip;
    g_strip.empty = ugfx_blend(g_amb.strip, ugfx_rgb(255, 255, 255), 24);
}

// The stage's radial ground, CACHED: computed once per size and colour
// pair, blitted every frame after that.
static uint32_t *g_grad;
static int g_grad_w, g_grad_h;
static uint32_t g_grad_c, g_grad_e;

static void stage_paint(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!g_grad || g_grad_w != w || g_grad_h != h ||
        g_grad_c != g_amb.centre || g_grad_e != g_amb.edge) {
        free(g_grad);
        g_grad = malloc((size_t)w * (size_t)h * sizeof *g_grad);
        if (!g_grad) { ugfx_fill_rect(s, x, y, w, h, g_amb.edge); return; }
        g_grad_w = w; g_grad_h = h; g_grad_c = g_amb.centre; g_grad_e = g_amb.edge;
        // An ellipse a little wider than the stage, centred just above
        // the middle; t squared needs no square root and falls off as a
        // lit-from-behind ground should.
        int cx = w / 2, cy = h * 45 / 100;
        long rx = (long)w * 65 / 100 + 1, ry = (long)h * 75 / 100 + 1;
        for (int yy = 0; yy < h; yy++) {
            long dy = (long)(yy - cy) * 256 / ry;
            for (int xx = 0; xx < w; xx++) {
                long dx = (long)(xx - cx) * 256 / rx;
                long t = (dx * dx + dy * dy) >> 8;
                if (t > 255) t = 255;
                g_grad[(size_t)yy * w + xx] = ugfx_blend(g_grad_c, g_grad_e, (uint8_t)t);
            }
        }
    }
    ugfx_blit(s, x, y, w, h, g_grad, w);
}

// --- zoom ---------------------------------------------------------------

static const int k_zoom[] = { 10, 25, 33, 50, 67, 100, 150, 200, 300, 400, 600, 800, 1200, 1600 };
#define ZOOM_STEPS ((int)(sizeof k_zoom / sizeof k_zoom[0]))

// The status bar's zoom readout: the mode as well as the number, since
// "Fit" says why it is that number.
static void zoom_readout(void) {
    int z = uui_image_zoom_pct(&g_view);
    if (g_view.zoom) snprintf(g_stat_zoom, sizeof g_stat_zoom, "%d%%", z);
    else snprintf(g_stat_zoom, sizeof g_stat_zoom, "Fit  %d%%", z);
}

static void zoom_to(int pct, int ax, int ay) {
    uui_image_set_zoom(&g_view, pct, ax, ay);
    zoom_readout();
    ulogf("imgview: zoom %d%%%s\n", uui_image_zoom_pct(&g_view), g_view.zoom ? "" : " (fit)");
}

// One step along the ladder. Zooming OUT past the fit size lands on fit,
// which is where Windows Photos and Gwenview both stop.
static void zoom_step(int dir, int ax, int ay) {
    int cur = uui_image_zoom_pct(&g_view);
    if (dir > 0) {
        for (int i = 0; i < ZOOM_STEPS; i++)
            if (k_zoom[i] > cur) { zoom_to(k_zoom[i], ax, ay); return; }
        return;
    }
    int fit;
    {   // the fit size, whatever mode is current
        int z = g_view.zoom;
        g_view.zoom = 0;
        fit = uui_image_zoom_pct(&g_view);
        g_view.zoom = z;
    }
    for (int i = ZOOM_STEPS - 1; i >= 0; i--)
        if (k_zoom[i] < cur) {
            if (k_zoom[i] <= fit) zoom_to(0, -1, -1);
            else zoom_to(k_zoom[i], ax, ay);
            return;
        }
    zoom_to(0, -1, -1);
}

// --- the folder ---------------------------------------------------------

// THE PROBE DECIDES, not the extension: uimg_probe() reads the magic
// bytes, so a JPEG saved as .dat is listed and a text file called
// photo.jpg is not.
static int keep_images(void *ctx, const char *dir, const struct sys_dirent *e) {
    (void)ctx;
    if (e->is_dir) return 0;
    char path[PATH_MAX_LEN];
    if (!k_path_join(dir, e->name, path, sizeof path)) return 0;
    uint8_t head[16];
    size_t got = ufile_read_head(path, head, sizeof head);
    return got >= 4 && uimg_probe(head, got);
}

static const struct uimg *strip_thumb(void *ctx, int i, int px) {
    (void)ctx;
    if (i < 0 || i >= g_count) return 0;
    char path[PATH_MAX_LEN];
    if (!k_path_join(g_dir, g_entries[i].name, path, sizeof path)) return 0;
    return uthumb_get(path, &g_entries[i].modified, g_entries[i].size, px);
}

static void load_dir(void) {
    static struct sys_dirent all[SYS_LISTDIR_MAX];   // static: the frame budget is 2 KiB
    int n = sys_listdir(g_dir, all, SYS_LISTDIR_MAX);
    g_count = 0;
    for (int i = 0; i < n && g_count < MAX_FILES; i++)
        if (keep_images(0, g_dir, &all[i])) g_entries[g_count++] = all[i];
    dirsort(g_entries, g_count, DIRSORT_NAME, 0);
    uui_thumbstrip_set(&g_strip, g_count, strip_thumb, 0);
    ulogf("imgview: listing %s -- %d image(s)\n", g_dir, g_count);
}

static int index_of(const char *name) {
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_entries[i].name, name) == 0) return i;
    return -1;
}

// --- decoding, on a WORKER THREAD -----------------------------------------
//
// **A DECODE IS THE LONGEST THING THIS APP DOES AND IT MUST NOT BE ON THE
// PAINT LOOP** (280 ms for a 1280x720 JPEG, measured). ONE AT A TIME, and
// a request that arrives during one is remembered rather than started
// beside it -- the last one asked for is the one shown.
struct decode_job {
    struct uapp *app;
    char path[PATH_MAX_LEN];
    char name[PATH_MAX_LEN];
    struct uimg img;
    struct uimg_info info;
    int rc;
    unsigned long long ms;
    char err[96];   // uimg_last_error() is one string per process
};

static struct decode_job *g_active;
static char g_pending_path[PATH_MAX_LEN], g_pending_name[PATH_MAX_LEN];
static int g_pending;
// Frames painted while a decode ran: the evidence the window kept
// painting (tools/imgview_test.py reads it from the "shown" line).
static unsigned g_frames_during;

static void *decode_main(void *arg) {
    struct decode_job *j = (struct decode_job *)arg;
    unsigned long long t0 = sys_monotonic_ns();
    j->rc = uimg_load(j->path, &j->img);
    j->ms = (sys_monotonic_ns() - t0) / 1000000ull;
    if (j->rc < 0) strlcpy(j->err, uimg_last_error(), sizeof j->err);
    uapp_post(j->app, USER_DECODED, 0);
    return NULL;
}

// "JPEG, baseline, 4:2:0": the codec's name in capitals, then its detail.
static void fmt_format(char *out, int cap, const struct uimg_info *in) {
    char name[16] = "";
    int n = 0;
    for (const char *p = in->format ? in->format : ""; *p && n < (int)sizeof name - 1; p++)
        name[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    name[n] = '\0';
    snprintf(out, (size_t)cap, "%s%s%s", name, name[0] && in->detail[0] ? ", " : "", in->detail);
}

static void fmt_size(char *out, int cap, uint32_t bytes) {
    if (bytes < 1024) snprintf(out, (size_t)cap, "%u B", (unsigned)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, (size_t)cap, "%u.%u KB", (unsigned)(bytes / 1024),
                 (unsigned)(bytes % 1024 * 10 / 1024));
    else snprintf(out, (size_t)cap, "%u.%u MB", (unsigned)(bytes >> 20),
                  (unsigned)((bytes & 0xFFFFF) * 10 >> 20));
}

static void start_decode(struct uapp *a, const char *path, const char *name) {
    struct decode_job *j = calloc(1, sizeof *j);
    if (!j) { strlcpy(g_stat_note, "not enough memory to decode", sizeof g_stat_note); return; }
    j->app = a;
    strlcpy(j->path, path, sizeof j->path);
    strlcpy(j->name, name, sizeof j->name);
    // THE HEADER HERE, not on the worker: a few hundred bytes, and it is
    // what lets the status bar say what is loading instead of going blank.
    if (uimg_load_info(j->path, &j->info) == 0) {
        snprintf(g_stat_dims, sizeof g_stat_dims, "%d x %d", j->info.w, j->info.h);
        fmt_format(g_info_detail, sizeof g_info_detail, &j->info);
    } else {
        g_stat_dims[0] = g_info_detail[0] = '\0';
    }
    strlcpy(g_stat_name, j->name, sizeof g_stat_name);
    strlcpy(g_stat_note, "decoding...", sizeof g_stat_note);
    ulogf("imgview: decoding %s\n", j->name);
    g_frames_during = 0;
    // PUBLISHED BEFORE THE THREAD EXISTS: finish_decode() takes the job
    // out of g_active, and the fallback below goes through it too.
    g_active = j;
    uapp_busy_begin(a);
    uapp_redraw(a);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &at, decode_main, j) != 0) {
        ulog("imgview: no worker thread; decoding on the main loop\n");
        decode_main(j);
    }
}

static void show_index(struct uapp *a, int i) {
    if (i < 0 || i >= g_count) return;
    g_cur = i;
    uui_thumbstrip_select(&g_strip, i);
    fmt_size(g_stat_file, sizeof g_stat_file, g_entries[i].size);
    char path[PATH_MAX_LEN];
    if (!k_path_join(g_dir, g_entries[i].name, path, sizeof path)) return;
    if (g_active) {
        strlcpy(g_pending_path, path, sizeof g_pending_path);
        strlcpy(g_pending_name, g_entries[i].name, sizeof g_pending_name);
        g_pending = 1;
        strlcpy(g_stat_name, g_entries[i].name, sizeof g_stat_name);
        return;
    }
    start_decode(a, path, g_entries[i].name);
}

// What the widget shows: the picture, or its turned copy.
static void show_rotation(void) {
    uui_image_set(&g_view, NULL);
    uimg_free(&g_rimg);
    if (g_have_img && g_rot && uimg_rotate(&g_img, g_rot, &g_rimg) == 0)
        uui_image_set(&g_view, &g_rimg);
    else if (g_have_img)
        uui_image_set(&g_view, &g_img);
    zoom_to(0, -1, -1);
}

static void finish_decode(struct uapp *a) {
    struct decode_job *j = g_active;
    if (!j) return;
    g_active = NULL;
    uapp_busy_end(a);

    if (j->rc == 0) {
        // The widget is pointed away BEFORE the old pixels are freed.
        uui_image_set(&g_view, NULL);
        uimg_free(&g_rimg);
        uimg_free(&g_img);
        g_img = j->img;
        g_have_img = 1;
        g_rot = 0;
        ambient_from(&g_img);
        apply_chrome();
        show_rotation();
        g_decode_ms = j->ms;
        snprintf(g_stat_dims, sizeof g_stat_dims, "%d x %d", j->img.w, j->img.h);
        fmt_format(g_info_detail, sizeof g_info_detail, &j->info);
        snprintf(g_stat_note, sizeof g_stat_note, "decoded in %llu ms", j->ms);
        ulogf("imgview: shown %s %dx%d in %llu ms, %u frame(s) during\n",
              j->name, j->img.w, j->img.h, j->ms, g_frames_during);
    } else {
        // A REFUSAL IS NOT A CORRUPTION, and the status bar says which:
        // the decoder's own sentence, copied on the worker.
        uui_image_set(&g_view, NULL);
        uimg_free(&g_rimg);
        if (g_have_img) { uimg_free(&g_img); g_have_img = 0; }
        ambient_default();
        apply_chrome();
        strlcpy(g_stat_dims, "not shown", sizeof g_stat_dims);
        strlcpy(g_stat_note, j->err, sizeof g_stat_note);
        ulogf("imgview: refused %s -- %s\n", j->name, j->err);
    }
    strlcpy(g_stat_name, j->name, sizeof g_stat_name);
    free(j);

    if (g_pending) {
        g_pending = 0;
        start_decode(a, g_pending_path, g_pending_name);
    }
    uapp_redraw(a);
}

// --- opening a picture from anywhere ---------------------------------

static struct uui_filedialog g_fd;

static void open_chosen(void *ctx, const char *path) {
    struct uapp *a = (struct uapp *)ctx;
    if (!path) { uapp_redraw(a); return; }
    // THE FILE'S DIRECTORY BECOMES THE FILMSTRIP, that file selected --
    // the same state a path on the command line produces.
    char dir[PATH_MAX_LEN];
    if (!k_path_dirname(path, dir, sizeof dir)) strlcpy(dir, "/", sizeof dir);
    strlcpy(g_dir, dir, sizeof g_dir);
    load_dir();
    int i = index_of(k_path_basename(path));
    show_index(a, i >= 0 ? i : 0);
    uapp_redraw(a);
}

static void open_dialog(struct uapp *a) {
    if (uui_filedialog_is_open(&g_fd)) return;
    uui_menubar_close(&g_menu);
    static const struct uui_filedialog_filter types[] = {
        { "Image files", keep_images, 0 },
        UUI_FILEDIALOG_ALL_FILES,
    };
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_OPEN,
        .title = "Open Image",
        .start_dir = g_dir,
        .filters = types,
        .filter_count = (int)(sizeof types / sizeof types[0]),
    };
    uui_filedialog_open(a, &g_fd, &o, open_chosen, a);
}

// --- the wallpaper ------------------------------------------------------
//
// THROUGH THE SETTINGS REGISTRY, the path `config set` and System
// Settings take, so the desktop -- another process -- sees the
// generation move. THE VALUE IS A NAME, so the picture has to live in
// /usr/share/wallpapers; anything else is refused with a sentence.
// SETTING_UNSAVED is live but not persisted, which is not success.
static int set_setting(const char *name, const char *value) {
    return usetting_set(name, value) == SETTING_SAVED;
}

static void set_wallpaper(struct uapp *a, const char *mode) {
    if (!mode) {
        int ok = set_setting("desktop.wallpaper", "none");
        strlcpy(g_stat_note, ok ? "wallpaper cleared" : "could not clear wallpaper",
                sizeof g_stat_note);
        ulogf("imgview: wallpaper none -- %s\n", ok ? "ok" : "FAILED");
        uapp_redraw(a);
        return;
    }
    if (g_cur < 0) return;
    if (strcmp(g_dir, WALLPAPER_DIR) != 0) {
        strlcpy(g_stat_note, "only files in /usr/share/wallpapers", sizeof g_stat_note);
        ulogf("imgview: wallpaper refused -- %s is not %s\n", g_dir, WALLPAPER_DIR);
        uapp_redraw(a);
        return;
    }
    char stem[PATH_MAX_LEN];
    strlcpy(stem, g_entries[g_cur].name, sizeof stem);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    int ok = set_setting("desktop.wallpaper", stem);
    ok = set_setting("desktop.wallpaper_mode", mode) && ok;
    snprintf(g_stat_note, sizeof g_stat_note, "%s wallpaper: %s, %s",
             ok ? "set" : "could not set", stem, mode);
    ulogf("imgview: wallpaper %s mode %s -- %s\n", stem, mode, ok ? "ok" : "FAILED");
    uapp_redraw(a);
}

// --- menus and the command bar ------------------------------------------

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Open...", CMD_OPEN,   "Ctrl+O"),
    UUI_MENU_SEP,
    UUI_MENU("Reload", CMD_RELOAD, "F5"),
    UUI_MENU_SEP,
    UUI_MENU("Exit",   CMD_EXIT,   "Alt+F4"),
};

static const struct uui_menu_item view_items[] = {
    UUI_MENU("Previous",      CMD_PREV,     "Left"),
    UUI_MENU("Next",          CMD_NEXT,     "Right"),
    UUI_MENU_SEP,
    UUI_MENU("Zoom in",       CMD_ZOOM_IN,  "+"),
    UUI_MENU("Zoom out",      CMD_ZOOM_OUT, "-"),
    UUI_MENU("Fit to window", CMD_FIT,      "0"),
    UUI_MENU("Actual size",   CMD_ACTUAL,   "1"),
    UUI_MENU_SEP,
    UUI_MENU("Rotate left",   CMD_ROT_L,    "L"),
    UUI_MENU("Rotate right",  CMD_ROT_R,    "R"),
    UUI_MENU_SEP,
    UUI_MENU("Properties",    CMD_PROPS,    "I"),
    UUI_MENU("Slideshow",     CMD_SLIDESHOW, "F11"),
};

static const struct uui_menu_item desktop_items[] = {
    UUI_MENU("Set as wallpaper (fill)", CMD_WALLPAPER_FILL, 0),
    UUI_MENU("Set as wallpaper (fit)",  CMD_WALLPAPER_FIT,  0),
    UUI_MENU_SEP,
    UUI_MENU("No wallpaper",            CMD_WALLPAPER_NONE, 0),
};

static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File",    file_items),
    UUI_SUBMENU("View",    view_items),
    UUI_SUBMENU("Desktop", desktop_items),
};

// COLOUR-CODED BY WHAT EACH COMMAND DOES, through the theme's action
// roles: moving about, zoom, rotate, the wallpaper, the slideshow.
#define TINT_NAV   UTHEME_ACT_NAV
#define TINT_ZOOM  UTHEME_ACT_VIEW
#define TINT_ROT   UTHEME_ACT_EDIT
#define TINT_WALL  UTHEME_ACT_MEDIA
#define TINT_SHOW  UTHEME_ACT_CREATE

static const struct uui_toolbar_item tb_items[] = {
    { "tb-back",         "Previous (Left)",      CMD_PREV,      0, 0, 0, TINT_NAV },
    { "tb-forward",      "Next (Right)",         CMD_NEXT,      0, 0, 0, TINT_NAV },
    UUI_TOOLBAR_SEP,
    { "tb-zoom-out",     "Zoom out (-)",         CMD_ZOOM_OUT,  0, 0, 0, TINT_ZOOM },
    { "tb-zoom-in",      "Zoom in (+)",          CMD_ZOOM_IN,   0, 0, 0, TINT_ZOOM },
    { "tb-fit",          "Fit to window (0)",    CMD_FIT,       0, 0, 0, TINT_ZOOM },
    { "tb-actual",       "Actual size (1)",      CMD_ACTUAL,    0, 0, 0, TINT_ZOOM },
    UUI_TOOLBAR_SEP,
    { "tb-rotate-left",  "Rotate left (L)",      CMD_ROT_L,     0, 0, 0, TINT_ROT },
    { "tb-rotate-right", "Rotate right (R)",     CMD_ROT_R,     0, 0, 0, TINT_ROT },
    UUI_TOOLBAR_SEP,
    { "tb-wallpaper",    "Set as the desktop wallpaper", CMD_WALLPAPER, "Set as wallpaper", 0, 0, TINT_WALL },
    { "tb-slideshow",    "Slideshow (F11)",      CMD_SLIDESHOW, "Slideshow", 0, 0, TINT_SHOW },
    { "tb-info",         "Properties (I)",       CMD_PROPS,     0, UUI_TB_END, 0, TINT_NAV },
};

// ONE STATE SOURCE for the menu's ticks and the bar's latches.
static unsigned item_flags(int code) {
    switch (code) {
    case CMD_FIT:   return g_view.zoom == 0 ? UUI_MI_CHECKED : 0;
    case CMD_PROPS: return g_props ? UUI_MI_CHECKED : 0;
    case CMD_PREV:  return g_cur > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_NEXT:  return g_cur >= 0 && g_cur < g_count - 1 ? 0 : UUI_MI_DISABLED;
    case CMD_WALLPAPER: case CMD_WALLPAPER_FIT: case CMD_WALLPAPER_FILL:
        return g_cur >= 0 && strcmp(g_dir, WALLPAPER_DIR) == 0 ? 0 : UUI_MI_DISABLED;
    default:        return 0;
    }
}

// The routed widgets. `.name` is what the layout log reports each as.
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,    .widget = &g_menu,    .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_toolbar_ops,    .widget = &g_tb,      .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_image_ops,      .widget = &g_view,    .id = ID_IMAGE,   .name = "image" },
    { .ops = &uui_thumbstrip_ops, .widget = &g_strip,   .id = ID_STRIP,   .name = "strip" },
    { .ops = &uui_button_ops,     .widget = &g_wp_fill, .id = ID_WP_FILL, .name = "wpfill" },
    { .ops = &uui_button_ops,     .widget = &g_wp_fit,  .id = ID_WP_FIT,  .name = "wpfit" },
};
#define W_MENU 0
#define W_TB 1
#define W_STRIP 3
#define W_FILL 4
#define W_FIT 5

// --- the slideshow ------------------------------------------------------

static void slideshow(struct uapp *a, int on) {
    g_slide = on;
    g_paused = 0;
    g_slide_t0 = sys_monotonic_ns() / 1000000ull;
    uui_menubar_close(&g_menu);
    int hide = on ? 1 : 0;
    g_widgets[W_MENU].hidden = g_widgets[W_TB].hidden = hide;
    g_widgets[W_STRIP].hidden = hide;
    g_widgets[W_FILL].hidden = g_widgets[W_FIT].hidden = hide || !g_props;
    zoom_to(0, -1, -1);
    uapp_set_fullscreen(a, on);
    ulogf("imgview: slideshow %s\n", on ? "on" : "off");
}

static void step(struct uapp *a, int dir) {
    if (!g_count) return;
    int i = g_cur + dir;
    if (g_slide) i = (i + g_count) % g_count;     // a slideshow loops
    if (i < 0 || i >= g_count) return;
    show_index(a, i);
}

// --- layout and drawing ------------------------------------------------

static void layout_all(int cw, int ch) {
    if (g_slide) {
        g_sx = 0; g_sy = 0; g_sw = cw; g_sh = ch;
        g_view.x = 0; g_view.y = 0; g_view.w = cw; g_view.h = ch;
        return;
    }
    int mb = uui_menubar_height(&g_menu);
    int tb = uui_toolbar_height(&g_tb);
    int sb = uui_statusbar_height(&g_status);
    int sw, sh;
    uui_thumbstrip_ops.natural_size(&g_strip, &sw, &sh);
    int pw = g_props ? ugfx_char_h() * 19 : 0;
    if (pw > cw / 2) pw = cw / 2;

    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_toolbar_ops.set_geometry(&g_tb, 0, mb, cw, tb);

    uui_statusbar_set_geometry(&g_status, 0, ch - sb, cw, sb);

    int top = mb + tb, bottom = ch - sb - sh;
    uui_thumbstrip_ops.set_geometry(&g_strip, 0, bottom, cw - pw, sh);
    g_sx = 0; g_sy = top; g_sw = cw - pw; g_sh = bottom - top;
    g_view.x = g_sx; g_view.y = g_sy; g_view.w = g_sw; g_view.h = g_sh;

    g_px0 = cw - pw; g_py0 = top; g_pw = pw; g_ph = ch - sb - top;
    int bh = utheme_control_h() + 2, pad = utheme_pad() * 2;
    int bw = (pw - 3 * pad) / 2;
    int by = g_py0 + g_ph - bh - pad;
    uui_button_set_geometry(&g_wp_fill, g_px0 + pad, by, bw, bh);
    uui_button_set_geometry(&g_wp_fit, g_px0 + 2 * pad + bw, by, bw, bh);
}

static void text_at(struct ugfx_surface *s, int x, int y, int w, const char *t,
                    uint32_t fg, uint32_t bg) {
    ugfx_draw_string_clipped(s, x, y, w, t, fg, bg);
}

static void draw_props(struct ugfx_surface *s) {
    if (!g_props || g_slide || g_pw <= 0) return;
    uint32_t bg = g_amb.panel, fg = UTHEME_TEXT;
    uint32_t dim = ugfx_blend(fg, bg, 110);
    ugfx_fill_rect(s, g_px0, g_py0, g_pw, g_ph, bg);
    ugfx_fill_rect(s, g_px0, g_py0, 1, g_ph, g_amb.chrome_line);
    int pad = utheme_pad() * 2, lh = ugfx_char_h() + utheme_pad();
    int x = g_px0 + pad, y = g_py0 + pad, w = g_pw - 2 * pad, kw = ugfx_char_h() * 6;

    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    text_at(s, x, y, w, "Properties", fg, bg);
    ugfx_set_font(was);
    y += lh + pad;

    char dims[32] = "", size[24] = "", mod[32] = "", dec[24] = "";
    if (g_have_img) snprintf(dims, sizeof dims, "%d x %d", g_view.img ? g_view.img->w : 0,
                             g_view.img ? g_view.img->h : 0);
    if (g_cur >= 0) {
        fmt_size(size, sizeof size, g_entries[g_cur].size);
        const struct rtc_time *t = &g_entries[g_cur].modified;
        snprintf(mod, sizeof mod, "%04u-%02u-%02u %02u:%02u", (unsigned)t->year,
                 (unsigned)t->month, (unsigned)t->day, (unsigned)t->hour, (unsigned)t->minute);
    }
    if (g_have_img) snprintf(dec, sizeof dec, "%llu ms", g_decode_ms);
    const struct { const char *head; uint32_t col; const char *k[4]; const char *v[4]; } sec[] = {
        { "IMAGE", utheme_action(TINT_ZOOM), { "Dimensions", "Format", "Decoded in", 0 },
          { dims, g_info_detail, dec, 0 } },
        { "FILE", utheme_action(TINT_ROT), { "Name", "Folder", "Size", "Modified" },
          { g_cur >= 0 ? g_entries[g_cur].name : "", g_dir, size, mod } },
    };
    for (int i = 0; i < 2; i++) {
        text_at(s, x, y, w, sec[i].head, sec[i].col, bg);
        y += lh;
        for (int k = 0; k < 4 && sec[i].k[k]; k++) {
            text_at(s, x, y, kw, sec[i].k[k], dim, bg);
            text_at(s, x + kw, y, w - kw, sec[i].v[k], fg, bg);
            y += lh;
        }
        y += pad;
    }
    text_at(s, x, y, w, "WALLPAPER", utheme_action(TINT_WALL), bg);
    y += lh;
    char cur[64] = "", wp[32] = "", mode[16] = "";
    if (usetting_get("desktop.wallpaper", wp, sizeof wp) > 0) {
        usetting_get("desktop.wallpaper_mode", mode, sizeof mode);
        snprintf(cur, sizeof cur, "Now: %s%s%s", wp, mode[0] ? ", " : "", mode);
    }
    text_at(s, x, y, w, cur, dim, bg);
}

// The navigator: the whole picture small, the part on screen outlined --
// only while the zoomed picture is bigger than the stage.
static void draw_navigator(struct ugfx_surface *s) {
    const struct uimg *im = g_view.img;
    if (!im || !uui_image_can_pan(&g_view)) return;
    int nw = ugfx_char_h() * 11, nh = nw * im->h / (im->w ? im->w : 1);
    if (nh > nw) { nh = nw; nw = nh * im->w / (im->h ? im->h : 1); }
    int nx = g_sx + g_sw - nw - 14, ny = g_sy + g_sh - nh - 12;
    uint32_t ring = ugfx_rgb(242, 232, 234), mark = ugfx_rgb(240, 182, 94);
    ugfx_fill_rect(s, nx - 2, ny - 2, nw + 4, nh + 4, ring);
    ugfx_blit_scaled_alpha(s, nx, ny, nw, nh, im->px, im->w, im->h, im->w, 255);
    long dw = (long)im->w * g_view.zoom / 100, dh = (long)im->h * g_view.zoom / 100;
    int vx = (int)(nx + (long)g_view.pan_x * nw / dw), vy = (int)(ny + (long)g_view.pan_y * nh / dh);
    int vw = (int)((long)(g_sw < dw ? g_sw : dw) * nw / dw);
    int vh = (int)((long)(g_sh < dh ? g_sh : dh) * nh / dh);
    ugfx_draw_rect(s, vx, vy, vw, vh, mark);
    ugfx_draw_rect(s, vx + 1, vy + 1, vw - 2, vh - 2, mark);
}

static void fill_round(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c) {
    int r = h / 2;
    ugfx_fill_rect(s, x + r, y, w - 2 * r, h, c);
    ugfx_fill_circle(s, x + r, y + r, r, c);
    ugfx_fill_circle(s, x + w - r - 1, y + r, r, c);
}

// The slideshow's floating controls, Esc hint and progress line.
static void draw_slideshow(struct ugfx_surface *s, int cw, int ch) {
    if (!g_slide) return;
    uint32_t pill = ugfx_rgb(18, 20, 28), fg = ugfx_rgb(232, 238, 248);
    uint32_t accent = ugfx_rgb(95, 143, 208), amber = ugfx_rgb(240, 182, 94);
    int lh = ugfx_char_h();
    const char *hint = "Esc to leave the slideshow";
    int hw = ugfx_text_width(hint) + lh * 2;
    fill_round(s, cw - hw - 16, 14, hw, lh + 10, pill);
    text_at(s, cw - hw - 16 + lh, 19, hw, hint, fg, pill);

    char label[96], every[16];
    snprintf(label, sizeof label, "%s  -  %d of %d", g_cur >= 0 ? g_entries[g_cur].name : "",
             g_cur + 1, g_count);
    snprintf(every, sizeof every, "Every %d s", g_slide_ms / 1000);
    int bh = lh * 3, bw = bh;
    int lw = ugfx_text_width(label), ew = ugfx_text_width(every) + lh;
    int pw = lw + 3 * bw + ew + lh * 5, ph = bh + 10;
    int px = (cw - pw) / 2, py = ch - ph - 28;
    fill_round(s, px, py, pw, ph, pill);
    int x = px + lh * 2;
    text_at(s, x, py + (ph - lh) / 2, lw, label, fg, pill);
    x += lw + lh;
    g_pill_prev = (struct hit){ x, py + 5, bw, bh };
    g_pill_play = (struct hit){ x + bw, py + 5, bw, bh };
    g_pill_next = (struct hit){ x + 2 * bw, py + 5, bw, bh };
    g_pill_every = (struct hit){ x + 3 * bw + lh / 2, py + 5, ew, bh };
    int cy = py + ph / 2, r = bh / 2 - 2;
    ugfx_fill_circle(s, g_pill_play.x + bw / 2, cy, r, accent);
    // Previous / next chevrons, pause bars or a play triangle.
    for (int k = 0; k < r / 2; k++) {
        ugfx_fill_rect(s, g_pill_prev.x + bw / 2 - r / 4 + k, cy - k, 2, 1, fg);
        ugfx_fill_rect(s, g_pill_prev.x + bw / 2 - r / 4 + k, cy + k, 2, 1, fg);
        ugfx_fill_rect(s, g_pill_next.x + bw / 2 + r / 4 - k, cy - k, 2, 1, fg);
        ugfx_fill_rect(s, g_pill_next.x + bw / 2 + r / 4 - k, cy + k, 2, 1, fg);
    }
    int pcx = g_pill_play.x + bw / 2;
    if (g_paused) {
        int xs[3] = { pcx - r / 3, pcx + r / 2, pcx - r / 3 }, ys[3] = { cy - r / 2, cy, cy + r / 2 };
        ugfx_fill_polygon(s, xs, ys, 3, pill);
    } else {
        ugfx_fill_rect(s, pcx - r / 3, cy - r / 2, r / 4 + 1, r, pill);
        ugfx_fill_rect(s, pcx + r / 6, cy - r / 2, r / 4 + 1, r, pill);
    }
    fill_round(s, g_pill_every.x, cy - lh / 2 - 3, ew, lh + 6, ugfx_rgb(42, 58, 94));
    text_at(s, g_pill_every.x + lh / 2, cy - lh / 2, ew, every, fg, ugfx_rgb(42, 58, 94));

    unsigned long long now = sys_monotonic_ns() / 1000000ull;
    long frac = g_paused ? 0 : (long)((now - g_slide_t0) * (unsigned long long)cw / (unsigned)g_slide_ms);
    if (frac > cw) frac = cw;
    ugfx_fill_rect(s, 0, ch - 4, cw, 4, ugfx_blend(ugfx_rgb(0, 0, 0), ugfx_rgb(255, 255, 255), 30));
    ugfx_fill_rect(s, 0, ch - 4, (int)frac, 4, amber);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    if (g_active) g_frames_during++;
    int cw = d->surface->w, ch = d->surface->h;
    layout_all(cw, ch);
    stage_paint(d->surface, g_sx, g_sy, g_sw, g_sh);
    if (!g_slide) {
        draw_props(d->surface);
        snprintf(g_stat_size, sizeof g_stat_size, "%s%s%s", g_stat_dims,
                 g_stat_dims[0] && g_stat_file[0] ? "  " : "", g_stat_file);
        uui_statusbar_draw(d->surface, &g_status);
        // The line between the toolbar and the stage.
        ugfx_fill_rect(d->surface, 0, g_sy - 1, cw, 1, g_amb.chrome_line);
    }
    uapp_log_layout(a, "imgview");   // every widget, by name (ui/uui_describe.h)
}

static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    draw_navigator(d->surface);
    draw_slideshow(d->surface, d->surface->w, d->surface->h);
}

// --- input ----------------------------------------------------------------

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_OPEN:     open_dialog(a); return;
    case CMD_RELOAD: {
        char keep[PATH_MAX_LEN] = "";
        if (g_cur >= 0) strlcpy(keep, g_entries[g_cur].name, sizeof keep);
        load_dir();
        int i = keep[0] ? index_of(keep) : -1;
        show_index(a, i >= 0 ? i : 0);
        break;
    }
    case CMD_EXIT:     uapp_quit(a, 0); return;
    case CMD_PREV:     step(a, -1); break;
    case CMD_NEXT:     step(a, +1); break;
    case CMD_ZOOM_IN:  zoom_step(+1, -1, -1); break;
    case CMD_ZOOM_OUT: zoom_step(-1, -1, -1); break;
    case CMD_FIT:      zoom_to(0, -1, -1); break;
    case CMD_ACTUAL:   zoom_to(100, -1, -1); break;
    case CMD_ROT_L:
    case CMD_ROT_R:
        g_rot = (g_rot + (code == CMD_ROT_R ? 1 : 3)) % 4;
        show_rotation();
        ulogf("imgview: rotate %d\n", g_rot * 90);
        break;
    case CMD_WALLPAPER:      set_wallpaper(a, "fill"); return;
    case CMD_WALLPAPER_FILL: set_wallpaper(a, "fill"); return;
    case CMD_WALLPAPER_FIT:  set_wallpaper(a, "fit");  return;
    case CMD_WALLPAPER_NONE: set_wallpaper(a, NULL);   return;
    case CMD_SLIDESHOW: slideshow(a, !g_slide); break;
    case CMD_PROPS:
        g_props = !g_props;
        g_widgets[W_FILL].hidden = g_widgets[W_FIT].hidden = !g_props || g_slide;
        ulogf("imgview: properties %s\n", g_props ? "on" : "off");
        break;
    default: return;
    }
    uapp_redraw(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    switch (id) {
    case ID_MENU: {
        int code = uui_menubar_take_code(&g_menu);   // PARKED in the widget
        if (code > 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    case ID_TOOLBAR: {
        int code = uui_toolbar_take_code(&g_tb);
        if (code > 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    case ID_STRIP: {
        int i = uui_thumbstrip_take(&g_strip);
        if (i >= 0 && i != g_cur) show_index(a, i);
        uapp_redraw(a);
        return;
    }
    case ID_WP_FILL: if (reason == UUI_REASON_RELEASE) set_wallpaper(a, "fill"); return;
    case ID_WP_FIT:  if (reason == UUI_REASON_RELEASE) set_wallpaper(a, "fit");  return;
    default: return;
    }
}

static int in_stage(int x, int y) {
    return x >= g_sx && y >= g_sy && x < g_sx + g_sw && y < g_sy + g_sh;
}

static int in_hit(const struct hit *h, int x, int y) {
    return x >= h->x && y >= h->y && x < h->x + h->w && y < h->y + h->h;
}

static int on_pill(int x, int y) {
    return g_slide && (in_hit(&g_pill_prev, x, y) || in_hit(&g_pill_play, x, y) ||
                       in_hit(&g_pill_next, x, y) || in_hit(&g_pill_every, x, y));
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)a;
    // A DRAG PANS a picture bigger than the stage, full screen included;
    // the picture follows the pointer, as every viewer's hand tool does.
    g_dragged = 0;
    if ((buttons & 1) && in_stage(x, y) && !on_pill(x, y) && uui_image_can_pan(&g_view)) {
        g_drag = 1;
        g_drag_x = x;
        g_drag_y = y;
    }
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    g_mx = x;
    g_my = y;
    if (!g_drag) return;
    if (!(buttons & 1)) { g_drag = 0; return; }
    if (uui_image_pan(&g_view, x - g_drag_x, y - g_drag_y)) uapp_redraw(a);
    if (x != g_drag_x || y != g_drag_y) g_dragged = 1;
    g_drag_x = x;
    g_drag_y = y;
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    g_drag = 0;
    // A pan that ENDS over a pill is not a click on it.
    if (!g_slide || g_dragged) return;
    if (in_hit(&g_pill_prev, x, y))      step(a, -1);
    else if (in_hit(&g_pill_next, x, y)) step(a, +1);
    else if (in_hit(&g_pill_play, x, y)) {
        g_paused = !g_paused;
        g_slide_t0 = sys_monotonic_ns() / 1000000ull;
    } else if (in_hit(&g_pill_every, x, y)) {
        g_slide_ms = g_slide_ms == 3000 ? 5000 : g_slide_ms == 5000 ? 10000 : 3000;
        g_slide_t0 = sys_monotonic_ns() / 1000000ull;
    } else return;
    uapp_redraw(a);
}

// THE WHEEL ZOOMS, about the pointer -- Windows Photos' default; in a
// slideshow it steps, as nothing there can be zoomed.
static void on_wheel(struct uapp *a, int notches) {
    if (g_slide) { step(a, notches > 0 ? -1 : +1); uapp_redraw(a); return; }
    if (!g_have_img || !in_stage(g_mx, g_my)) return;
    zoom_step(notches > 0 ? +1 : -1, g_mx, g_my);
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    int code;
    if (!g_slide && uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    if (key == 0x0F) { do_command(a, CMD_OPEN); return; }   // Ctrl-O, a control code
    if (uui_key_is_shortcut(key, mods)) return;
    // THE KEYS ARE A TABLE: every one is "this key runs that command",
    // the same commands the menu and the bar run.
    static const struct { int key, cmd; } keys[] = {
        { KEY_ARROW_LEFT, CMD_PREV },  { KEY_ARROW_UP, CMD_PREV },
        { KEY_ARROW_RIGHT, CMD_NEXT }, { KEY_ARROW_DOWN, CMD_NEXT },
        { KEY_F5, CMD_RELOAD },        { KEY_F11, CMD_SLIDESHOW },
        { '+', CMD_ZOOM_IN }, { '=', CMD_ZOOM_IN }, { '-', CMD_ZOOM_OUT },
        { '0', CMD_FIT },     { '1', CMD_ACTUAL },
        { 'r', CMD_ROT_R },   { 'R', CMD_ROT_R },  { 'l', CMD_ROT_L }, { 'L', CMD_ROT_L },
        { 'i', CMD_PROPS },   { 'I', CMD_PROPS },
    };
    for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++)
        if (keys[k].key == key) { do_command(a, keys[k].cmd); return; }
    if ((key == KEY_HOME || key == KEY_END) && g_count) {
        show_index(a, key == KEY_HOME ? 0 : g_count - 1);
        uapp_redraw(a);
    } else if (key == 27 && g_slide) {           // Esc leaves the slideshow
        do_command(a, CMD_SLIDESHOW);
    } else if (key == ' ' && g_slide) {          // Space pauses it
        g_paused = !g_paused;
        g_slide_t0 = sys_monotonic_ns() / 1000000ull;
        uapp_redraw(a);
    }
}

static int on_tick(struct uapp *a) {
    uthumb_tick();
    int redraw = uui_toolbar_tick(&g_tb);
    if (g_slide && !g_paused) {
        unsigned long long now = sys_monotonic_ns() / 1000000ull;
        // A ZOOMED picture holds the slideshow -- it is being looked at --
        // and its interval restarts when it zooms back out.
        if (g_view.zoom) g_slide_t0 = now;
        if (now - g_slide_t0 >= (unsigned long long)g_slide_ms) {
            g_slide_t0 = now;
            step(a, +1);
        }
        redraw = 1;   // the progress line
    }
    return redraw;
}

static int on_user(struct uapp *a, int a0, int a1) {
    (void)a1;
    if (a0 == USER_DECODED) { finish_decode(a); return 1; }
    if (a0 == USER_THUMB) return uthumb_posted();
    return 0;
}

static int wake_thumbs(void *ctx) {
    (void)ctx;
    return uapp_post(g_app, USER_THUMB, 0);
}

static void on_open(struct uapp *a) {
    g_app = a;
    static const struct uthumb_config cfg = { .wake = wake_thumbs, .log_prefix = "imgview" };
    uthumb_init(&cfg);
    layout_all(uapp_width(a), uapp_height(a));
    load_dir();
    int i = g_want[0] ? index_of(g_want) : -1;
    show_index(a, i >= 0 ? i : 0);
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    layout_all(w, h);
}

// A WINDOW SIZED BY THE FONT, as docs/gui-guidelines.md asks: room for
// the stage, the strip and the bars, with desktop still showing around
// it on a 1280x720 screen at the default font.
static void on_size(int *w, int *h) {
    int c = ugfx_char_h();
    *w = c * 54;
    *h = c * 35;
}

int main(int argc, char **argv) {
    // An argument may be a directory to browse or a file to show.
    if (argc > 1 && argv[1][0]) {
        struct sys_stat st;
        if (sys_stat(argv[1], &st) == 0 && !st.is_dir) {
            if (!k_path_dirname(argv[1], g_dir, sizeof g_dir))
                strlcpy(g_dir, DEFAULT_DIR, sizeof g_dir);
            strlcpy(g_want, k_path_basename(argv[1]), sizeof g_want);
        } else {
            strlcpy(g_dir, argv[1], sizeof g_dir);
        }
    }

    uui_menubar_init(&g_menu, menu_items, (int)(sizeof menu_items / sizeof menu_items[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    g_tb.accent_latch = 1;   // a latched toggle is filled in the accent
    g_tb.overflow = 1;
    uui_thumbstrip_init(&g_strip);
    uui_button_init(&g_wp_fill, 0, 0, 1, 1, "Fill", UTHEME_BUTTON_BG, UTHEME_TEXT, CMD_WALLPAPER_FILL);
    uui_button_init(&g_wp_fit,  0, 0, 1, 1, "Fit",  UTHEME_BUTTON_BG, UTHEME_TEXT, CMD_WALLPAPER_FIT);
    g_wp_fill.outlined = g_wp_fit.outlined = 1;
    g_widgets[W_FILL].hidden = g_widgets[W_FIT].hidden = 1;

    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_stat_name;
    g_status.panes[0].chars = 18;
    g_status.panes[1].text = g_stat_size;
    g_status.panes[1].chars = 20;
    g_status.panes[2].text = g_stat_note;
    g_status.panes[2].chars = 0;
    g_status.panes[3].text = g_stat_zoom;
    g_status.panes[3].chars = 9;
    g_status.count = 4;

    uui_image_init(&g_view, NULL, UIMG_FIT_CONTAIN);
    g_view.transparent = 1;   // the ambient stage is painted underneath
    g_view.max_w = 1280;
    g_view.max_h = 720;
    ambient_default();
    apply_chrome();
    zoom_readout();

    strlcpy(g_stat_name, "no image", sizeof g_stat_name);

    struct uapp_desc desc = {
        .title        = "Image Viewer",
        .app_id       = "imgview",
        .flags        = UAPP_RESIZABLE,
        .min_w        = 420,
        .min_h        = 300,
        .tick_ms      = 500,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .on_size      = on_size,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_draw_over = on_draw_over,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_press     = on_press,
        .on_motion    = on_motion,
        .on_release   = on_release,
        .on_wheel     = on_wheel,
        .on_resize    = on_resize,
        .on_tick      = on_tick,
        .on_user      = on_user,
    };
    int rc = uapp_run(&desc);
    uui_image_release(&g_view);
    uimg_free(&g_rimg);
    if (g_have_img) uimg_free(&g_img);
    free(g_grad);
    return rc;
}
