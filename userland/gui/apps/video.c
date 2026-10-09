// Video Player -- a ring-3 client that plays video files.
//
// THE SHAPE IS THE DESIGN LANGUAGE'S (docs/gui-guidelines.md), chosen
// from mockups on 2026-10-09 (V3): menu bar, a command bar coloured by
// role, the picture as the HERO on a stage tinted by the video, the
// seek bar and transport under it, the folder's other videos as a
// panel on the RIGHT that the bar toggles, a status bar of readouts --
// and full screen that drops all of it for a floating bar that hides
// itself while the pointer is still.
//
// THE PARTS: lib/uvid_play.h plays (a decoding thread, the sound through
// usnd as the clock); ui/uui_video.h draws the frame it says is due;
// ui/uui_medialist.h is the playlist, its pictures from lib/uthumb.h.
// This file is the window around them and the seek-bar preview, which
// decodes stills on a thread of its own.
//
// It is handed a file or a directory on the command line -- `Handles=`
// in its .desktop entry points .mpg, .mpeg and .avi here -- and defaults
// to /usr/share/videos.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "lib/uvid.h"
#include "lib/uvid_play.h"
#include "lib/usnd.h"
#include "lib/uconf.h"
#include "lib/ufile.h"
#include "lib/uthumb.h"
#include "lib/uduration.h"
#include "lib/uimg.h"
#include "kpath.h"
#include "rt/sys.h"
#include "ui/uui.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_medialist.h"
#include "ui/uui_scale.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_transport.h"
#include "ui/uui_primitives.h"
#include "ui/uui_toast.h"
#include "ui/uui_anim.h"
#include "ui/uui_video.h"
#include "ui/uambient.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "keyboard.h"

#define VIDEO_CONF  "/etc/video.conf"   // this app's own preferences
#define DEFAULT_DIR "/usr/share/videos"
#define PROPERTIES  "/bin/wm/apps/properties"
#define SAVE_DIR    "/home"
#define MAX_ITEMS   200
#define PATH_MAX_V  256
#define HIDE_MS     2500                // full screen: the bar hides after this still
#define SEEK_STEP   10000               // Left/Right, ms

enum { ID_MENU = 1, ID_TOOLBAR, ID_VIDEO, ID_LIST, ID_POS, ID_VOL, ID_TRANSPORT, ID_PILL };

enum {
    CMD_OPEN = 1, CMD_RELOAD, CMD_EXIT,
    CMD_PLAY, CMD_STOP, CMD_PREV, CMD_NEXT, CMD_BACK, CMD_FWD, CMD_LOOP,
    CMD_SLOWER, CMD_FASTER, CMD_NORMAL, CMD_SPEED,
    CMD_SAVE_FRAME, CMD_PROPS, CMD_PLAYLIST, CMD_FULL,
};

enum { POST_THUMB = 1, POST_PREVIEW };

struct item {
    char name[64];
    uint32_t ms;
    char detail[48];
    struct rtc_time mtime;
    uint32_t size;
};

static struct uapp *g_app;
static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
static struct uui_video g_vid;
static struct uui_medialist g_list;
static struct uui_scale g_pos, g_vol;
static struct uui_transport g_tp;
static struct uui_statusbar g_status;
static struct uui_filedialog g_fd;
static struct uui_toast g_toast;
static struct uambient g_amb;
static struct uambient_stage g_stage;

static struct uvid_play *g_play;
static struct item g_items[MAX_ITEMS];
static int g_count, g_cur = -1;
static char g_dir[PATH_MAX_V] = DEFAULT_DIR;
static int g_have_sound, g_panel = 1, g_loop, g_full;
static int g_tinted;                  // the stage has taken its colours from this video
static int g_pending_play;            // a file was named: play it once the window is up
static char g_speed_label[8] = "1x";
static const int SPEEDS[] = { 50, 75, 100, 125, 150, 200 };

static char g_stat_name[72], g_stat_size[24], g_stat_codec[72], g_stat_fps[16], g_stat_drop[24], g_stat_pos[16];
static char g_stat_clock[32];           // what the pictures are timed by

// Geometry, from layout_all().
static int g_sx, g_sy, g_sw, g_sh;        // the stage
static int g_cx, g_cy, g_cw, g_ch;        // the control strip under it (or the pill)
static int g_time_y, g_title_y;
static int g_pill_x, g_pill_y, g_pill_w, g_pill_h;

// Full screen's floating bar: shown on movement, hidden after HIDE_MS.
static uint64_t g_moved_ns;
static int g_bar_shown = 1;

// --- the seek-bar preview: a still at the hovered time, on its own thread --

static pthread_t g_pv_thread;
static pthread_mutex_t g_pv_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_pv_path[PATH_MAX_V];        // under the lock: the request
static int32_t g_pv_want = -1;            // ms, -1 for none
static struct uimg g_pv_img;              // under the lock: the answer
static int32_t g_pv_have = -1;
static int g_pv_hover = -1;               // the main thread's: ms under the pointer, -1 off the bar
static int g_pv_x;

static void *preview_main(void *arg) {
    (void)arg;
    char open_path[PATH_MAX_V] = "";
    struct uvid *v = 0;
    for (;;) {
        pthread_mutex_lock(&g_pv_lock);
        int32_t want = g_pv_want;
        char path[PATH_MAX_V];
        strlcpy(path, g_pv_path, sizeof path);
        g_pv_want = -1;
        pthread_mutex_unlock(&g_pv_lock);
        if (want < 0) { sys_sleep_ms(15); continue; }   // parked by sleeping: a cond var spins here
        if (strcmp(path, open_path)) {
            uvid_close(v);
            v = 0;
            if (uvid_open(path, &v) != 0) { v = 0; open_path[0] = '\0'; continue; }
            strlcpy(open_path, path, sizeof open_path);
        }
        // A KEYFRAME, not the exact frame: fast is what a hover wants.
        const struct uvid_frame *f;
        struct uimg im = {0};
        if (uvid_seek(v, (uint32_t)want, UVID_SEEK_KEY) != 0 || uvid_next(v, &f) != 1 ||
            uvid_frame_to_uimg(f, ugfx_char_w() * 18, ugfx_char_w() * 10, &im) != 0)
            continue;
        pthread_mutex_lock(&g_pv_lock);
        uimg_free(&g_pv_img);
        g_pv_img = im;
        g_pv_have = want;
        pthread_mutex_unlock(&g_pv_lock);
        uapp_post(g_app, POST_PREVIEW, 0);
    }
    return 0;
}

