// Image Viewer -- a ring-3 client that shows a decoded picture.
//
// IT BROWSES A DIRECTORY RATHER THAN OPENING A FILE, and that is the
// design decision worth stating. A file-open dialog belongs to the
// application in this system (Notepad draws its own; a display server
// has no business owning one -- see notepad.c), so a picture viewer
// could have grown a second copy of that dialog. Instead the window IS
// the browser: a list of the images in one directory down the left, the
// selected one filling the rest. That is macOS Preview's sidebar and
// the Windows Photos filmstrip, it makes Up/Down arrows do the obvious
// thing, and it needs no modal at all.
//
// It is handed a directory (or a file, whose directory it lists) on the
// command line, defaulting to /usr/share/wallpapers -- which is also
// where "Set as wallpaper" points the desktop, so the two halves of
// this feature meet in one place a person can see.
//
// THE DECODE IS THIS PROCESS'S, not the kernel's and not the
// compositor's: uimg_load() runs here, and the widget is handed pixels.
// See lib/uimg.h for why an image decoder is a ring-3 library in every
// system worth copying.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "lib/uimg.h"
#include "setting_abi.h"
#include "kpath.h"   // k_path_join/_dirname -- the KERNEL's, linked into ring 3
#include <dirent.h>
#include <stdlib.h>
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "keyboard.h"

#define WIN_W 720
#define WIN_H 460
#define PATH_MAX_LEN 64          // FS_PATH_MAX
#define MAX_FILES 64
#define SIDEBAR_CHARS 16
#define WALLPAPER_DIR "/usr/share/wallpapers"
#define DEFAULT_DIR WALLPAPER_DIR

#define ID_LIST  1
#define ID_IMAGE 2

enum {
    CMD_RELOAD = 1,
    CMD_EXIT,
    CMD_FIT,
    CMD_ACTUAL,
    CMD_FILL,
    CMD_STRETCH,
    CMD_WALLPAPER_FIT,
    CMD_WALLPAPER_FILL,
    CMD_WALLPAPER_NONE,
};

static char g_dir[PATH_MAX_LEN] = DEFAULT_DIR;
static char g_names[MAX_FILES][PATH_MAX_LEN];
static const char *g_name_ptrs[MAX_FILES];
static int g_count;

static struct uimg g_img;              // the decoded picture, owned here
static int g_have_img;
static struct uui_image g_view;
static struct uui_listbox g_list;
static struct uui_menubar g_menu;
static struct uui_statusbar g_status;

static char g_stat_name[PATH_MAX_LEN];
static char g_stat_size[48];
static char g_stat_note[96];

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Reload", CMD_RELOAD, "F5"),
    UUI_MENU_SEP,
    UUI_MENU("Exit",   CMD_EXIT,   "Alt+F4"),
};

static const struct uui_menu_item view_items[] = {
    UUI_MENU("Fit to window", CMD_FIT,     0),
    UUI_MENU("Actual size",   CMD_ACTUAL,  0),
    UUI_MENU("Fill window",   CMD_FILL,    0),
    UUI_MENU("Stretch",       CMD_STRETCH, 0),
};

// Setting a wallpaper is TWO decisions -- which file, and how it is
// placed -- so the menu asks both at once rather than leaving the mode
// to a second trip. "None" is here because a plain colour is a
// legitimate choice and the only way back to it.
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

static struct uui_item g_widgets[] = {
    { &uui_listbox_ops, &g_list,  0, 0, ID_LIST },
    { &uui_image_ops,   &g_view,  0, 0, ID_IMAGE },
};

// --- loading ----------------------------------------------------------

static int menubar_h(void) {
    int h;
    uui_menubar_natural_size(&g_menu, 0, &h);
    return h;
}

static int statusbar_h(void) {
    int h;
    uui_statusbar_natural_size(&g_status, 0, &h);
    return h;
}

