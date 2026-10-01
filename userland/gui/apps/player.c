// Audio Player -- a ring-3 client that plays sound files.
//
// THE SHAPE IS THE DESIGN LANGUAGE'S (docs/gui-guidelines.md): menu bar,
// a command bar coloured by role, the track as the HERO on a stage
// tinted by its cover, the playlist as a panel on the RIGHT that the
// bar toggles, a status bar of readouts, and full screen that drops the
// chrome. Chosen from mockups on 2026-10-01 (Amberol's stage, the Image
// Viewer's chrome).
//
// THE PARTS: the folder, its tags and the play order are
// userland/player/pl_list.c; the cover, its colours and the spectrum
// pl_stage.c. Decoding, mixing and refilling are lib/usnd.h's, on ITS
// worker thread -- this calls usnd_play() and reads a position back.
//
// It is handed a directory or a file on the command line -- `Handles=`
// in its .desktop entry points .wav, .mp3 and .mid here -- and
// defaults to /usr/share/music.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "player/player_internal.h"
#include "lib/usnd.h"
#include "lib/uconf.h"
#include "lib/ufile.h"
#include "lib/unum.h"
#include "kpath.h"
#include "ui/uui.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_scale.h"
#include "ui/uui_table.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_transport.h"
#include "ui/uui_primitives.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "keyboard.h"

// This app's own preferences: the toggles, remembered (an app's own
// preferences are the app's, not the settings registry's).
#define PLAYER_CONF "/etc/player.conf"

enum { ID_MENU = 1, ID_TOOLBAR, ID_LIST, ID_POS, ID_VOL, ID_TRANSPORT };

enum {
    CMD_OPEN = 1, CMD_RELOAD, CMD_EXIT,
    CMD_PLAY, CMD_STOP, CMD_PREV, CMD_NEXT, CMD_SHUFFLE, CMD_REPEAT,
    CMD_PLAYLIST, CMD_VIZ, CMD_FULL,
};

static struct uapp *g_app;
static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
static struct uui_table g_list;
static struct uui_scale g_pos, g_vol;
static struct uui_transport g_tp;
static struct uui_statusbar g_status;
static struct uui_filedialog g_fd;
static struct uambient_stage g_stage;

static int g_cur = -1;            // the track on the stage
static int g_have_sound;
static int g_panel = 1, g_viz = 1, g_full;
static int g_was_playing, g_user_stopped;
static char g_want[64];           // the file named on the command line

static char g_stat_sink[32], g_stat_fmt[64], g_stat_pos[24], g_stat_note[80];

// The stage's geometry, from layout_all(), for the drawing.
static int g_sx, g_sy, g_sw, g_sh;
static int g_art_x, g_art_y, g_art;
static int g_title_y, g_sub_y, g_spec_y, g_spec_h, g_time_y;

// --- menus, the bar, the keys ------------------------------------------------

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Open...", CMD_OPEN,   "Ctrl+O"),
    UUI_MENU("Reload",  CMD_RELOAD, "F3"),
    UUI_MENU_SEP,
    UUI_MENU("Exit",    CMD_EXIT,   "Alt+F4"),
};
static const struct uui_menu_item play_items[] = {
    UUI_MENU("Play / Pause", CMD_PLAY, "Space"),
    UUI_MENU("Stop",         CMD_STOP, "S"),
    UUI_MENU_SEP,
    UUI_MENU("Previous",     CMD_PREV, "P"),
    UUI_MENU("Next",         CMD_NEXT, "N"),
    UUI_MENU_SEP,
    UUI_MENU("Shuffle",      CMD_SHUFFLE, "R"),
    UUI_MENU("Repeat",       CMD_REPEAT,  "L"),
};
static const struct uui_menu_item view_items[] = {
    UUI_MENU("Playlist",     CMD_PLAYLIST, "Ctrl+L"),
    UUI_MENU("Visualiser",   CMD_VIZ,      "V"),
    UUI_MENU_SEP,
    UUI_MENU("Full screen",  CMD_FULL,     "F11"),
};
static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File",     file_items),
    UUI_SUBMENU("Playback", play_items),
    UUI_SUBMENU("View",     view_items),
};