static void preview_ask(int ms) {
    if (g_cur < 0) return;
    pthread_mutex_lock(&g_pv_lock);
    k_path_join(g_dir, g_items[g_cur].name, g_pv_path, sizeof g_pv_path);
    g_pv_want = ms;
    pthread_mutex_unlock(&g_pv_lock);
}

// --- menus, the bar, the keys ------------------------------------------------

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Open...",     CMD_OPEN,       "Ctrl+O"),
    UUI_MENU("Reload",      CMD_RELOAD,     "F3"),
    UUI_MENU_SEP,
    UUI_MENU("Save frame",  CMD_SAVE_FRAME, "Ctrl+S"),
    UUI_MENU("Properties",  CMD_PROPS,      "I"),
    UUI_MENU_SEP,
    UUI_MENU("Exit",        CMD_EXIT,       "Alt+F4"),
};
static const struct uui_menu_item play_items[] = {
    UUI_MENU("Play / Pause",   CMD_PLAY,   "Space"),
    UUI_MENU("Stop",           CMD_STOP,   "S"),
    UUI_MENU_SEP,
    UUI_MENU("Back 10 s",      CMD_BACK,   "Left"),
    UUI_MENU("Forward 10 s",   CMD_FWD,    "Right"),
    UUI_MENU("Previous video", CMD_PREV,   "P"),
    UUI_MENU("Next video",     CMD_NEXT,   "N"),
    UUI_MENU_SEP,
    UUI_MENU("Slower",         CMD_SLOWER, "["),
    UUI_MENU("Faster",         CMD_FASTER, "]"),
    UUI_MENU("Normal speed",   CMD_NORMAL, "="),
    UUI_MENU_SEP,
    UUI_MENU("Loop",           CMD_LOOP,   "L"),
};
static const struct uui_menu_item view_items[] = {
    UUI_MENU("Playlist",    CMD_PLAYLIST, "Ctrl+L"),
    UUI_MENU("Full screen", CMD_FULL,     "F11"),
};
static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File",     file_items),
    UUI_SUBMENU("Playback", play_items),
    UUI_SUBMENU("View",     view_items),
};

// COLOUR-CODED BY WHAT EACH COMMAND DOES (the theme's action roles).
static const struct uui_toolbar_item tb_items[] = {
    { "tb-open",       "Open (Ctrl+O)",           CMD_OPEN,       "Open", 0, 0, UTHEME_ACT_NAV },
    UUI_TOOLBAR_SEP,
    { "tb-back",       "Previous video (P)",      CMD_PREV,       0, 0, 0, UTHEME_ACT_NAV },
    { "tb-forward",    "Next video (N)",          CMD_NEXT,       0, 0, 0, UTHEME_ACT_NAV },
    UUI_TOOLBAR_SEP,
    { "tb-repeat",     "Loop (L)",                CMD_LOOP,       0, 0, 0, UTHEME_ACT_ARRANGE },
    { "tb-speed",      "Speed ([ and ])",         CMD_SPEED,      g_speed_label, 0, 0, UTHEME_ACT_ARRANGE },
    UUI_TOOLBAR_SEP,
    { "tb-camera",     "Save this frame (Ctrl+S)", CMD_SAVE_FRAME, "Save frame", 0, 0, UTHEME_ACT_CREATE },
    { "tb-info",       "Properties (I)",          CMD_PROPS,      0, 0, 0, UTHEME_ACT_NAV },
    { "tb-playlist",   "Playlist (Ctrl+L)",       CMD_PLAYLIST,   "Playlist", UUI_TB_END, 0, UTHEME_ACT_VIEW },
    { "tb-fullscreen", "Full screen (F11)",       CMD_FULL,       0, UUI_TB_END, 0, UTHEME_ACT_VIEW },
};

static unsigned item_flags(int code) {
    switch (code) {
    case CMD_PLAYLIST: return g_panel ? UUI_MI_CHECKED : 0;
    case CMD_LOOP:     return g_loop ? UUI_MI_CHECKED : 0;
    case CMD_FULL:     return g_full ? UUI_MI_CHECKED : 0;
    case CMD_NORMAL:   return g_play && uvid_play_speed(g_play) == 100 ? UUI_MI_CHECKED : 0;
    case CMD_STOP: case CMD_SAVE_FRAME: case CMD_PROPS: case CMD_BACK: case CMD_FWD:
        return g_play ? 0 : UUI_MI_DISABLED;
    default:           return 0;
    }
}

// THE KEYS ARE A TABLE of the commands the menu and the bar run -- the
// letters are VLC's and mpv's (n, p, s, l, f, [, ], =).
static const struct { int key, cmd; } KEYS[] = {
    { ' ', CMD_PLAY },  { 's', CMD_STOP },  { 'S', CMD_STOP },
    { 'p', CMD_PREV },  { 'P', CMD_PREV },  { 'n', CMD_NEXT },  { 'N', CMD_NEXT },
    { KEY_ARROW_LEFT, CMD_BACK }, { KEY_ARROW_RIGHT, CMD_FWD },
    { 'l', CMD_LOOP },  { 'L', CMD_LOOP },  { '[', CMD_SLOWER }, { ']', CMD_FASTER }, { '=', CMD_NORMAL },
    { 'i', CMD_PROPS }, { 'I', CMD_PROPS }, { 'f', CMD_FULL }, { 'F', CMD_FULL },
    { KEY_F11, CMD_FULL }, { KEY_F3, CMD_RELOAD },
    { 0x0F, CMD_OPEN }, { 0x0C, CMD_PLAYLIST }, { 0x13, CMD_SAVE_FRAME },   // Ctrl+O, Ctrl+L, Ctrl+S
};

// The full-screen bar's ground: a pane of dark glass under the controls.
// An item of its own, placed BEFORE them, so the router paints it over
// the picture and under them.
struct pill { int x, y, w, h, hidden; };
static struct pill g_pill;

