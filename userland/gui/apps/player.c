// Audio Player -- a ring-3 client that plays sound files.
//
// IT BROWSES A DIRECTORY RATHER THAN OPENING A FILE, which is Image
// Viewer's call and made for the same reason: a file-open dialog
// belongs to the application here, so a second copy of Notepad's would
// have been the alternative. The window IS the browser -- the playable
// files in one directory down the left, the transport on the right --
// and Up/Down then do the obvious thing.
//
// THE DECODING, MIXING AND REFILLING ARE lib/usnd.h's, on ITS worker
// thread. This file contains no samples, no ring and no format: it
// calls usnd_play() and reads a position back, which is the whole point
// of the library existing (see usnd.h for the three seams).
//
// It is handed a directory or a file on the command line -- `Handles=`
// in its .desktop entry points .wav here, so the File Manager opens one
// with it -- and defaults to /usr/share/sounds.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "lib/usnd.h"
#include "kpath.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uui_fileview.h"
#include "ui/uui_label.h"
#include "ui/uui_scale.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "keyboard.h"

#define WIN_W 620
#define WIN_H 380
#define PATH_MAX_LEN 64          // FS_PATH_MAX
#define MAX_FILES 64
#define SIDEBAR_CHARS 18
#define DEFAULT_DIR "/usr/share/sounds"

#define ID_LIST  1
#define ID_POS   2
#define ID_VOL   3
#define ID_PLAY  4
#define ID_STOP  5
#define ID_PREV  6
#define ID_NEXT  7

enum {
    CMD_RELOAD = 1,
    CMD_EXIT,
    CMD_PLAY,
    CMD_STOP,
    CMD_PREV,
    CMD_NEXT,
};

static char g_dir[PATH_MAX_LEN] = DEFAULT_DIR;
static struct sys_dirent g_entries[MAX_FILES];   // 5 KB -- not a local

static struct uui_fileview g_list;
static struct uui_menubar g_menu;
static struct uui_statusbar g_status;
static struct uui_label g_now, g_fmt, g_time, g_vol_lbl;
static struct uui_scale g_pos, g_vol;
static struct uui_button g_play, g_stop, g_prev, g_next;

static char g_now_txt[PATH_MAX_LEN + 16] = "Nothing playing";
static char g_fmt_txt[64];
static char g_time_txt[32] = "0:00 / 0:00";
static char g_vol_txt[16];
static char g_stat_sink[32];
static char g_stat_note[80];

static int g_have_sound;      // did usnd_init() succeed

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Reload", CMD_RELOAD, "F3"),
    UUI_MENU_SEP,
    UUI_MENU("Exit",   CMD_EXIT,   "Alt+F4"),
};

static const struct uui_menu_item play_items[] = {
    UUI_MENU("Play / Pause", CMD_PLAY, "Space"),
    UUI_MENU("Stop",         CMD_STOP, 0),
    UUI_MENU_SEP,
    UUI_MENU("Previous",     CMD_PREV, 0),
    UUI_MENU("Next",         CMD_NEXT, 0),
};

static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File",     file_items),
    UUI_SUBMENU("Playback", play_items),
};

static struct uui_item g_widgets[] = {
    { .ops = &uui_fileview_ops, .widget = &g_list, .id = ID_LIST },
    { .ops = &uui_scale_ops, .widget = &g_pos, .id = ID_POS },
    { .ops = &uui_scale_ops, .widget = &g_vol, .id = ID_VOL },
    { .ops = &uui_button_ops, .widget = &g_play, .id = ID_PLAY },
    { .ops = &uui_button_ops, .widget = &g_stop, .id = ID_STOP },
    { .ops = &uui_button_ops, .widget = &g_prev, .id = ID_PREV },
    { .ops = &uui_button_ops, .widget = &g_next, .id = ID_NEXT },
    { .ops = &uui_label_ops, .widget = &g_now },
    { .ops = &uui_label_ops, .widget = &g_fmt },
    { .ops = &uui_label_ops, .widget = &g_time },
    { .ops = &uui_label_ops, .widget = &g_vol_lbl },
};

// --- the listing ------------------------------------------------------