// COLOUR-CODED BY WHAT EACH COMMAND DOES (the theme's action roles).
static const struct uui_toolbar_item tb_items[] = {
    { "tb-open",       "Open (Ctrl+O)",       CMD_OPEN,     "Open", 0, 0, UTHEME_ACT_NAV },
    UUI_TOOLBAR_SEP,
    { "tb-shuffle",    "Shuffle (R)",         CMD_SHUFFLE,  0, 0, 0, UTHEME_ACT_ARRANGE },
    { "tb-repeat",     "Repeat (L)",          CMD_REPEAT,   0, 0, 0, UTHEME_ACT_ARRANGE },
    { "tb-playlist",   "Playlist (Ctrl+L)",   CMD_PLAYLIST, "Playlist", UUI_TB_END, 0, UTHEME_ACT_VIEW },
    { "tb-fullscreen", "Full screen (F11)",   CMD_FULL,     0, UUI_TB_END, 0, UTHEME_ACT_VIEW },
};

// ONE STATE SOURCE for the menu's ticks and the bar's latches.
static unsigned item_flags(int code) {
    switch (code) {
    case CMD_SHUFFLE:  return g_shuffle ? UUI_MI_CHECKED : 0;
    case CMD_REPEAT:   return g_repeat ? UUI_MI_CHECKED : 0;
    case CMD_PLAYLIST: return g_panel ? UUI_MI_CHECKED : 0;
    case CMD_VIZ:      return g_viz ? UUI_MI_CHECKED : 0;
    case CMD_FULL:     return g_full ? UUI_MI_CHECKED : 0;
    case CMD_STOP:     return usnd_playing() ? 0 : UUI_MI_DISABLED;
    default:           return 0;
    }
}

// THE KEYS ARE A TABLE of the commands the menu and the bar run --
// letters are VLC's (n, p, s, r, l, v).
static const struct { int key, cmd; } KEYS[] = {
    { ' ', CMD_PLAY },  { 's', CMD_STOP },  { 'S', CMD_STOP },
    { 'p', CMD_PREV },  { 'P', CMD_PREV },  { 'n', CMD_NEXT },  { 'N', CMD_NEXT },
    { 'r', CMD_SHUFFLE }, { 'R', CMD_SHUFFLE }, { 'l', CMD_REPEAT }, { 'L', CMD_REPEAT },
    { 'v', CMD_VIZ },   { 'V', CMD_VIZ },   { KEY_F11, CMD_FULL }, { KEY_F3, CMD_RELOAD },
    { 0x0F, CMD_OPEN }, { 0x0C, CMD_PLAYLIST },      // Ctrl+O, Ctrl+L: control codes
};

// The routed widgets. `.name` is what the layout log reports each as.
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu, .id = ID_MENU,      .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,   .id = ID_TOOLBAR,   .name = "toolbar" },
    { .ops = &uui_table_ops,     .widget = &g_list, .id = ID_LIST,      .name = "list" },
    { .ops = &uui_scale_ops,     .widget = &g_pos,  .id = ID_POS,       .name = "pos" },
    { .ops = &uui_scale_ops,     .widget = &g_vol,  .id = ID_VOL,       .name = "vol" },
    { .ops = &uui_transport_ops, .widget = &g_tp,   .id = ID_TRANSPORT, .name = "transport" },
};
#define W_MENU 0
#define W_TB   1
#define W_LIST 2

// The format beside the title: a folder holding the same piece as MIDI
// and as MP3 -- this image's own -- otherwise shows two identical rows.
static const struct uui_table_column list_cols[] = {
    { "Title",  0, UUI_TALIGN_LEFT },
    { "Format", 5, UUI_TALIGN_LEFT },
    { "Length", 5, UUI_TALIGN_RIGHT },
};