static void pill_natural(const void *w, int *ow, int *oh) { (void)w; *ow = *oh = 0; }
static void pill_geometry(void *w, int x, int y, int ww, int hh) {
    struct pill *p = w; p->x = x; p->y = y; p->w = ww; p->h = hh;
}
static void pill_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct pill *p = w; *x = p->x; *y = p->y; *ow = p->w; *oh = p->h;
}
static void pill_draw(struct ugfx_surface *s, const void *w) {
    const struct pill *p = w;
    uui_glass_round_rect(s, p->x, p->y, p->w, p->h, p->h / 2, ugfx_rgb(18, 20, 28), 200, 40);
}
// Swallows a click that misses the controls on it, so it does not fall
// through to the picture and pause the video.
static int pill_hit(const void *w, int cx, int cy) {
    const struct pill *p = w;
    return uui_hit(p->x, p->y, p->w, p->h, cx, cy);
}
static int pill_press(void *w, int cx, int cy, unsigned mods) { (void)mods; return pill_hit(w, cx, cy); }
static int pill_release(void *w, int cx, int cy) { (void)w; (void)cx; (void)cy; return 1; }
static const struct uui_widget_ops pill_ops = {
    .natural_size = pill_natural, .set_geometry = pill_geometry, .bounds = pill_bounds,
    .draw = pill_draw, .hit = pill_hit, .press = pill_press, .release = pill_release,
};

// The routed widgets, in PAINT ORDER. `.name` is what the layout log
// reports each as.
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,    .widget = &g_menu, .id = ID_MENU,      .name = "menu" },
    { .ops = &uui_toolbar_ops,    .widget = &g_tb,   .id = ID_TOOLBAR,   .name = "toolbar" },
    { .ops = &uui_video_ops,      .widget = &g_vid,  .id = ID_VIDEO,     .name = "video" },
    { .ops = &uui_medialist_ops,  .widget = &g_list, .id = ID_LIST,      .name = "list",
      .flags = UUI_TRACK_HOVER },
    { .ops = &pill_ops,           .widget = &g_pill, .id = ID_PILL,      .name = "pill" },
    { .ops = &uui_scale_ops,      .widget = &g_pos,  .id = ID_POS,       .name = "pos" },
    { .ops = &uui_scale_ops,      .widget = &g_vol,  .id = ID_VOL,       .name = "vol" },
    { .ops = &uui_transport_ops,  .widget = &g_tp,   .id = ID_TRANSPORT, .name = "transport" },
};
#define W_MENU 0
#define W_TB   1
#define W_LIST 3
#define W_PILL 4
#define W_POS  5
#define W_VOL  6
#define W_TP   7

// --- the folder --------------------------------------------------------------

static int keep_video(void *ctx, const char *dir, const struct sys_dirent *e) {
    (void)ctx;
    if (e->is_dir) return 0;
    char path[PATH_MAX_V];
    if (!k_path_join(dir, e->name, path, sizeof path)) return 0;
    uint8_t head[16];
    size_t got = ufile_read_head(path, head, sizeof head);
    return got >= 4 && uvid_probe(head, got);
}

static int by_name(const void *a, const void *b) {
    return strcmp(((const struct item *)a)->name, ((const struct item *)b)->name);
}

static void scan(const char *dir) {
    static struct sys_dirent all[SYS_LISTDIR_MAX];   // static: the frame budget is 2 KiB
    if (dir != g_dir) strlcpy(g_dir, dir, sizeof g_dir);
    g_count = 0;
    int n = sys_listdir(g_dir, all, SYS_LISTDIR_MAX);
    for (int i = 0; i < n && g_count < MAX_ITEMS; i++) {
        if (!keep_video(0, g_dir, &all[i])) continue;
        struct item *it = &g_items[g_count];
        memset(it, 0, sizeof *it);
        strlcpy(it->name, all[i].name, sizeof it->name);
        char path[PATH_MAX_V];
        k_path_join(g_dir, all[i].name, path, sizeof path);
        struct uvid_info in;
        if (uvid_load_info(path, &in) == 0) {
            it->ms = in.ms;
            const char *snd = !in.audio[0] ? "" : strstr(in.audio, "MP2") ? "MP2" : strstr(in.audio, "PCM") ? "PCM" : "sound";
            snprintf(it->detail, sizeof it->detail, "%s%s%s", in.detail, snd[0] ? " + " : "", snd);
        } else {
            strlcpy(it->detail, uvid_last_error(), sizeof it->detail);
        }
        struct sys_stat st;
        if (sys_stat(path, &st) == 0) { it->mtime = st.modified; it->size = (uint32_t)st.size; }
        g_count++;
    }
    qsort(g_items, (size_t)g_count, sizeof g_items[0], by_name);
}

static int index_of(const char *name) {
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_items[i].name, name)) return i;
    return -1;
}