// THE PROBE DECIDES, not the extension -- usnd_probe() reads the magic
// bytes, so a WAV saved as .snd is listed and a text file called
// track.wav is not. The same policy Image Viewer applies to pictures,
// and the reason the filter is a callback rather than something
// uui_fileview does itself.
static int keep_audio(void *ctx, const char *dir, const struct sys_dirent *e) {
    (void)ctx;
    if (e->is_dir) return 0;

    char path[PATH_MAX_LEN];
    if (!k_path_join(dir, e->name, path, sizeof path)) return 0;

    uint8_t head[16];
    int fd = sys_open(path, 0);
    if (fd < 0) return 0;
    long got = (long)sys_read(fd, head, sizeof head);
    sys_close(fd);
    return got >= 12 && usnd_probe(head, (size_t)got);
}

// --- formatting -------------------------------------------------------

static void fmt_time(char *out, int cap, uint64_t frames) {
    unsigned secs = (unsigned)(frames / USND_RATE);
    snprintf(out, cap, "%u:%02u", secs / 60, secs % 60);
}

static void refresh_time(void) {
    char a[16], b[16];
    fmt_time(a, sizeof a, usnd_position());
    fmt_time(b, sizeof b, usnd_duration());
    snprintf(g_time_txt, sizeof g_time_txt, "%s / %s", a, b);
}

static void refresh_transport(void) {
    // ONE BUTTON, TWO STATES -- Play and Pause are the same control on
    // every player there has ever been, and two buttons would leave one
    // of them meaningless at any moment.
    g_play.label = (usnd_playing() && !usnd_paused()) ? "Pause" : "Play";
    int idle = !usnd_playing();
    g_stop.disabled = idle;
    g_pos.disabled = idle || usnd_duration() == 0;
}

// --- playing ----------------------------------------------------------

static void play_selected(void) {
    char path[PATH_MAX_LEN];
    if (!uui_fileview_selected_path(&g_list, path, sizeof path)) return;
    const char *name = uui_fileview_selected_name(&g_list);

    struct usnd_info in;
    if (usnd_load_info(path, &in) != 0) {
        snprintf(g_now_txt, sizeof g_now_txt, "%s", name ? name : "?");
        snprintf(g_fmt_txt, sizeof g_fmt_txt, "%s", usnd_last_error());
        strlcpy(g_stat_note, "cannot play", sizeof g_stat_note);
        ulogf("player: refused %s -- %s\n", path, usnd_last_error());
        return;
    }
    snprintf(g_fmt_txt, sizeof g_fmt_txt, "%s, %s", in.format, in.detail);

    if (!g_have_sound) {
        snprintf(g_now_txt, sizeof g_now_txt, "%s", name ? name : "?");
        strlcpy(g_stat_note, "no audio device", sizeof g_stat_note);
        return;
    }
    if (usnd_play(path) != 0) {
        snprintf(g_now_txt, sizeof g_now_txt, "%s", name ? name : "?");
        strlcpy(g_stat_note, usnd_last_error(), sizeof g_stat_note);
        ulogf("player: play %s failed -- %s\n", path, usnd_last_error());
        return;
    }
    snprintf(g_now_txt, sizeof g_now_txt, "%s", name ? name : "?");
    uui_scale_set_range(&g_pos, 0, (long)usnd_duration());
    uui_scale_set_value(&g_pos, 0);
    strlcpy(g_stat_note, "playing", sizeof g_stat_note);
    refresh_time();
    refresh_transport();
    ulogf("player: playing %s -- %s, %u frames\n", path, in.detail,
          (unsigned)usnd_duration());
}

// Moves the selection by one and plays it. `uui_fileview_key` is what
// moves it, so the scroll follows the selection exactly as it does when
// the arrows are pressed.
static void step_track(int down) {
    if (!uui_fileview_key(&g_list, down ? KEY_ARROW_DOWN : KEY_ARROW_UP)) return;
    play_selected();
}

static void toggle_play(void) {
    if (!usnd_playing()) { play_selected(); return; }
    usnd_set_paused(!usnd_paused());
    strlcpy(g_stat_note, usnd_paused() ? "paused" : "playing", sizeof g_stat_note);
    refresh_transport();
}