static void list_cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (row < 0 || row >= g_track_count) return;
    if (col == 0) snprintf(out, (size_t)cap, "%s", g_tracks[row].title);
    else if (col == 1) snprintf(out, (size_t)cap, "%s", g_tracks[row].format);
    else pl_fmt_ms(out, (size_t)cap, g_tracks[row].ms);
}

static void save_prefs(void) {
    uconf_set(PLAYER_CONF, "shuffle", g_shuffle ? "on" : "off");
    uconf_set(PLAYER_CONF, "repeat", g_repeat ? "on" : "off");
    uconf_set(PLAYER_CONF, "playlist", g_panel ? "on" : "off");
    uconf_set(PLAYER_CONF, "visualiser", g_viz ? "on" : "off");
}

static int pref_on(const char *key, int dflt) {
    char v[8];
    if (!uconf_get(PLAYER_CONF, key, v, sizeof v)) return dflt;
    return !strcmp(v, "on");
}

// --- playing -------------------------------------------------------------------

static void fmt_frames(char *out, size_t cap, uint64_t frames) {
    unsigned secs = (unsigned)(frames / USND_RATE);
    snprintf(out, cap, "%u:%02u", secs / 60, secs % 60);
}

static void show_selection(int i) {
    g_list.selected = i;
    int vis = uui_table_visible_rows(&g_list);
    if (i >= 0 && i < g_list.top) g_list.top = i;
    else if (i >= 0 && vis > 0 && i >= g_list.top + vis) g_list.top = i - vis + 1;
}

static void refresh_transport(void) {
    g_tp.playing = usnd_playing() && !usnd_paused();
    g_tp.disabled = g_track_count == 0;
    g_tp.no_prev = pl_step(g_cur, -1) < 0 && g_cur >= 0;
    g_tp.no_next = pl_step(g_cur, +1) < 0 && g_cur >= 0;
    g_pos.disabled = !usnd_playing() || usnd_duration() == 0;
    if (g_cur >= 0) snprintf(g_stat_pos, sizeof g_stat_pos, "%d of %d", g_cur + 1, g_track_count);
    else g_stat_pos[0] = '\0';
}

static void play_index(int i) {
    if (i < 0 || i >= g_track_count) return;
    g_cur = i;
    show_selection(i);
    stage_set_track(i);
    char path[PL_PATH_MAX];
    pl_path(i, path, sizeof path);

    struct usnd_info in;
    if (usnd_load_info(path, &in) != 0) {
        snprintf(g_stat_fmt, sizeof g_stat_fmt, "%s", usnd_last_error());
        strlcpy(g_stat_note, "cannot play", sizeof g_stat_note);
        ulogf("player: refused %s -- %s\n", path, usnd_last_error());
        return;
    }
    snprintf(g_stat_fmt, sizeof g_stat_fmt, "%s, %s", g_tracks[i].format, in.detail);
    // Logged on IDENTIFICATION rather than on playback, because a machine
    // with no audio device returns below and would otherwise report
    // nothing at all -- which is exactly the machine the GUI test runs on.
    ulogf("player: opened %s -- %s, %s\n", path, in.format, in.detail);
    // What the stage shows, from the tags -- a test's evidence that the
    // title is the tag's and not the file name.
    ulogf("player: now %s / %s\n", g_tracks[i].title,
          g_tracks[i].artist[0] ? g_tracks[i].artist : "-");
    if (!g_have_sound) {
        strlcpy(g_stat_note, "no audio device", sizeof g_stat_note);
        return;
    }
    if (usnd_play(path) != 0) {
        strlcpy(g_stat_note, usnd_last_error(), sizeof g_stat_note);
        ulogf("player: play %s failed -- %s\n", path, usnd_last_error());
        return;
    }
    g_user_stopped = 0;
    g_was_playing = 1;
    uui_scale_set_range(&g_pos, 0, (long)usnd_duration());
    uui_scale_set_value(&g_pos, 0);
    strlcpy(g_stat_note, "playing", sizeof g_stat_note);
    refresh_transport();
    ulogf("player: playing %s -- %s, %u frames\n", path, in.detail, (unsigned)usnd_duration());
}