static void title_of(int i, char *out, size_t cap) {
    strlcpy(out, g_items[i].name, cap);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

static void list_text(void *ctx, int row, char *title, int tcap, char *detail, int dcap, char *right, int rcap) {
    (void)ctx;
    if (row < 0 || row >= g_count) return;
    title_of(row, title, (size_t)tcap);
    strlcpy(detail, g_items[row].detail, (size_t)dcap);
    if (g_items[row].ms) uduration_clock(g_items[row].ms, right, (unsigned long)rcap);
}

static const struct uimg *list_thumb(void *ctx, int row, int px) {
    (void)ctx;
    if (row < 0 || row >= g_count) return 0;
    char path[PATH_MAX_V];
    k_path_join(g_dir, g_items[row].name, path, sizeof path);
    return uthumb_get(path, &g_items[row].mtime, g_items[row].size, px);
}

static int thumb_wake(void *ctx) { (void)ctx; return uapp_post(g_app, POST_THUMB, 0) == 0; }

// --- playing --------------------------------------------------------------------

static void refresh(void) {
    int on = g_play != 0;
    g_tp.playing = on && !uvid_play_paused(g_play) && !uvid_play_ended(g_play);
    g_tp.disabled = g_count == 0;
    g_tp.no_prev = g_cur <= 0;
    g_tp.no_next = g_cur < 0 || g_cur >= g_count - 1;
    g_pos.disabled = !on;
    g_list.current = g_cur;
    if (g_cur >= 0) snprintf(g_stat_pos, sizeof g_stat_pos, "%d of %d", g_cur + 1, g_count);
    else g_stat_pos[0] = '\0';
    if (on) {
        struct uvid_play_stats st;
        uvid_play_stats(g_play, &st);
        snprintf(g_stat_drop, sizeof g_stat_drop, "%u dropped", st.dropped);
        const char *c = st.sound ? "synced to sound"
                      : !uvid_play_info(g_play)->has_audio ? "no sound track"
                      : !g_have_sound ? "no sound device"
                      : uvid_play_speed(g_play) != 100 ? "silent at this speed" : "sound ended";
        strlcpy(g_stat_clock, c, sizeof g_stat_clock);
    } else {
        g_stat_drop[0] = g_stat_clock[0] = '\0';
    }
    int sp = on ? uvid_play_speed(g_play) : 100;
    if (sp % 100 == 0) snprintf(g_speed_label, sizeof g_speed_label, "%dx", sp / 100);
    else if (sp % 10 == 0) snprintf(g_speed_label, sizeof g_speed_label, "%d.%dx", sp / 100, sp % 100 / 10);
    else snprintf(g_speed_label, sizeof g_speed_label, "%d.%02dx", sp / 100, sp % 100);
}

static void stop(void) {
    uvid_play_close(g_play);
    g_play = 0;
    g_vid.play = 0;
}

static void play_index(int i) {
    if (i < 0 || i >= g_count) return;
    stop();
    g_cur = i;
    uui_medialist_select(&g_list, i);
    char path[PATH_MAX_V];
    k_path_join(g_dir, g_items[i].name, path, sizeof path);
    int rc = uvid_play_open(&g_play, path, 0);
    title_of(i, g_stat_name, sizeof g_stat_name);
    if (rc != 0) {
        snprintf(g_stat_codec, sizeof g_stat_codec, "%s", uvid_last_error());
        g_stat_size[0] = g_stat_fps[0] = '\0';
        ulogf("video: refused %s -- %s\n", path, uvid_last_error());
        refresh();
        return;
    }
    g_vid.play = g_play;
    g_tinted = 0;
    const struct uvid_info *in = uvid_play_info(g_play);
    snprintf(g_stat_size, sizeof g_stat_size, "%d x %d", in->w, in->h);
    snprintf(g_stat_codec, sizeof g_stat_codec, "%s%s%s", in->detail, in->audio[0] ? ", " : "", in->audio);
    if (in->fps_num && in->fps_den) {
        uint32_t c = (uint32_t)(100ull * in->fps_num / in->fps_den);
        if (c % 100) snprintf(g_stat_fps, sizeof g_stat_fps, "%u.%02u fps", c / 100, c % 100);
        else snprintf(g_stat_fps, sizeof g_stat_fps, "%u fps", c / 100);
    }
    uui_scale_set_range(&g_pos, 0, (long)in->ms);
    uui_scale_set_value(&g_pos, 0);
    uapp_set_tick(g_app, 10);
    // Logged on IDENTIFICATION, so a machine with no sound device still
    // says what it opened -- the machine the GUI tests run on.
    ulogf("video: playing %s -- %s, %dx%d, %u ms, sound %s\n", path, in->detail, in->w, in->h, in->ms,
          in->audio[0] ? in->audio : "none");
    refresh();
}

static void toggle_play(void) {
    if (!g_play) { play_index(g_cur >= 0 ? g_cur : 0); return; }
    if (uvid_play_ended(g_play)) { uvid_play_seek(g_play, 0); uvid_play_set_paused(g_play, 0); }
    else uvid_play_set_paused(g_play, !uvid_play_paused(g_play));
    uapp_set_tick(g_app, uvid_play_paused(g_play) ? 250 : 10);
    ulogf("video: %s\n", uvid_play_paused(g_play) ? "paused" : "playing");
}

static void seek_by(int delta) {
    if (!g_play) return;
    int64_t t = (int64_t)uvid_play_position(g_play) + delta;
    if (t < 0) t = 0;
    uvid_play_seek(g_play, (uint32_t)t);
    uui_scale_set_value(&g_pos, (long)t);
}

static void set_speed_step(int dir) {
    if (!g_play) return;
    int cur = uvid_play_speed(g_play), n = (int)(sizeof SPEEDS / sizeof SPEEDS[0]), k = 2;
    for (int i = 0; i < n; i++) if (SPEEDS[i] == cur) k = i;
    if (dir == 0) k = 2;
    else if (dir > 0 && k < n - 1) k++;
    else if (dir < 0 && k > 0) k--;
    else if (dir == 2) k = (k + 1) % n;          // the bar's button cycles
    uvid_play_set_speed(g_play, SPEEDS[k]);
    ulogf("video: speed %d%%\n", SPEEDS[k]);
}

// SAVE FRAME: the picture showing, at its own size, as a PNG beside the
// user's files -- named after the video and the time, so two saves of
// the same moment are one file.
static void save_frame(void) {
    const struct uvid_frame *f = g_play ? uvid_play_frame(g_play) : 0;
    if (!f) return;
    struct uimg im;
    if (uvid_frame_to_uimg(f, 0, 0, &im) != 0) return;
    char t[64], path[PATH_MAX_V];
    title_of(g_cur, t, sizeof t);
    uint32_t s = (uint32_t)(f->pts_ms / 1000);
    snprintf(path, sizeof path, "%s/%s-%um%02us.png", SAVE_DIR, t, s / 60, s % 60);
    int rc = uimg_save(path, &im, "png");
    uimg_free(&im);
    char msg[96];
    if (rc == 0) snprintf(msg, sizeof msg, "Saved %s", k_path_basename(path));
    else snprintf(msg, sizeof msg, "Could not save the frame: %s", uimg_last_error());
    uui_toast_show(&g_toast, msg, uui_anim_ms(200), sys_monotonic_ns());
    ulogf("video: save frame %s -- %s\n", path, rc == 0 ? "ok" : uimg_last_error());
}

static void open_folder(const char *dir, const char *select) {
    scan(dir);
    uui_medialist_set(&g_list, g_count, list_text, list_thumb, 0);
    int i = select && select[0] ? index_of(select) : -1;
    ulogf("video: listing %s -- %d video(s)\n", g_dir, g_count);
    if (i >= 0) play_index(i);
    else { g_cur = -1; stop(); refresh(); }
}

static void open_chosen(void *ctx, const char *path) {
    (void)ctx;
    if (path) {
        char dir[PATH_MAX_V];
        if (!k_path_dirname(path, dir, sizeof dir)) strlcpy(dir, "/", sizeof dir);
        open_folder(dir, k_path_basename(path));
    }
    uapp_redraw(g_app);
}

static void open_dialog(struct uapp *a) {
    if (uui_filedialog_is_open(&g_fd)) return;
    uui_menubar_close(&g_menu);
    static const struct uui_filedialog_filter types[] = {
        { "Videos", keep_video, 0 },
        UUI_FILEDIALOG_ALL_FILES,
    };
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_OPEN, .title = "Open Video", .start_dir = g_dir,
        .filters = types, .filter_count = (int)(sizeof types / sizeof types[0]),
    };
    uui_filedialog_open(a, &g_fd, &o, open_chosen, a);
}