// --- layout -----------------------------------------------------------

static int menubar_h(void) { int h; uui_menubar_natural_size(&g_menu, 0, &h); return h; }
static int statusbar_h(void) { int h; uui_statusbar_natural_size(&g_status, 0, &h); return h; }

static void layout_all(int cw, int ch) {
    int mb = menubar_h(), sb = statusbar_h();
    int gap = utheme_gap(), row = ugfx_char_h();
    int side = ugfx_char_w() * SIDEBAR_CHARS;
    if (side > cw / 2) side = cw / 2;

    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_statusbar_set_geometry(&g_status, 0, ch - sb, cw, sb);
    uui_fileview_set_geometry(&g_list, 0, mb, side, ch - mb - sb);

    int px = side + gap * 2;
    int pw = cw - px - gap * 2;
    if (pw < row * 4) pw = row * 4;
    int y = mb + gap * 2;

    uui_label_ops.set_geometry(&g_now, px, y, pw, row); y += row + gap / 2;
    uui_label_ops.set_geometry(&g_fmt, px, y, pw, row); y += row + gap * 2;

    int sh; uui_scale_natural_size(&g_pos, 0, &sh);
    uui_scale_set_geometry(&g_pos, px, y, pw, sh); y += sh + gap / 2;
    uui_label_ops.set_geometry(&g_time, px, y, pw, row); y += row + gap * 2;

    // The transport row: four equal buttons across the panel.
    int bh; uui_button_natural_size(&g_play, 0, &bh);
    int bw = (pw - gap * 3) / 4;
    uui_button_set_geometry(&g_prev, px, y, bw, bh);
    uui_button_set_geometry(&g_play, px + (bw + gap), y, bw, bh);
    uui_button_set_geometry(&g_stop, px + (bw + gap) * 2, y, bw, bh);
    uui_button_set_geometry(&g_next, px + (bw + gap) * 3, y, bw, bh);
    y += bh + gap * 2;

    uui_label_ops.set_geometry(&g_vol_lbl, px, y, pw, row); y += row + gap / 2;
    uui_scale_set_geometry(&g_vol, px, y, pw, sh);
}

// Self-reported geometry, so a GUI test asks the app where things are
// rather than guessing pixels (docs/gui-guidelines.md). The grammar is
// notepad.c's and imgview.c's, deliberately: one parser in tools/.
static void log_layout(void) {
    int x, y, w, h;
    uapp_logf_layout("player: layout pos %d %d %d %d\n", g_pos.x, g_pos.y, g_pos.w, g_pos.h);
    uapp_logf_layout("player: layout vol %d %d %d %d\n", g_vol.x, g_vol.y, g_vol.w, g_vol.h);
    uapp_logf_layout("player: layout play %d %d %d %d\n", g_play.x, g_play.y, g_play.w, g_play.h);
    uapp_logf_layout("player: layout stop %d %d %d %d\n", g_stop.x, g_stop.y, g_stop.w, g_stop.h);
    uapp_logf_layout("player: layout prev %d %d %d %d\n", g_prev.x, g_prev.y, g_prev.w, g_prev.h);
    uapp_logf_layout("player: layout next %d %d %d %d\n", g_next.x, g_next.y, g_next.w, g_next.h);
    uui_fileview_ops.bounds(&g_list, &x, &y, &w, &h);
    uapp_logf_layout("player: layout list %d %d %d %d\n", x, y, w, h);
    uapp_logf_layout("player: layout menubar %d %d %d %d\n", g_menu.x, g_menu.y, g_menu.w, g_menu.h);
    for (int i = 0; i < (int)(sizeof menu_items / sizeof menu_items[0]); i++) {
        if (!uui_menubar_title_rect(&g_menu, i, &x, &y, &w, &h)) continue;
        uapp_logf_layout("player: layout title %d %d %d %d %d\n", i, x, y, w, h);
    }
    for (int l = 0; l < uui_menubar_depth(&g_menu); l++) {
        if (uui_menubar_popup_rect(&g_menu, l, &x, &y, &w, &h))
            uapp_logf_layout("player: layout popup %d %d %d %d %d\n", l, x, y, w, h);
        for (int i = 0; uui_menubar_item_rect(&g_menu, l, i, &x, &y, &w, &h); i++)
            uapp_logf_layout("player: layout item %d %d %d %d %d %d\n", l, i, x, y, w, h);
    }
    uapp_logf_layout("player: layout state %d %d %llu %llu %d\n",
                     usnd_playing(), usnd_paused(),
                     (unsigned long long)usnd_position(),
                     (unsigned long long)usnd_duration(), usnd_volume());
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    layout_all(d->surface->w, d->surface->h);
    uui_menubar_draw(d->surface, &g_menu);
    uui_statusbar_draw(d->surface, &g_status);
    log_layout();
}