static void toggle_play(void) {
    if (!usnd_playing()) { play_index(g_cur >= 0 ? g_cur : pl_step(-1, +1)); return; }
    usnd_set_paused(!usnd_paused());
    strlcpy(g_stat_note, usnd_paused() ? "paused" : "playing", sizeof g_stat_note);
    refresh_transport();
}

static void step(int dir) {
    int n = pl_step(g_cur, dir);
    if (n >= 0) play_index(n);
}

static void load_folder(const char *dir, const char *select) {
    pl_scan(dir);
    uui_table_set_rows(&g_list, g_track_count);
    int i = select && select[0] ? pl_index_of(select) : -1;
    g_cur = -1;
    if (i >= 0) { g_cur = i; show_selection(i); }
    stage_set_track(g_cur >= 0 ? g_cur : (g_track_count ? 0 : -1));
    ulogf("player: listing %s -- %d track(s)\n", g_dir, g_track_count);
}

// --- opening -------------------------------------------------------------------

static int keep_audio(void *ctx, const char *dir, const struct sys_dirent *e) {
    (void)ctx;
    if (e->is_dir) return 0;
    char path[PL_PATH_MAX];
    if (!k_path_join(dir, e->name, path, sizeof path)) return 0;
    uint8_t head[16];
    size_t got = ufile_read_head(path, head, sizeof head);
    return got >= 12 && usnd_probe(head, got);
}

// CHOSEN MEANS PLAY IT: the folder becomes the playlist, the file plays.
static void open_chosen(void *ctx, const char *path) {
    struct uapp *a = ctx;
    if (path) {
        char dir[PL_PATH_MAX];
        if (!k_path_dirname(path, dir, sizeof dir)) strlcpy(dir, "/", sizeof dir);
        load_folder(dir, k_path_basename(path));
        play_index(pl_index_of(k_path_basename(path)));
    }
    uapp_redraw(a);
}

static void open_dialog(struct uapp *a) {
    if (uui_filedialog_is_open(&g_fd)) return;
    uui_menubar_close(&g_menu);
    static const struct uui_filedialog_filter types[] = {
        { "Sound files", keep_audio, 0 },
        UUI_FILEDIALOG_ALL_FILES,
    };
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_OPEN, .title = "Open Sound File", .start_dir = g_dir,
        .filters = types, .filter_count = (int)(sizeof types / sizeof types[0]),
    };
    uui_filedialog_open(a, &g_fd, &o, open_chosen, a);
}

// --- layout --------------------------------------------------------------------

static void full_screen(struct uapp *a, int on) {
    g_full = on;
    uui_menubar_close(&g_menu);
    g_widgets[W_MENU].hidden = g_widgets[W_TB].hidden = on;
    g_widgets[W_LIST].hidden = on || !g_panel;
    uapp_set_fullscreen(a, on);
    ulogf("player: full screen %s\n", on ? "on" : "off");
}