static void save_prefs(void) {
    uconf_set(VIDEO_CONF, "playlist", g_panel ? "on" : "off");
    uconf_set(VIDEO_CONF, "loop", g_loop ? "on" : "off");
}

static int pref_on(const char *key, int dflt) {
    char v[8];
    if (!uconf_get(VIDEO_CONF, key, v, sizeof v)) return dflt;
    return !strcmp(v, "on");
}

// --- layout --------------------------------------------------------------------

static void show_bar(int on) {
    g_bar_shown = on;
    int hide = g_full && !on;
    g_widgets[W_PILL].hidden = !g_full || hide;
    g_widgets[W_POS].hidden = g_widgets[W_VOL].hidden = g_widgets[W_TP].hidden = hide;
}

static void full_screen(struct uapp *a, int on) {
    g_full = on;
    uui_menubar_close(&g_menu);
    g_widgets[W_MENU].hidden = g_widgets[W_TB].hidden = on;
    g_widgets[W_LIST].hidden = on || !g_panel;
    g_moved_ns = sys_monotonic_ns();
    show_bar(1);
    uapp_set_fullscreen(a, on);
    ulogf("video: full screen %s\n", on ? "on" : "off");
}

static void layout_all(int cw, int ch) {
    int lh = ugfx_char_h(), pad = ugfx_char_w();
    int tw, th, sh;
    uui_transport_ops.natural_size(&g_tp, &tw, &th);
    uui_scale_natural_size(&g_pos, 0, &sh);
    int timew = ugfx_text_width("00:00:00");
    int volw = pad * 10;

    if (g_full) {
        g_sx = 0; g_sy = 0; g_sw = cw; g_sh = ch;
        uui_video_ops.set_geometry(&g_vid, 0, 0, cw, ch);
        // The floating bar: the transport, the times either side of the
        // seek bar, the volume -- one row in a capsule.
        g_pill_h = th + pad;
        g_pill_w = cw * 70 / 100;
        if (g_pill_w > pad * 110) g_pill_w = pad * 110;
        g_pill_x = (cw - g_pill_w) / 2;
        g_pill_y = ch - g_pill_h - pad * 3;
        pill_geometry(&g_pill, g_pill_x, g_pill_y, g_pill_w, g_pill_h);
        int x = g_pill_x + g_pill_h / 2, cy = g_pill_y + g_pill_h / 2;
        uui_transport_ops.set_geometry(&g_tp, x, cy - th / 2, tw, th);
        x += tw + pad;
        g_cx = x; g_time_y = cy - lh / 2;
        int right = g_pill_x + g_pill_w - g_pill_h / 2;
        uui_scale_set_geometry(&g_vol, right - volw, cy - sh / 2, volw, sh);
        int seek_r = right - volw - pad * 4 - timew;
        uui_scale_set_geometry(&g_pos, x + timew + pad, cy - sh / 2, seek_r - (x + timew + pad), sh);
        g_cw = seek_r + pad;   // where the duration is drawn
        return;
    }
    int mb = uui_menubar_height(&g_menu), tb = uui_toolbar_height(&g_tb);
    int sb = uui_statusbar_height(&g_status);
    int pw = g_panel ? pad * 36 : 0;
    if (pw > cw * 2 / 5) pw = cw * 2 / 5;
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_toolbar_ops.set_geometry(&g_tb, 0, mb, cw, tb);
    uui_statusbar_set_geometry(&g_status, 0, ch - sb, cw, sb);

    // The control strip: the seek bar with its times, then the title, the
    // transport and the volume on one row.
    int strip = pad + (sh > lh ? sh : lh) + pad / 2 + th + pad;
    g_sx = 0; g_sy = mb + tb; g_sw = cw - pw; g_sh = ch - sb - g_sy - strip;
    if (g_sh < lh * 4) g_sh = lh * 4;
    g_cx = 0; g_cy = g_sy + g_sh; g_cw = g_sw; g_ch = strip;
    uui_video_ops.set_geometry(&g_vid, g_sx + pad, g_sy + pad, g_sw - pad * 2, g_sh - pad * 2);

    int row1 = g_cy + pad;
    g_time_y = row1 + ((sh > lh ? sh : lh) - lh) / 2;
    uui_scale_set_geometry(&g_pos, g_cx + pad * 2 + timew, row1 + ((sh > lh ? sh : lh) - sh) / 2,
                           g_cw - pad * 4 - timew * 2, sh);
    int row2 = row1 + (sh > lh ? sh : lh) + pad / 2;
    uui_transport_ops.set_geometry(&g_tp, g_cx + (g_cw - tw) / 2, row2, tw, th);
    g_title_y = row2 + (th - lh) / 2;
    uui_scale_set_geometry(&g_vol, g_cx + g_cw - volw - pad * 2, row2 + (th - sh) / 2, volw, sh);

    // The playlist: a heading line, then the list.
    uui_medialist_ops.set_geometry(&g_list, cw - pw, g_sy + lh + pad, pw, ch - sb - g_sy - lh - pad);
}

// --- drawing -------------------------------------------------------------------