static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    uui_menubar_draw_popup(d->surface, &g_menu);
}

// --- input ------------------------------------------------------------

static void apply_volume(struct uapp *a) {
    usnd_set_volume((int)uui_scale_value(&g_vol));
    snprintf(g_vol_txt, sizeof g_vol_txt, "Volume %d%%", usnd_volume());
    uapp_redraw(a);
}

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_RELOAD: uui_fileview_reload(&g_list); break;
    case CMD_EXIT:   uapp_quit(a, 0); return;
    case CMD_PLAY:   toggle_play(); break;
    case CMD_STOP:
        usnd_stop();
        uui_scale_set_value(&g_pos, 0);
        strlcpy(g_stat_note, "stopped", sizeof g_stat_note);
        refresh_transport();
        break;
    case CMD_PREV:   step_track(0); break;
    case CMD_NEXT:   step_track(1); break;
    default: return;
    }
    uapp_redraw(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    switch (id) {
    case ID_LIST:
        if (reason == UUI_REASON_RELEASE) { play_selected(); uapp_redraw(a); }
        return;
    case ID_VOL:
        // LIVE, on every motion: a volume control that only acted when
        // you let go would be unusable, which is why uui_scale reports
        // motion as well as release.
        apply_volume(a);
        return;
    case ID_POS:
        // ON RELEASE ONLY: re-seeking the decoder per pixel of a drag is
        // work nobody asked for, and the thumb already follows the
        // pointer without it.
        if (reason == UUI_REASON_RELEASE && usnd_playing()) {
            usnd_seek_to((uint64_t)uui_scale_value(&g_pos));
            refresh_time();
            uapp_redraw(a);
        }
        return;
    }
    if (reason != UUI_REASON_RELEASE) return;
    switch (id) {
    case ID_PLAY: do_command(a, CMD_PLAY); break;
    case ID_STOP: do_command(a, CMD_STOP); break;
    case ID_PREV: do_command(a, CMD_PREV); break;
    case ID_NEXT: do_command(a, CMD_NEXT); break;
    }
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (uui_menubar_press(&g_menu, x, y)) uapp_redraw(a);
}
static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (uui_menubar_motion(&g_menu, x, y)) uapp_redraw(a);
}
static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    int code = uui_menubar_release(&g_menu, x, y);
    if (code > 0) do_command(a, code);
    else if (code == 0 && !uui_menubar_is_open(&g_menu)) uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    int code = 0;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code > 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    if (key == 0x9B) { do_command(a, CMD_RELOAD); return; }   // F3
    if (key == ' ')  { do_command(a, CMD_PLAY);   return; }

    // Volume and seeking are FORWARDED TO THE SCALES rather than
    // reimplemented here: the widget owns what a step is, and this app
    // declares no focus ring, so forwarding is what stops uui_scale's
    // `key` op being a slot nothing can reach.
    if (key == '+' || key == '=') { uui_scale_key(&g_vol, KEY_ARROW_RIGHT); apply_volume(a); return; }
    if (key == '-' || key == '_') { uui_scale_key(&g_vol, KEY_ARROW_LEFT);  apply_volume(a); return; }
    if ((key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT) && usnd_playing()) {
        if (uui_scale_key(&g_pos, key)) {
            usnd_seek_to((uint64_t)uui_scale_value(&g_pos));
            refresh_time();
            uapp_redraw(a);
        }
        return;
    }
    // The list owns the arrows: moving the selection does NOT start
    // playing, so the keyboard can browse a directory without every
    // keypress interrupting the track.
    if (uui_fileview_key(&g_list, key)) { uapp_redraw(a); return; }
    if (key == '\n' || key == '\r') { play_selected(); uapp_redraw(a); }
}