static void layout_all(int cw, int ch) {
    int lh = ugfx_char_h(), pad = ugfx_char_w();
    int mb = g_full ? 0 : uui_menubar_height(&g_menu);
    int tb = g_full ? 0 : uui_toolbar_height(&g_tb);
    int sb = g_full ? 0 : uui_statusbar_height(&g_status);
    int pw = (!g_full && g_panel) ? lh * 18 : 0;
    if (pw > cw / 2) pw = cw / 2;

    if (!g_full) {
        uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
        uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
        uui_toolbar_ops.set_geometry(&g_tb, 0, mb, cw, tb);
        uui_statusbar_set_geometry(&g_status, 0, ch - sb, cw, sb);
    }
    g_sx = 0; g_sy = mb + tb; g_sw = cw - pw; g_sh = ch - sb - g_sy;
    // The playlist: a header line, then the table.
    uui_table_ops.set_geometry(&g_list, cw - pw, g_sy + lh + pad, pw, g_sh - lh - pad);

    // THE STAGE, a centred column: the cover, the spectrum, the title,
    // the artist, the seek bar and its times, the transport.
    int tw, th;
    uui_transport_ops.natural_size(&g_tp, &tw, &th);
    int sh;
    uui_scale_natural_size(&g_pos, 0, &sh);
    int big = ugfx_font_display()->line_h;
    g_spec_h = g_viz ? lh * 3 / 2 : 0;
    int fixed = g_spec_h + pad + big + lh + pad * 2 + sh + lh + pad + th;
    g_art = g_sh - fixed - pad * 4;
    if (g_art > g_sw * 45 / 100) g_art = g_sw * 45 / 100;
    if (g_art > lh * 18) g_art = lh * 18;
    if (g_art < lh * 3) g_art = lh * 3;
    int y = g_sy + (g_sh - (g_art + fixed)) / 2;
    if (y < g_sy + pad) y = g_sy + pad;
    g_art_x = g_sx + (g_sw - g_art) / 2; g_art_y = y; y += g_art + pad / 2;
    g_spec_y = y; y += g_spec_h + pad / 2;
    g_title_y = y; y += big;
    g_sub_y = y; y += lh + pad * 2;
    int seek_w = g_sw * 55 / 100;
    if (seek_w > pad * 48) seek_w = pad * 48;
    uui_scale_set_geometry(&g_pos, g_sx + (g_sw - seek_w) / 2, y, seek_w, sh);
    y += sh;
    g_time_y = y; y += lh + pad;
    uui_transport_ops.set_geometry(&g_tp, g_sx + (g_sw - tw) / 2, y, tw, th);
    // Volume: the stage's bottom-right corner, a speaker before it.
    int vw = pad * 10;
    uui_scale_set_geometry(&g_vol, g_sx + g_sw - vw - pad * 2, g_sy + g_sh - sh - pad, vw, sh);
}

// --- drawing -------------------------------------------------------------------

static void centred(struct ugfx_surface *s, int y, const char *t, uint32_t fg, uint32_t bg) {
    int w = ugfx_text_width(t);
    int x = g_sx + (g_sw - w) / 2;
    if (x < g_sx + 4) x = g_sx + 4;
    ugfx_draw_string_clipped(s, x, y, g_sw - (x - g_sx) - 4, t, fg, bg);
}

static void draw_stage(struct ugfx_surface *s) {
    uambient_paint(&g_stage, &g_amb, s, g_sx, g_sy, g_sw, g_sh);
    uint32_t fg = ugfx_rgb(244, 245, 250), dim = ugfx_rgb(196, 200, 212);
    uint32_t bg = g_amb.centre;   // text sits near the lit middle

    const struct uimg *c = stage_cover(g_art);
    if (c) ugfx_blit(s, g_art_x, g_art_y, c->w, c->h, c->px, c->w);

    if (g_viz) {
        int bw = g_art / STAGE_BANDS;
        for (int k = 0; k < STAGE_BANDS && bw > 1; k++) {
            int h = g_levels[k] * g_spec_h / 255;
            if (h < 1) continue;            // silence draws nothing, not a dashed line
            ugfx_fill_rect(s, g_art_x + k * bw + 1, g_spec_y + g_spec_h - h, bw - 2, h,
                           ugfx_blend(g_amb.centre, fg, 170));
        }
    }

    const struct pl_track *t = g_cur >= 0 ? &g_tracks[g_cur] : 0;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_display());
    centred(s, g_title_y, t ? t->title : g_track_count ? "Nothing playing" : "No sound files here", fg, bg);
    ugfx_set_font(was);
    char sub[160] = "";
    if (t && (t->artist[0] || t->album[0]))
        snprintf(sub, sizeof sub, "%s%s%s", t->artist, t->artist[0] && t->album[0] ? "  -  " : "",
                 t->album);
    else if (!t) snprintf(sub, sizeof sub, "%s", g_track_count ? "Choose a track in the playlist"
                                                               : "Open a folder with File > Open");
    centred(s, g_sub_y, sub, dim, bg);

    char a[16], b[16];
    fmt_frames(a, sizeof a, usnd_playing() ? usnd_position() : 0);
    if (usnd_playing() && usnd_duration()) fmt_frames(b, sizeof b, usnd_duration());
    else pl_fmt_ms(b, sizeof b, t ? t->ms : 0);
    ugfx_draw_string_clipped(s, g_pos.x, g_time_y, g_pos.w / 2, a, dim, bg);
    int bw = ugfx_text_width(b);
    ugfx_draw_string_clipped(s, g_pos.x + g_pos.w - bw, g_time_y, bw, b, dim, bg);

    // The speaker before the volume slider: a box and a cone, two-thirds
    // of a line tall.
    int r = ugfx_char_h() / 3, vx = g_vol.x - r * 4, vy = g_vol.y + g_vol.h / 2;
    int xs[6] = { vx, vx + r, vx + r * 2, vx + r * 2, vx + r, vx };
    int ys[6] = { vy - r / 2, vy - r / 2, vy - r, vy + r, vy + r / 2, vy + r / 2 };
    ugfx_fill_polygon(s, xs, ys, 6, dim);
}