static void speaker(struct ugfx_surface *s, int x, int y, uint32_t c) {
    int r = ugfx_char_h() / 3;
    int xs[6] = { x, x + r, x + r * 2, x + r * 2, x + r, x };
    int ys[6] = { y - r / 2, y - r / 2, y - r, y + r, y + r / 2, y + r / 2 };
    ugfx_fill_polygon(s, xs, ys, 6, c);
}

static void draw_times(struct ugfx_surface *s, uint32_t fg, uint32_t bg) {
    char a[16] = "0:00", b[16] = "0:00";
    if (g_play) {
        uduration_clock(g_pos.dragging ? (uint64_t)uui_scale_value(&g_pos) : uvid_play_position(g_play), a, sizeof a);
        uduration_clock(uvid_play_info(g_play)->ms, b, sizeof b);
    }
    int pad = ugfx_char_w(), bw = ugfx_text_width(b);
    if (g_full) {
        ugfx_draw_string_clipped(s, g_cx, g_time_y, g_pos.x - g_cx - pad, a, fg, bg);
        ugfx_draw_string_clipped(s, g_pos.x + g_pos.w + pad, g_time_y, bw, b, fg, bg);
        return;
    }
    ugfx_draw_string_clipped(s, g_cx + pad, g_time_y, g_pos.x - g_cx - pad * 2, a, fg, bg);
    ugfx_draw_string_clipped(s, g_pos.x + g_pos.w + pad, g_time_y, bw, b, fg, bg);
}

// The controls' colours for their ground: the theme's on the chrome,
// light on the full-screen bar's dark glass.
static struct uui_scale g_scale_theme;

static void apply_ground(void) {
    g_tp.dark = g_full;
    struct uui_scale *sc[2] = { &g_pos, &g_vol };
    for (int i = 0; i < 2; i++) {
        if (g_full) {
            sc[i]->track_bg = ugfx_rgb(84, 88, 100);
            sc[i]->fill_bg = sc[i]->thumb_bg = ugfx_rgb(240, 242, 248);
        } else {
            sc[i]->track_bg = g_scale_theme.track_bg;
            sc[i]->fill_bg = g_scale_theme.fill_bg;
            sc[i]->thumb_bg = g_scale_theme.thumb_bg;
        }
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    int cw = s->w, ch = s->h;
    layout_all(cw, ch);
    apply_ground();
    refresh();
    // The stage: the ambient ground, the picture over it (a widget).
    uambient_paint(&g_stage, &g_amb, s, g_sx, g_sy, g_sw, g_sh);
    g_vid.bg = g_full ? ugfx_rgb(0, 0, 0) : g_amb.edge;
    if (g_full) return;

    // The control strip on the chrome.
    ugfx_fill_rect(s, g_cx, g_cy, g_cw, g_ch, g_amb.chrome);
    ugfx_fill_rect(s, g_cx, g_cy, g_cw, 1, g_amb.chrome_line);
    draw_times(s, UTHEME_TEXT, g_amb.chrome);
    char title[64] = "";
    if (g_cur >= 0) title_of(g_cur, title, sizeof title);
    int pad = ugfx_char_w();
    ugfx_draw_string_clipped(s, g_cx + pad * 2, g_title_y, g_tp.x - g_cx - pad * 4,
                             title[0] ? title : g_count ? "Choose a video" : "No videos here",
                             uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), g_amb.chrome);
    speaker(s, g_vol.x - ugfx_char_h() * 4 / 3, g_vol.y + g_vol.h / 2, UTHEME_TEXT);

    // The playlist's heading.
    if (g_panel) {
        int x = g_list.x, lh = ugfx_char_h();
        ugfx_fill_rect(s, x, g_sy, cw - x, g_list.y - g_sy, g_list.bg);
        ugfx_fill_rect(s, x, g_sy, 1, ch - g_sy, g_amb.chrome_line);
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, x + pad, g_sy + pad / 2, pad * 10, "Up next", utheme_action(UTHEME_ACT_VIEW), g_list.bg);
        int hw = ugfx_text_width("Up next");
        ugfx_set_font(was);
        ugfx_draw_string_clipped(s, x + pad * 2 + hw, g_sy + pad / 2, cw - x - pad * 3 - hw, g_dir,
                                 uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), g_list.bg);
        (void)lh;
    }
    uui_statusbar_draw(s, &g_status);
    ugfx_fill_rect(s, 0, g_sy - 1, cw, 1, g_amb.chrome_line);
}

// Over the widgets: the full-screen times, the seek preview, the toast.
static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = d->surface;
    uint32_t light = ugfx_rgb(240, 242, 248), glass = ugfx_rgb(40, 42, 50);
    if (g_full && g_bar_shown) {
        draw_times(s, light, glass);
        speaker(s, g_vol.x - ugfx_char_h() * 4 / 3, g_vol.y + g_vol.h / 2, light);
    }
    // THE SEEK PREVIEW: the still the preview thread found for the time
    // under the pointer, above the bar, with the time under it.
    if (g_pv_hover >= 0 && !g_widgets[W_POS].hidden) {
        pthread_mutex_lock(&g_pv_lock);
        if (g_pv_img.px) {
            int w = g_pv_img.w, h = g_pv_img.h, pad = ugfx_char_w(), lh = ugfx_char_h();
            int x = g_pv_x - w / 2, y = g_pos.y - h - lh - pad * 2;
            if (x < 4) x = 4;
            if (x + w > s->w - 4) x = s->w - 4 - w;
            uui_fill_round_rect(s, x - 3, y - 3, w + 6, h + lh + pad + 6, 6, ugfx_rgb(18, 20, 28));
            ugfx_blit(s, x, y, w, h, g_pv_img.px, w);
            char t[16];
            uduration_clock((uint64_t)g_pv_hover, t, sizeof t);
            int tw = ugfx_text_width(t);
            ugfx_draw_string_clipped(s, x + (w - tw) / 2, y + h + pad / 2, tw, t, light, ugfx_rgb(18, 20, 28));
        }
        pthread_mutex_unlock(&g_pv_lock);
    }
    if (g_toast.shown) {
        int y1 = g_full ? s->h : g_cy;
        uui_toast_draw(s, &g_toast, 0, 0, s->w, y1, sys_monotonic_ns());
    }
    // Every widget by name (ui/uui_describe.h), then what no widget knows
    // -- in WHOLE SECONDS: the block is logged again whenever a line of it
    // changes, and a per-frame value would log it thirty times a second.
    uapp_log_layout(a, "video");
    if (g_play) {
        struct uvid_play_stats st;
        uvid_play_stats(g_play, &st);
        uapp_logf_layout("video: layout state %d %u %d %d\n", uvid_play_paused(g_play),
                         uvid_play_position(g_play) / 1000, st.sound, uvid_play_ended(g_play));
    }
    // The preview's second, once its still has arrived; -1 for none.
    uapp_logf_layout("video: layout preview %d\n", g_pv_hover >= 0 && g_pv_have >= 0 ? g_pv_have / 1000 : -1);
}