static int on_tick(struct uapp *a) {
    (void)a;
    // The thumb is NOT moved while the user is holding it -- writing the
    // playback position into a control somebody is dragging is the
    // classic seek-bar fight.
    if (!g_pos.dragging) uui_scale_set_value(&g_pos, (long)usnd_position());
    refresh_time();
    refresh_transport();
    return 1;
}

static void on_open(struct uapp *a) {
    layout_all(uapp_width(a), uapp_height(a));
    uui_fileview_reload(&g_list);
}

static void on_resize(struct uapp *a, int w, int h) { (void)a; layout_all(w, h); }

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0]) {
        struct sys_stat st;
        if (sys_stat(argv[1], &st) == 0 && !st.is_dir) {
            if (!k_path_dirname(argv[1], g_dir, sizeof g_dir))
                strlcpy(g_dir, DEFAULT_DIR, sizeof g_dir);
        } else {
            strlcpy(g_dir, argv[1], sizeof g_dir);
        }
    }

    // NO HARDWARE IS NOT AN ERROR. The default boot has no AC97
    // attached, so the window opens, lists files and says why nothing
    // plays -- the alternative is an app that refuses to start on the
    // machine most people will run it on.
    g_have_sound = usnd_init() == 0;
    snprintf(g_stat_sink, sizeof g_stat_sink, "%s",
             g_have_sound ? usnd_sink_name() : "no device");
    strlcpy(g_stat_note, g_have_sound ? "ready" : usnd_last_error(),
            sizeof g_stat_note);

    uui_menubar_init(&g_menu, menu_items,
                     (int)(sizeof menu_items / sizeof menu_items[0]));
    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_stat_sink;
    g_status.panes[0].chars = 12;
    g_status.panes[1].text = g_stat_note;
    g_status.panes[1].chars = 0;
    g_status.count = 2;

    uui_fileview_init(&g_list, 0, 0, 100, 100, g_entries, MAX_FILES);
    uui_fileview_set_mode(&g_list, UUI_FILEVIEW_LIST);
    uui_fileview_set_navigable(&g_list, 0);
    uui_fileview_set_filter(&g_list, keep_audio, NULL);
    uui_fileview_set_dir(&g_list, g_dir);

    uui_label_init(&g_now, g_now_txt);
    uui_label_init(&g_fmt, g_fmt_txt);
    uui_label_init(&g_time, g_time_txt);
    snprintf(g_vol_txt, sizeof g_vol_txt, "Volume %d%%", usnd_volume());
    uui_label_init(&g_vol_lbl, g_vol_txt);

    uui_scale_init(&g_pos, 0, 0, 0);
    g_pos.step = USND_RATE * 5;   // an arrow seeks five seconds, not 1% of the track
    uui_scale_init(&g_vol, 0, 100, usnd_volume());
    g_vol.step = 5;

    uui_button_init(&g_prev, 0, 0, 0, 0, "Prev", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_PREV);
    uui_button_init(&g_play, 0, 0, 0, 0, "Play", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_PLAY);
    uui_button_init(&g_stop, 0, 0, 0, 0, "Stop", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_STOP);
    uui_button_init(&g_next, 0, 0, 0, 0, "Next", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_NEXT);
    refresh_transport();

    struct uapp_desc desc = {
        .title        = "Audio Player",
        .app_id       = "player",
        .w            = WIN_W,
        .h            = WIN_H,
        .flags        = UAPP_RESIZABLE,
        .min_w        = 420,
        .min_h        = 280,
        .tick_ms      = 250,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_draw_over = on_draw_over,
        .on_widget    = on_widget,
        .on_press     = on_press,
        .on_motion    = on_motion,
        .on_release   = on_release,
        .on_key       = on_key,
        .on_tick      = on_tick,
        .on_resize    = on_resize,
    };
    int rc = uapp_run(&desc);
    if (g_have_sound) usnd_shutdown();
    return rc;
}