static void draw_panel(struct ugfx_surface *s, int cw) {
    if (g_full || !g_panel) return;
    int x = g_list.x, lh = ugfx_char_h(), pad = ugfx_char_w();
    ugfx_fill_rect(s, x, g_sy, cw - x, g_list.y - g_sy, g_amb.panel);
    ugfx_fill_rect(s, x, g_sy, 1, g_sh, g_amb.chrome_line);
    char head[PL_PATH_MAX + 16];
    snprintf(head, sizeof head, "%s", g_dir);
    char count[16];
    snprintf(count, sizeof count, "%d", g_track_count);
    int cwid = ugfx_text_width(count);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, x + pad, g_sy + pad / 2, cw - x - pad * 3 - cwid, head,
                             UTHEME_TEXT, g_amb.panel);
    ugfx_set_font(was);
    ugfx_draw_string_clipped(s, cw - pad - cwid, g_sy + pad / 2, cwid, count,
                             uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), g_amb.panel);
    (void)lh;
}

static void apply_chrome(void) {
    g_menu.bar_bg = g_amb.chrome;
    g_tb.bg = g_amb.chrome;
    g_status.bg = g_amb.chrome;
    // The seek and volume bars on the stage: light on the dark ground.
    g_pos.track_bg = g_vol.track_bg = ugfx_blend(g_amb.edge, ugfx_rgb(255, 255, 255), 70);
    g_pos.fill_bg = g_vol.fill_bg = ugfx_rgb(244, 245, 250);
    g_pos.thumb_bg = g_vol.thumb_bg = ugfx_rgb(244, 245, 250);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    int cw = d->surface->w, ch = d->surface->h;
    apply_chrome();
    layout_all(cw, ch);
    refresh_transport();
    draw_stage(d->surface);
    draw_panel(d->surface, cw);
    if (!g_full) {
        uui_statusbar_draw(d->surface, &g_status);
        ugfx_fill_rect(d->surface, 0, g_sy - 1, cw, 1, g_amb.chrome_line);
    }
    // Every widget by name (ui/uui_describe.h), then the one fact no
    // widget knows -- the transport's state.
    uapp_log_layout(a, "player");
    uapp_logf_layout("player: layout state %d %d %llu %llu %d\n",
                     usnd_playing(), usnd_paused(),
                     (unsigned long long)usnd_position(),
                     (unsigned long long)usnd_duration(), usnd_volume());
}

// --- input ---------------------------------------------------------------------