// --- input ---------------------------------------------------------------------

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_OPEN:   open_dialog(a); return;
    case CMD_RELOAD: {
        char keep[64] = "";
        if (g_cur >= 0) strlcpy(keep, g_items[g_cur].name, sizeof keep);
        open_folder(g_dir, keep);
        break;
    }
    case CMD_EXIT:   uapp_quit(a, 0); return;
    case CMD_PLAY:   toggle_play(); break;
    case CMD_STOP:
        if (g_play) { uvid_play_seek(g_play, 0); uvid_play_set_paused(g_play, 1); uapp_set_tick(a, 250); }
        break;
    case CMD_PREV:   if (g_cur > 0) play_index(g_cur - 1); break;
    case CMD_NEXT:   if (g_cur < g_count - 1) play_index(g_cur + 1); break;
    case CMD_BACK:   seek_by(-SEEK_STEP); break;
    case CMD_FWD:    seek_by(SEEK_STEP); break;
    case CMD_LOOP:   g_loop = !g_loop; save_prefs(); ulogf("video: loop %s\n", g_loop ? "on" : "off"); break;
    case CMD_SLOWER: set_speed_step(-1); break;
    case CMD_FASTER: set_speed_step(+1); break;
    case CMD_NORMAL: set_speed_step(0); break;
    case CMD_SPEED:  set_speed_step(2); break;
    case CMD_SAVE_FRAME: save_frame(); break;
    case CMD_PROPS:
        if (g_cur >= 0) {
            char path[PATH_MAX_V];
            k_path_join(g_dir, g_items[g_cur].name, path, sizeof path);
            uapp_spawn(a, PROPERTIES, path);
        }
        break;
    case CMD_PLAYLIST:
        g_panel = !g_panel;
        g_widgets[W_LIST].hidden = g_full || !g_panel;
        save_prefs();
        ulogf("video: playlist %s\n", g_panel ? "on" : "off");
        break;
    case CMD_FULL:   full_screen(a, !g_full); break;
    default: return;
    }
    refresh();
    uapp_redraw(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    switch (id) {
    case ID_MENU: {
        int code = uui_menubar_take_code(&g_menu);
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
    case ID_TRANSPORT:
        switch (uui_transport_take(&g_tp)) {
        case UUI_TRANSPORT_PREV: do_command(a, CMD_PREV); break;
        case UUI_TRANSPORT_PLAY: do_command(a, CMD_PLAY); break;
        case UUI_TRANSPORT_NEXT: do_command(a, CMD_NEXT); break;
        default: uapp_redraw(a); break;
        }
        return;
    case ID_VIDEO:
        // A click pauses, a double-click goes full screen -- and undoes
        // the pause its first click made, as Windows Media Player does.
        switch (uui_video_take(&g_vid)) {
        case UUI_VIDEO_CLICK:  do_command(a, CMD_PLAY); break;
        case UUI_VIDEO_DOUBLE: do_command(a, CMD_PLAY); do_command(a, CMD_FULL); break;
        default: break;
        }
        return;
    case ID_LIST: {
        int r = uui_medialist_take(&g_list);
        if (r >= 0 && r != g_cur) play_index(r);
        uapp_redraw(a);
        return;
    }
    case ID_VOL:
        usnd_set_volume((int)uui_scale_value(&g_vol));
        uapp_redraw(a);
        return;
    case ID_POS:
        // On RELEASE: the bar follows the drag and the times show where
        // it is, and the decoder seeks once, where it is let go.
        if (reason == UUI_REASON_RELEASE && g_play) uvid_play_seek(g_play, (uint32_t)uui_scale_value(&g_pos));
        uapp_redraw(a);
        return;
    }
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (g_full) {
        g_moved_ns = sys_monotonic_ns();
        if (!g_bar_shown) { show_bar(1); uapp_redraw(a); }
    }
    // The preview follows the pointer along the seek bar.
    int over = g_play && !g_widgets[W_POS].hidden && g_pos.w > 0 &&
               uui_hit(g_pos.x, g_pos.y - ugfx_char_h() / 2, g_pos.w, g_pos.h + ugfx_char_h(), x, y);
    int ms = -1;
    if (over) {
        long dur = (long)uvid_play_info(g_play)->ms;
        ms = (int)((long long)(x - g_pos.x) * dur / g_pos.w);
        if (ms < 0) ms = 0;
        if (ms > dur) ms = (int)dur;
    }
    if (ms != g_pv_hover) {
        // A new second asks for a new still; the same one only moves it.
        if (ms >= 0 && (g_pv_hover < 0 || ms / 1000 != g_pv_hover / 1000)) preview_ask(ms);
        if (ms < 0) {
            pthread_mutex_lock(&g_pv_lock);
            uimg_free(&g_pv_img);
            g_pv_have = -1;
            pthread_mutex_unlock(&g_pv_lock);
        }
        g_pv_hover = ms;
        g_pv_x = x;
        uapp_redraw(a);
    }
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    int code;
    if (!g_full && uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    if (key == 27 && g_full) { do_command(a, CMD_FULL); return; }   // Esc leaves full screen
    for (unsigned k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++)
        if (KEYS[k].key == key) { do_command(a, KEYS[k].cmd); return; }
    if (uui_key_is_shortcut(key, mods)) return;
    if (key == '+' || key == '-') {
        uui_scale_key(&g_vol, key == '+' ? KEY_ARROW_RIGHT : KEY_ARROW_LEFT);
        usnd_set_volume((int)uui_scale_value(&g_vol));
        uapp_redraw(a);
        return;
    }
    // Up/Down move the playlist's selection; Enter plays it.
    if (!g_full && uui_medialist_ops.key(&g_list, key, mods)) {
        int r = uui_medialist_take(&g_list);
        if (r >= 0 && r != g_cur) play_index(r);
        uapp_redraw(a);
    }
}

static int on_tick(struct uapp *a) {
    int redraw = uui_toolbar_tick(&g_tb);
    if (g_play) {
        if (uvid_play_tick(g_play)) redraw = 1;
        // The stage takes its colours from the first frame of each video.
        const struct uvid_frame *f = uvid_play_frame(g_play);
        if (f && !g_tinted) {
            struct uimg im;
            if (uvid_frame_to_uimg(f, 64, 36, &im) == 0) {
                uambient_from(&g_amb, &im);
                uimg_free(&im);
                redraw = 1;
            }
            g_tinted = 1;
        }
        if (!g_pos.dragging) {
            long now = (long)uvid_play_position(g_play);
            if (now / 250 != uui_scale_value(&g_pos) / 250) redraw = 1;
            uui_scale_set_value(&g_pos, now);
        }
        // THE END: again with Loop, else the next video, else rest on
        // the last frame.
        if (uvid_play_ended(g_play) && !uvid_play_paused(g_play)) {
            if (g_loop) uvid_play_seek(g_play, 0);
            else if (g_cur < g_count - 1) play_index(g_cur + 1);
            else { uvid_play_set_paused(g_play, 1); uapp_set_tick(a, 250); ulogf("video: end of the playlist\n"); }
            redraw = 1;
        }
    }
    if (g_full && g_bar_shown && g_play && !uvid_play_paused(g_play) && g_pv_hover < 0 &&
        sys_monotonic_ns() - g_moved_ns > (uint64_t)HIDE_MS * 1000000) {
        show_bar(0);
        redraw = 1;
    }
    if (g_toast.shown && sys_monotonic_ns() - g_toast.t0_ns > 3000000000ull) {
        uui_toast_hide(&g_toast);
        redraw = 1;
    }
    if (g_toast.shown) redraw = 1;     // the rise animates
    if (uthumb_tick()) redraw = 1;
    return redraw;
}

static int on_user(struct uapp *a, int a0, int a1) {
    (void)a; (void)a1;
    if (a0 == POST_THUMB) return uthumb_posted();
    return a0 == POST_PREVIEW;
}

static void on_open(struct uapp *a) {
    g_app = a;
    layout_all(uapp_width(a), uapp_height(a));
    pthread_create(&g_pv_thread, 0, preview_main, 0);
    if (g_pending_play) play_index(g_cur);
    refresh();
}

static void on_resize(struct uapp *a, int w, int h) { (void)a; layout_all(w, h); }

static void on_size(int *w, int *h) {
    *w = ugfx_char_w() * 84;
    *h = ugfx_char_h() * 30;
}

int main(int argc, char **argv) {
    char dir[PATH_MAX_V], want[64] = "";
    strlcpy(dir, DEFAULT_DIR, sizeof dir);
    if (argc > 1 && argv[1][0]) {
        struct sys_stat st;
        if (sys_stat(argv[1], &st) == 0 && !st.is_dir) {
            if (!k_path_dirname(argv[1], dir, sizeof dir)) strlcpy(dir, DEFAULT_DIR, sizeof dir);
            strlcpy(want, k_path_basename(argv[1]), sizeof want);
        } else {
            strlcpy(dir, argv[1], sizeof dir);
        }
    }
    g_panel = pref_on("playlist", 1);
    g_loop = pref_on("loop", 0);

    // NO SOUND DEVICE IS NOT AN ERROR: the pictures play on the clock.
    g_have_sound = usnd_init() == 0;

    uui_menubar_init(&g_menu, menu_items, (int)(sizeof menu_items / sizeof menu_items[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    g_tb.accent_latch = 1;

    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_stat_name;  g_status.panes[0].chars = 0;
    g_status.panes[1].text = g_stat_size;  g_status.panes[1].chars = 10;
    g_status.panes[2].text = g_stat_codec; g_status.panes[2].chars = 0;
    g_status.panes[3].text = g_stat_fps;   g_status.panes[3].chars = 8;
    g_status.panes[4].text = g_stat_drop;  g_status.panes[4].chars = 10;
    g_status.panes[5].text = g_stat_clock; g_status.panes[5].chars = 15;
    g_status.panes[6].text = g_stat_pos;   g_status.panes[6].chars = 7;
    g_status.count = 7;

    uui_video_init(&g_vid);
    uui_medialist_init(&g_list);
    uui_scale_init(&g_pos, 0, 0, 0);
    g_pos.step = SEEK_STEP;
    uui_scale_init(&g_vol, 0, 100, usnd_volume());
    g_vol.step = 5;
    g_scale_theme = g_vol;
    uui_transport_init(&g_tp);
    uambient_default(&g_amb);

    struct uthumb_config tc = { .wake = thumb_wake, .log_prefix = "video" };
    uthumb_init(&tc);

    g_widgets[W_LIST].hidden = !g_panel;
    g_widgets[W_PILL].hidden = 1;

    struct uapp_desc desc = {
        .title        = "Video Player",
        .app_id       = "video",
        .flags        = UAPP_RESIZABLE,
        .min_w        = 480,
        .min_h        = 320,
        .tick_ms      = 250,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .on_open      = on_open,
        .on_size      = on_size,
        .on_draw      = on_draw,
        .on_draw_over = on_draw_over,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_motion    = on_motion,
        .on_tick      = on_tick,
        .on_user      = on_user,
        .on_resize    = on_resize,
    };
    // The folder is read before the window opens, so the first frame
    // already lists it; the file named plays once the window is up.
    scan(dir);
    uui_medialist_set(&g_list, g_count, list_text, list_thumb, 0);
    if (want[0]) {
        g_cur = index_of(want);
        uui_medialist_select(&g_list, g_cur);
    }
    ulogf("video: listing %s -- %d video(s)\n", g_dir, g_count);
    g_pending_play = g_cur >= 0;
    int rc = uapp_run(&desc);
    stop();
    uui_video_free(&g_vid);
    uambient_stage_free(&g_stage);
    if (g_have_sound) usnd_shutdown();
    return rc;
}