// Lists `g_dir`, keeping only the files a codec claims. THE PROBE
// DECIDES, not the extension: uimg_probe() reads the magic bytes, so a
// JPEG saved as .dat is listed and a text file called photo.jpg is not.
// Every real image library sniffs for the same reason -- an extension is
// a hint typed by a person.
// Name order, ascending -- the same rule dirsort's DIRSORT_NAME applies,
// so this listing and `/bin/ls` agree about what row 1 is.
//
// qsort over an array of fixed-width names, which is the idiom
// <stdlib.h> here documents: the element IS the string, so strcmp can be
// the comparator directly. (Not stable, and it does not need to be --
// two files cannot share a name.)
static int by_name(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static void sort_names(void) {
    qsort(g_names, (size_t)g_count, sizeof g_names[0], by_name);
    for (int i = 0; i < g_count; i++) g_name_ptrs[i] = g_names[i];
}

static void reload_listing(void) {
    g_count = 0;

    // opendir/readdir and fopen/fread rather than sys_listdir/sys_open:
    // tolibc is deliberately COMPLETE (docs/libc-design.md), so an
    // ordinary program should read like an ordinary C program. What
    // stays a raw syscall is nothing here.
    DIR *d = opendir(g_dir);
    if (!d) {
        uui_listbox_set_items(&g_list, g_name_ptrs, 0);
        ulogf("imgview: cannot list %s\n", g_dir);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_count < MAX_FILES) {
        char path[PATH_MAX_LEN];
        if (!k_path_join(g_dir, e->d_name, path, sizeof path)) continue;

        // THE PROBE DECIDES WHAT A FILE IS, not the extension:
        // uimg_probe() reads the magic bytes, so a JPEG saved as .dat is
        // listed and a text file called photo.jpg is not. Every real
        // image library sniffs for the same reason -- an extension is a
        // hint typed by a person. Sixteen bytes is more than any probe
        // needs and one read either way.
        uint8_t head[16];
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t got = fread(head, 1, sizeof head, f);
        fclose(f);
        if (got < 4 || !uimg_probe(head, got)) continue;

        strlcpy(g_names[g_count], e->d_name, sizeof g_names[0]);
        g_name_ptrs[g_count] = g_names[g_count];
        g_count++;
    }
    closedir(d);

    // SORTED HERE, not by readdir: SYS_LISTDIR returns whatever order
    // the filesystem walks, and dirsort is the one ordering /bin/ls and
    // every app's file list share (lib/dirsort.h says why two of them
    // drifting is a bug nobody looks for). It sorts dirents, so this
    // sorts the names it kept -- same comparison, applied to the
    // filtered list.
    sort_names();
    uui_listbox_set_items(&g_list, g_name_ptrs, g_count);
    ulogf("imgview: listing %s -- %d image(s)\n", g_dir, g_count);
}

static void show(int index) {
    if (index < 0 || index >= g_count) return;
    g_list.selected = index;

    char path[PATH_MAX_LEN];
    if (!k_path_join(g_dir, g_names[index], path, sizeof path)) return;

    struct uimg_info info;
    int rc = uimg_load_info(path, &info);
    if (rc == 0) {
        unsigned long long t0 = sys_monotonic_ns();
        struct uimg fresh;
        rc = uimg_load(path, &fresh);
        unsigned long long ms = (sys_monotonic_ns() - t0) / 1000000ull;
        if (rc == 0) {
            // The widget is pointed at the NEW image before the old one
            // is freed: pointing it at freed pixels, even for the length
            // of one statement, is a use-after-free the first repaint
            // would find.
            struct uimg old = g_img;
            g_img = fresh;
            g_have_img = 1;
            uui_image_set(&g_view, &g_img);
            uimg_free(&old);
            snprintf(g_stat_size, sizeof g_stat_size, "%dx%d %s", info.w, info.h,
                     info.detail);
            snprintf(g_stat_note, sizeof g_stat_note, "decoded in %llu ms", ms);
            ulogf("imgview: shown %s %dx%d in %llu ms\n", g_names[index],
                  info.w, info.h, ms);
        }
    }
    if (rc < 0) {
        // A REFUSAL IS NOT A CORRUPTION, and the status bar says which:
        // uimg_last_error() carries the decoder's own sentence.
        if (g_have_img) {
            uimg_free(&g_img);
            g_have_img = 0;
            uui_image_set(&g_view, NULL);
        }
        strlcpy(g_stat_size, "not shown", sizeof g_stat_size);
        strlcpy(g_stat_note, uimg_last_error(), sizeof g_stat_note);
        ulogf("imgview: refused %s -- %s\n", g_names[index], uimg_last_error());
    }
    strlcpy(g_stat_name, g_names[index], sizeof g_stat_name);
}

// "Set as wallpaper" -- through the SETTINGS REGISTRY, not by writing
// /etc/desktop.conf directly.
//
// `wallpaper` and `wallpaper_mode` are registered settings
// (kernel/lib/wallpaper_config.c), so this is the same path `config set`
// and System Settings take: the write is the registry's, and the
// generation counter moves -- which is how the desktop, a different
// process, finds out. (The registry does not validate an enum value
// against its choice list; an unknown name is stored and the desktop
// falls back to its plain colour, logging why.)
//
// THE VALUE IS A NAME AND THE PICTURE HAS TO LIVE IN
// /usr/share/wallpapers, because that is what a name means here (the
// same rule as a font face or a cursor theme). A file elsewhere is
// refused with a sentence rather than silently doing nothing -- copy it
// in and it becomes selectable everywhere at once, including in
// System Settings.
static int set_setting(const char *name, const char *value) {
    struct setting_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.op = SETTING_OP_SET;
    strlcpy(msg.name, name, sizeof msg.name);
    strlcpy(msg.value, value, sizeof msg.value);
    if (sys_setting(&msg) != 0) return 0;
    // SETTING_UNSAVED means live but not persisted, which a caller must
    // not report as success (setting_abi.h says so in as many words).
    return msg.result == SETTING_SAVED;
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
    if (g_list.selected < 0 || g_list.selected >= g_count) return;

    if (strcmp(g_dir, WALLPAPER_DIR) != 0) {
        strlcpy(g_stat_note, "only files in /usr/share/wallpapers", sizeof g_stat_note);
        ulogf("imgview: wallpaper refused -- %s is not %s\n", g_dir, WALLPAPER_DIR);
        uapp_redraw(a);
        return;
    }

    // The setting's value is the filename without its extension.
    char stem[PATH_MAX_LEN];
    strlcpy(stem, g_names[g_list.selected], sizeof stem);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    int ok = set_setting("desktop.wallpaper", stem);
    ok = set_setting("desktop.wallpaper_mode", mode) && ok;
    snprintf(g_stat_note, sizeof g_stat_note, "%s wallpaper: %s, %s",
             ok ? "set" : "could not set", stem, mode);
    ulogf("imgview: wallpaper %s mode %s -- %s\n", stem, mode, ok ? "ok" : "FAILED");
    uapp_redraw(a);
}

// --- layout and drawing ------------------------------------------------

static void layout_all(int cw, int ch) {
    int mb = menubar_h(), sb = statusbar_h();
    int side = ugfx_char_w() * SIDEBAR_CHARS;
    if (side > cw / 2) side = cw / 2;

    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_statusbar_set_geometry(&g_status, 0, ch - sb, cw, sb);

    g_list.x = 0;
    g_list.y = mb;
    g_list.w = side;
    g_list.h = ch - mb - sb;

    g_view.x = side;
    g_view.y = mb;
    g_view.w = cw - side;
    g_view.h = ch - mb - sb;
}

// --- self-reported layout ---------------------------------------------
//
// docs/gui-guidelines.md: a GUI test asks the app where things are. The
// menu's rectangles in particular cannot be re-derived in Python -- they
// depend on which submenu is open and on which way the placement
// flipped -- so they come from the same accessors the widget draws with.
// The grammar is notepad.c's, deliberately: two apps reporting layout
// two different ways would mean two parsers in tools/.
//
// `layout image` is emitted FIRST on every draw, so a parser has a frame
// boundary and cannot report a popup that closed three frames ago as
// still open (the trap tools/menubar_test.py documents).
static void log_layout(void) {
    int x, y, w, h;

    ulogf("imgview: layout image %d %d %d %d\n", g_view.x, g_view.y, g_view.w, g_view.h);
    // Where the PICTURE landed inside that box, which is not the same
    // rect: a letterboxed image is smaller than its widget, and a test
    // sampling the widget would be sampling the bars.
    if (uui_image_drawn_rect(&g_view, &x, &y, &w, &h))
        ulogf("imgview: layout picture %d %d %d %d\n", x, y, w, h);
    ulogf("imgview: layout list %d %d %d %d\n", g_list.x, g_list.y, g_list.w, g_list.h);
    ulogf("imgview: layout menubar %d %d %d %d\n", g_menu.x, g_menu.y, g_menu.w, g_menu.h);
    for (int i = 0; i < (int)(sizeof menu_items / sizeof menu_items[0]); i++) {
        if (!uui_menubar_title_rect(&g_menu, i, &x, &y, &w, &h)) continue;
        ulogf("imgview: layout title %d %d %d %d %d\n", i, x, y, w, h);
    }
    for (int l = 0; l < uui_menubar_depth(&g_menu); l++) {
        if (uui_menubar_popup_rect(&g_menu, l, &x, &y, &w, &h))
            ulogf("imgview: layout popup %d %d %d %d %d\n", l, x, y, w, h);
        for (int i = 0; uui_menubar_item_rect(&g_menu, l, i, &x, &y, &w, &h); i++)
            ulogf("imgview: layout item %d %d %d %d %d %d\n", l, i, x, y, w, h);
    }
    ulogf("imgview: layout selected %d\n", g_list.selected);
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

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_RELOAD:
        reload_listing();
        show(g_list.selected < 0 ? 0 : g_list.selected);
        break;
    case CMD_EXIT:    uapp_quit(a, 0); return;
    case CMD_FIT:     uui_image_set_fit(&g_view, UIMG_FIT_CONTAIN); break;
    case CMD_ACTUAL:  uui_image_set_fit(&g_view, UIMG_FIT_NONE);    break;
    case CMD_FILL:    uui_image_set_fit(&g_view, UIMG_FIT_COVER);   break;
    case CMD_STRETCH: uui_image_set_fit(&g_view, UIMG_FIT_STRETCH); break;
    case CMD_WALLPAPER_FILL: set_wallpaper(a, "fill"); return;
    case CMD_WALLPAPER_FIT:  set_wallpaper(a, "fit");  return;
    case CMD_WALLPAPER_NONE: set_wallpaper(a, NULL);   return;
    default: return;
    }
    uapp_redraw(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    if (id == ID_LIST && reason == UUI_REASON_RELEASE) {
        // Commit on RELEASE, as every control here does
        // (docs/gui-guidelines.md): a press that lands on the wrong row
        // and is dragged off must not have decoded a file.
        show(g_list.selected);
        uapp_redraw(a);
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
    int next = g_list.selected;
    if (key == KEY_ARROW_DOWN || key == KEY_ARROW_RIGHT) next++;
    else if (key == KEY_ARROW_UP || key == KEY_ARROW_LEFT) next--;
    else if (key == KEY_HOME) next = 0;
    else if (key == KEY_END) next = g_count - 1;
    else if (key == 0x9B) { do_command(a, CMD_RELOAD); return; }  // F3 -- there is no F5 code
    else return;

    if (next < 0) next = 0;
    if (next > g_count - 1) next = g_count - 1;
    if (next != g_list.selected) {
        show(next);
        uui_listbox_key(&g_list, key);   // keeps the scroll following the selection
        uapp_redraw(a);
    }
}

static void on_open(struct uapp *a) {
    layout_all(uapp_width(a), uapp_height(a));
    reload_listing();
    if (g_count > 0) show(0);
    (void)a;
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    layout_all(w, h);
}

int main(int argc, char **argv) {
    // An argument may be a directory to browse or a file to show. A file
    // is the more useful default for "open this with the viewer", and
    // costs one stat.
    if (argc > 1 && argv[1][0]) {
        struct sys_stat st;
        if (sys_stat(argv[1], &st) == 0 && !st.is_dir) {
            // A file: browse the directory it is in. k_path_dirname() is
            // the kernel's, KTESTed, and linked into ring 3 -- walking
            // back over the string by hand here would be the fourth copy
            // of that loop in this tree.
            if (!k_path_dirname(argv[1], g_dir, sizeof g_dir))
                strlcpy(g_dir, DEFAULT_DIR, sizeof g_dir);
        } else {
            strlcpy(g_dir, argv[1], sizeof g_dir);
        }
    }

    uui_menubar_init(&g_menu, menu_items,
                     (int)(sizeof menu_items / sizeof menu_items[0]));
    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_stat_name;
    g_status.panes[0].chars = 18;
    g_status.panes[1].text = g_stat_size;
    g_status.panes[1].chars = 0;
    g_status.panes[2].text = g_stat_note;
    g_status.panes[2].chars = 24;
    g_status.count = 3;

    uui_listbox_init(&g_list, 0, 0, 100, 100, g_name_ptrs, 0);
    uui_image_init(&g_view, NULL, UIMG_FIT_CONTAIN);
    // Without these a 4000px photograph would ask for a 4000px window
    // and uui_layout would hand it one (it overflows rather than
    // shrinking -- CLAUDE.md). The viewer wants a sane window and a
    // scaled picture inside it.
    g_view.max_w = WIN_W;
    g_view.max_h = WIN_H;

    strlcpy(g_stat_name, "no image", sizeof g_stat_name);

    struct uapp_desc desc = {
        .title        = "Image Viewer",
        .app_id       = "imgview",
        .w            = WIN_W,
        .h            = WIN_H,
        .flags        = UAPP_RESIZABLE,
        .min_w        = 320,
        .min_h        = 200,
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
        .on_resize    = on_resize,
    };
    int rc = uapp_run(&desc);
    uui_image_release(&g_view);
    if (g_have_img) uimg_free(&g_img);
    return rc;
}