static void apply_volume(struct uapp *a) {
    usnd_set_volume((int)uui_scale_value(&g_vol));
    uapp_redraw(a);
}

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_OPEN:   open_dialog(a); return;
    case CMD_RELOAD: {
        char keep[64] = "";
        if (g_cur >= 0) strlcpy(keep, g_tracks[g_cur].name, sizeof keep);
        load_folder(g_dir, keep);
        break;
    }
    case CMD_EXIT:   uapp_quit(a, 0); return;
    case CMD_PLAY:   toggle_play(); break;
    case CMD_STOP:
        usnd_stop();
        g_user_stopped = 1;
        uui_scale_set_value(&g_pos, 0);
        strlcpy(g_stat_note, "stopped", sizeof g_stat_note);
        break;
    case CMD_PREV:   step(-1); break;
    case CMD_NEXT:   step(+1); break;
    case CMD_SHUFFLE:
        g_shuffle = !g_shuffle;
        if (g_shuffle) pl_reshuffle(g_cur);
        save_prefs();
        ulogf("player: shuffle %s\n", g_shuffle ? "on" : "off");
        break;
    case CMD_REPEAT:
        g_repeat = !g_repeat;
        save_prefs();
        ulogf("player: repeat %s\n", g_repeat ? "on" : "off");
        break;
    case CMD_PLAYLIST:
        g_panel = !g_panel;
        g_widgets[W_LIST].hidden = g_full || !g_panel;
        save_prefs();
        ulogf("player: playlist %s\n", g_panel ? "on" : "off");
        break;
    case CMD_VIZ:
        g_viz = !g_viz;
        save_prefs();
        break;
    case CMD_FULL:   full_screen(a, !g_full); break;
    default: return;
    }
    refresh_transport();
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
    case ID_TRANSPORT:
        switch (uui_transport_take(&g_tp)) {
        case UUI_TRANSPORT_PREV: do_command(a, CMD_PREV); break;
        case UUI_TRANSPORT_PLAY: do_command(a, CMD_PLAY); break;
        case UUI_TRANSPORT_NEXT: do_command(a, CMD_NEXT); break;
        default: uapp_redraw(a); break;
        }
        return;
    case ID_LIST:
        // A row clicked PLAYS -- the playlist is for choosing what to hear.
        if (reason == UUI_REASON_RELEASE && g_list.selected >= 0 && g_list.selected != g_cur)
            play_index(g_list.selected);
        uapp_redraw(a);
        return;
    case ID_VOL:
        // LIVE, on every motion: a volume control that only acted when
        // you let go would be unusable.
        apply_volume(a);
        return;
    case ID_POS:
        // ON RELEASE ONLY: re-seeking the decoder per pixel of a drag is
        // work nobody asked for, and the thumb follows the pointer anyway.
        if (reason == UUI_REASON_RELEASE && usnd_playing()) {
            usnd_seek_to((uint64_t)uui_scale_value(&g_pos));
            uapp_redraw(a);
        }
        return;
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
    // Volume and seeking go TO THE SCALES: the widget owns what a step is.
    if (key == '+' || key == '=') { uui_scale_key(&g_vol, KEY_ARROW_RIGHT); apply_volume(a); return; }
    if (key == '-' || key == '_') { uui_scale_key(&g_vol, KEY_ARROW_LEFT);  apply_volume(a); return; }
    if ((key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT) && usnd_playing()) {
        if (uui_scale_key(&g_pos, key)) {
            usnd_seek_to((uint64_t)uui_scale_value(&g_pos));
            uapp_redraw(a);
        }
        return;
    }
    // Up/Down move the playlist's selection WITHOUT playing; Enter plays.
    if (key == '\n' || key == '\r') { play_index(g_list.selected); uapp_redraw(a); return; }
    if (uui_table_key(&g_list, key)) uapp_redraw(a);
}

static int on_tick(struct uapp *a) {
    (void)a;
    int redraw = uui_toolbar_tick(&g_tb);
    int playing = usnd_playing();
    // THE NEXT TRACK PLAYS when one ends on its own -- not after Stop,
    // and not past the end without Repeat.
    if (g_have_sound && g_was_playing && !playing && !g_user_stopped) {
        int n = pl_step(g_cur, +1);
        if (n >= 0) play_index(n);
        else {
            strlcpy(g_stat_note, "end of the playlist", sizeof g_stat_note);
            ulogf("player: end of the playlist\n");
        }
        redraw = 1;
    }
    g_was_playing = usnd_playing();
    if (!g_pos.dragging && playing) {
        uui_scale_set_value(&g_pos, (long)usnd_position());
        redraw = 1;
    }
    if (stage_spectrum_tick(g_viz && playing && !usnd_paused())) redraw = 1;
    return redraw;
}

static void on_open(struct uapp *a) {
    g_app = a;
    layout_all(uapp_width(a), uapp_height(a));
}

static void on_resize(struct uapp *a, int w, int h) { (void)a; layout_all(w, h); }

static void on_size(int *w, int *h) {
    *w = ugfx_char_w() * 64;
    *h = ugfx_char_h() * 42;
}

int main(int argc, char **argv) {
    char dir[PL_PATH_MAX];
    strlcpy(dir, g_dir, sizeof dir);
    if (argc > 1 && argv[1][0]) {
        struct sys_stat st;
        if (sys_stat(argv[1], &st) == 0 && !st.is_dir) {
            if (!k_path_dirname(argv[1], dir, sizeof dir)) strlcpy(dir, g_dir, sizeof dir);
            strlcpy(g_want, k_path_basename(argv[1]), sizeof g_want);
        } else {
            strlcpy(dir, argv[1], sizeof dir);
        }
    }
    g_shuffle = pref_on("shuffle", 0);
    g_repeat = pref_on("repeat", 0);
    g_panel = pref_on("playlist", 1);
    g_viz = pref_on("visualiser", 1);
    srand((unsigned)sys_ticks());

    // NO HARDWARE IS NOT AN ERROR: the window opens, lists files and says
    // why nothing plays.
    g_have_sound = usnd_init() == 0;
    snprintf(g_stat_sink, sizeof g_stat_sink, "%s", g_have_sound ? usnd_sink_name() : "no device");
    strlcpy(g_stat_note, g_have_sound ? "ready" : usnd_last_error(), sizeof g_stat_note);

    uui_menubar_init(&g_menu, menu_items, (int)(sizeof menu_items / sizeof menu_items[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    g_tb.accent_latch = 1;   // a latched toggle is filled in the accent

    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_stat_sink;  g_status.panes[0].chars = 12;
    g_status.panes[1].text = g_stat_fmt;   g_status.panes[1].chars = 0;
    g_status.panes[2].text = g_stat_pos;   g_status.panes[2].chars = 8;
    g_status.panes[3].text = g_stat_note;  g_status.panes[3].chars = 16;
    g_status.count = 4;

    uui_table_init(&g_list, 0, 0, 0, 0, list_cols, 3, list_cell, 0);
    uui_table_set_header(&g_list, 0);
    uui_scale_init(&g_pos, 0, 0, 0);
    g_pos.step = USND_RATE * 5;   // an arrow seeks five seconds, not 1% of the track
    uui_scale_init(&g_vol, 0, 100, usnd_volume());
    g_vol.step = 5;
    uui_transport_init(&g_tp);
    g_tp.dark = 1;
    uambient_default(&g_amb);

    load_folder(dir, g_want);
    g_widgets[W_LIST].hidden = !g_panel;

    struct uapp_desc desc = {
        .title        = "Audio Player",
        .app_id       = "player",
        .flags        = UAPP_RESIZABLE,
        .min_w        = 420,
        .min_h        = 300,
        .tick_ms      = 100,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .on_open      = on_open,
        .on_size      = on_size,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_tick      = on_tick,
        .on_resize    = on_resize,
    };
    int rc = uapp_run(&desc);
    uambient_stage_free(&g_stage);
    if (g_have_sound) usnd_shutdown();
    return rc;
}
