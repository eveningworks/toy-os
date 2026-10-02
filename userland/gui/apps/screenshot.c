// Screenshot -- capture the screen, a window, or a dragged region.
//
// The second front end on lib/ushot.h; /bin/screenshot is the other, and
// neither wraps the other. What this adds over the command is the two
// things a command cannot have: a preview of what you just took, and a
// region you drag rather than type as four numbers.
//
// **REGION SELECT WORKS ON A FROZEN CAPTURE, NOT ON THE LIVE SCREEN.**
// A full-screen shot is taken first, the window goes fullscreen showing
// that picture, the band is dragged over it, and the crop happens in
// memory. Spectacle and GNOME's shell both do exactly this, and the
// reason is that the alternative -- an overlay over the live screen --
// means the thing being photographed can move while you are choosing
// what to photograph.
//
// **THE APP IS NEVER IN ITS OWN PICTURE**: every capture asks
// WIN_SHOT_NO_SELF, so the compositor renders one frame without this
// client's windows. The window visibly goes away and comes back, which
// is what a screenshot tool is supposed to look like.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_button.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_image.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_radio_list.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_focus.h"
#include "lib/uclip.h"
#include "lib/ushot.h"
#include "kpath.h"   // k_path_basename -- the kernel's, linked into ring 3
#include "rubberband.h"

#define SHOT_DIR "/home/screenshots"

enum {
    ID_MODE = 1, ID_DELAY, ID_POINTER, ID_TAKE, ID_SAVE, ID_COPY, ID_PREVIEW,
};

enum { MODE_SCREEN, MODE_WINDOW, MODE_REGION };

static const char *const MODE_LABELS[] = {
    "Whole screen", "Active window", "Select a region",
};

static struct uui_radio_list g_mode;
static struct uui_spinbox    g_delay;
static struct uui_checkbox   g_pointer;
static struct uui_button     g_take, g_save, g_copy;
static struct uui_image      g_preview;
static struct uui_label      g_status_label, g_mode_label, g_delay_label;
static struct uui_filedialog g_chooser;

static struct uui_item   g_form_items[2], g_opt_items[3], g_btn_items[3], g_root_items[5];
static struct uui_layout g_form, g_opts, g_btns, g_root;

// THE TAB ORDER, and it is not optional: the radio list, the spinbox and
// the checkbox all declare a `key` op, and a widget that takes keys gets
// none until the app routes them (docs/conventions/gui.md). Reading order.
static struct uui_focusable g_focusables[] = {
    { &g_mode,    &uui_radio_list_ops },
    { &g_delay,   &uui_spinbox_ops },
    { &g_pointer, &uui_checkbox_ops },
    { &g_take,    &uui_button_ops },
    { &g_save,    &uui_button_ops },
    { &g_copy,    &uui_button_ops },
};
static struct uui_focus g_focus;

static char g_status[128] = "Ready.";
static char g_last_path[UUI_FILEDIALOG_PATH_MAX];

static struct ushot g_shot;
static int g_shot_ok;      // g_shot holds a capture
static int g_open;         // the compositor answered ushot_open()

// --- the overlay -------------------------------------------------------
//
// Both "Select a region" and "Active window" work the same way: a
// full-screen capture is taken FIRST, the window goes fullscreen showing
// that frozen picture, and the choice is made over it. While an overlay
// is up the root layout's child count is ZERO and the router has no
// widgets, so the toolkit draws and routes nothing and this file's
// on_draw/on_press have the whole window.
//
// Choosing over a FROZEN picture rather than the live screen is what
// Spectacle and GNOME's shell both do, and the reason is that the thing
// being photographed must not move while you are deciding what to
// photograph.
enum { OVERLAY_NONE, OVERLAY_REGION, OVERLAY_PICK };
static int g_overlay;

static struct rubberband g_band;
static int g_band_dragging;

// The window the pointer is over, as the compositor answers it
// (WIN_SHOT_WINDOW_AT + WIN_SHOT_PROBE). `g_pick_have` separates "no
// window there" from "a window of zero size", which cannot happen but
// would be indistinguishable.
static struct win_shot g_pick;
static int g_pick_have;

// ARMED BY A PRESS INSIDE THE OVERLAY, and nothing else commits.
//
// The release that COMMITS the Take button also reaches on_release --
// uapp routes to the widgets first and then calls the app's own handler
// (ui/uapp.h) -- so by the time it arrives the picker is already up, and
// without this it ended the picker on the very click that opened it.
// It is also the arm-on-press/commit-on-release contract every control
// here follows (docs/gui-guidelines.md): press on the window you want,
// release on it.
static int g_pick_armed;

static void status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
    // No set_text: uui_label BORROWS its string (ui/uui_label.h), so the
    // one at init points here for good and rewriting the buffer is the
    // whole update.
}

// FILE-SCOPE, NOT A LOCAL: uui_image BORROWS the struct it is given and
// reads it again at every redraw (ui/uui_image.h). A local one dangles
// the moment this returns, and the widget then scales from a stack
// address -- which faulted on the first repaint after a capture.
static struct uimg g_preview_img;

// THE MARKER FOR "THIS IS WHAT YOU WILL GET", used by both overlays.
//
// Two rings, accent inside a dark outer line, because ONE ring is
// invisible against a window whose own chrome is that colour -- which on
// this theme is every focused title bar. The same reason
// ugfx_draw_string_shadowed() exists.
static void outline(struct ugfx_surface *s, int x, int y, int w, int h) {
    uint32_t ink = UTHEME_ACCENT;
    ugfx_draw_rect(s, x - 2, y - 2, w + 4, h + 4, UTHEME_TEXT);
    ugfx_draw_rect(s, x - 1, y - 1, w + 2, h + 2, ink);
    ugfx_draw_rect(s, x, y, w, h, ink);
    ugfx_draw_rect(s, x + 1, y + 1, w - 2, h - 2, UTHEME_TEXT);

    char size[32];
    snprintf(size, sizeof size, "%d x %d", w, h);
    // Inside the box when there is room above it, below the top edge
    // otherwise -- a label drawn off the top of the screen is a label
    // nobody sees, and the topmost window is exactly the common case.
    int ly = y >= ugfx_char_h() + 6 ? y - ugfx_char_h() - 4 : y + 4;
    ugfx_draw_string_shadowed(s, x + 4, ly, size, UTHEME_ACCENT_TEXT);
}

static void show_preview(void) {
    g_preview_img.w = g_shot.w;
    g_preview_img.h = g_shot.h;
    g_preview_img.px = g_shot.px;
    g_preview_img.has_alpha = 0;
    uui_image_set(&g_preview, &g_preview_img);
}

// /home/screenshots/shot-YYYYMMDD-HHMMSS.qoi. QOI rather than PNG
// because this desktop can open a QOI and cannot open a PNG (lib/uimg.h)
// -- a picture the system that made it cannot show is a strange default.
// Save As... offers both.
static void auto_save(void) {
    sys_mkdir(SHOT_DIR);
    time_t now = time(NULL);
    struct tm tm;
    char stamp[32];
    if (localtime_r(&now, &tm) && strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm))
        snprintf(g_last_path, sizeof g_last_path, SHOT_DIR "/shot-%s.qoi", stamp);
    else
        snprintf(g_last_path, sizeof g_last_path, SHOT_DIR "/shot-%d.qoi", sys_getpid());

    int rc = ushot_save(&g_shot, g_last_path, NULL);
    if (rc < 0) {
        status("Could not save: %s", ushot_strerror(rc));
        g_last_path[0] = '\0';
    } else {
        status("%dx%d saved to %s", g_shot.w, g_shot.h, g_last_path);
    }
}

// --- taking -----------------------------------------------------------

static void enter_overlay(struct uapp *a, int kind) {
    g_overlay = kind;
    g_band_dragging = 0;
    g_pick_have = 0;
    g_pick_armed = 0;
    rb_clear(&g_band);
    // BOTH HALVES. Emptying the layout stops it drawing; the ROUTER
    // draws and hit-tests `uapp_desc.widgets` independently, so a layout
    // with no children still left every control on screen (ui/uapp.h).
    g_root.count = 0;
    uapp_set_widgets(a, 0, 0);
    uapp_set_fullscreen(a, 1);
    uapp_redraw(a);
}

static void leave_overlay(struct uapp *a) {
    g_overlay = OVERLAY_NONE;
    g_band_dragging = 0;
    g_pick_have = 0;
    g_pick_armed = 0;
    g_root.count = 5;
    uapp_set_widgets(a, g_root_items, 5);
    uapp_set_fullscreen(a, 0);
    uapp_redraw(a);
}

static void take(struct uapp *a) {
    if (!g_open) {
        status("No desktop is running.");
        return;
    }
    int delay = uui_spinbox_value(&g_delay);
    if (delay > 0) {
        // A BLOCKING SLEEP IN A GUI CLIENT IS NORMALLY WRONG -- the
        // compositor pings, and a client that does not answer is drawn
        // as Not Responding. This one is deliberate and bounded: the
        // delay exists so the person can arrange the screen, and an app
        // that kept repainting itself while they did would be one more
        // thing moving in the picture. See docs/conventions/gui.md on
        // long work belonging in a child process -- this is not long
        // work, it is an intentional pause.
        uapp_flush(a);
        sleep((unsigned)delay);
    }

    unsigned flags = WIN_SHOT_NO_SELF;
    if (g_pointer.checked) flags |= WIN_SHOT_POINTER;

    // EVERY MODE STARTS WITH A FULL-SCREEN CAPTURE. For "Whole screen"
    // that is the answer; for the other two it is the frozen picture the
    // choice is made over, and the crop happens in memory afterwards --
    // so a window is captured as it was when you pressed the button, not
    // as it is when you finally click it.
    int rc = ushot_take(&g_shot, WIN_SHOT_SCREEN, flags, 0, 0, 0, 0);
    if (rc < 0) {
        g_shot_ok = 0;
        status("%s", ushot_strerror(rc));
        uapp_redraw(a);
        return;
    }
    g_shot_ok = 1;

    if (g_mode.selected == MODE_REGION) {
        status("Drag a rectangle. Esc or the secondary button cancels.");
        enter_overlay(a, OVERLAY_REGION);
        return;
    }
    if (g_mode.selected == MODE_WINDOW) {
        status("Point at a window and click it. Esc cancels.");
        enter_overlay(a, OVERLAY_PICK);
        return;
    }
    show_preview();
    auto_save();
    uapp_redraw(a);
}

// --- the chooser ------------------------------------------------------

static void on_chosen(void *ctx, const char *path) {
    struct uapp *a = ctx;
    if (!path) return;
    int rc = ushot_save(&g_shot, path, NULL);
    if (rc < 0) status("Could not save: %s", ushot_strerror(rc));
    else {
        snprintf(g_last_path, sizeof g_last_path, "%s", path);
        status("Saved to %s", path);
    }
    uapp_redraw(a);
}

static int keep_images(void *ctx, const char *dir, const struct sys_dirent *e) {
    (void)ctx; (void)dir;
    if (e->is_dir) return 1;
    const char *dot = strrchr(e->name, '.');
    return dot && (strcmp(dot, ".qoi") == 0 || strcmp(dot, ".png") == 0);
}

static const struct uui_filedialog_filter FILTERS[] = {
    { "Images (.qoi .png)", keep_images, 0 },
    UUI_FILEDIALOG_ALL_FILES,
};

// --- callbacks --------------------------------------------------------

static void on_action(struct uapp *a, int code) {
    switch (code) {
    case ID_TAKE:
        take(a);
        break;
    case ID_SAVE: {
        if (!g_shot_ok) { status("Take a screenshot first."); uapp_redraw(a); break; }
        if (uui_filedialog_is_open(&g_chooser)) break;
        // k_path_basename() never fails and never returns NULL, so the
        // empty answer -- no path yet, or a trailing slash -- is what
        // selects the default name.
        const char *name = k_path_basename(g_last_path);
        struct uui_filedialog_opts o = {
            .mode = UUI_FILEDIALOG_SAVE,
            .title = "Save Screenshot",
            .start_dir = SHOT_DIR,
            .initial_name = name[0] ? name : "screenshot.qoi",
            .filters = FILTERS,
            .filter_count = 2,
        };
        uui_filedialog_open(a, &g_chooser, &o, on_chosen, a);
        break;
    }
    case ID_COPY:
        // THE FILE, NOT THE PIXELS. This clipboard carries files or
        // text (lib/uclip.h), so what a paste in the File Manager gets
        // is the saved image -- which is why Copy needs a saved file
        // and says so rather than appearing to do nothing.
        if (!g_last_path[0]) { status("Nothing saved yet to copy."); }
        else {
            uclip_begin(0, UCLIP_COPY);
            if (uclip_add(0, g_last_path) && uclip_commit(0) > 0)
                status("Copied %s to the clipboard.", g_last_path);
            else
                status("The clipboard refused it.");
        }
        uapp_redraw(a);
        break;
    default:
        break;
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    // WHERE EVERY CONTROL IS, by name, so a test never derives geometry
    // the app already knows (docs/conventions/gui.md). Gated off by
    // default, so this costs nothing.
    uapp_log_layout(a, "screenshot");
    if (g_overlay)
        uapp_logf_layout("screenshot: overlay %s %d %d %d %d",
                          g_overlay == OVERLAY_PICK ? "pick" : "region",
                          g_pick_have ? g_pick.x : g_shot.x,
                          g_pick_have ? g_pick.y : g_shot.y,
                          g_pick_have ? g_pick.w : g_shot.w,
                          g_pick_have ? g_pick.h : g_shot.h);
    if (!g_overlay) return;
    struct ugfx_surface *s = uapp_surface(d);

    // The frozen capture, 1:1 -- a fullscreen window's content area IS
    // the screen, so the shot's own coordinates are the window's.
    ugfx_blit(s, 0, 0, g_shot.w, g_shot.h, g_shot.px, g_shot.w);

    int x = 0, y = 0, w = 0, h = 0, have = 0;
    if (g_overlay == OVERLAY_REGION)
        have = rb_rect(&g_band, &x, &y, &w, &h) && w > 0 && h > 0;
    else if (g_pick_have) {
        x = g_pick.x; y = g_pick.y; w = g_pick.w; h = g_pick.h;
        have = w > 0 && h > 0;
    }
    if (have) outline(s, x, y, w, h);
}

// `buttons` IS TWO FIELDS, not a button mask: the low byte is the mask
// and the next one is the KEY_MOD_* bits held (abi/win_proto.h). Reading
// it raw makes every test here answer whatever the modifiers happen to
// be -- which showed up as a band that could be started and never
// dragged, with no error anywhere.
static void on_press(struct uapp *a, int x, int y, unsigned mods) {
    if (!g_overlay) return;
    unsigned buttons = WIN_MOUSE_BUTTONS(mods);
    if (buttons & 2) { leave_overlay(a); status("Cancelled."); return; }
    if (g_overlay == OVERLAY_PICK) { g_pick_armed = 1; return; }
    if (g_overlay != OVERLAY_REGION) return;
    rb_begin(&g_band, x, y, RB_REPLACE);
    g_band_dragging = 1;
    uapp_redraw(a);
}

static void on_motion(struct uapp *a, int x, int y, unsigned mods) {
    if (g_overlay == OVERLAY_PICK) {
        // A LEAVE CARRIES NO POSITION, so it must not be probed -- the
        // compositor would answer about (-1, -1).
        if (x < 0 || y < 0) return;
        struct win_shot r;
        int ok = ushot_probe(&g_shot, WIN_SHOT_WINDOW_AT, x, y, &r) == 0;
        // REDRAWN ONLY WHEN THE TARGET CHANGES. A repaint here is a
        // full-screen blit, and one per pointer move over the same
        // window is a megabyte of memcpy for an identical picture.
        if (ok != g_pick_have ||
            (ok && (r.x != g_pick.x || r.y != g_pick.y ||
                    r.w != g_pick.w || r.h != g_pick.h))) {
            g_pick = r;
            g_pick_have = ok;
            uapp_redraw(a);
        }
        return;
    }
    if (g_overlay != OVERLAY_REGION || !g_band_dragging) return;
    // A DRAG NEEDS THE BUTTON STILL DOWN, and the pointer grab is not
    // that fact (docs/conventions/gui.md): a motion with nothing held is
    // a release this app never saw.
    // IGNORED, NOT TREATED AS A RELEASE (docs/conventions/gui.md). A
    // pointer LEAVE arrives here as a motion to (-1, -1) with no buttons
    // -- "a leave carries no position" -- and ending the band on it made
    // the drag impossible: the band began, the leave cancelled it, and
    // the release found nothing in flight.
    if (x < 0 || y < 0) return;
    if (!(WIN_MOUSE_BUTTONS(mods) & 1)) return;
    rb_motion(&g_band, x, y, 0, 0);
    uapp_redraw(a);
}

static void on_release(struct uapp *a, int x, int y, unsigned mods) {
    (void)x; (void)y; (void)mods;

    // PICKING COMMITS ON THE RELEASE, like every other control here
    // (docs/gui-guidelines.md): a press that wandered onto a different
    // window before letting go takes the one it ended on, which is what
    // the outline was showing all along.
    if (g_overlay == OVERLAY_PICK) {
        if (!g_pick_armed) return;   // the click that OPENED the picker
        if (!g_pick_have) { leave_overlay(a); status("No window there."); return; }
        struct win_shot p = g_pick;
        leave_overlay(a);
        if (ushot_crop(&g_shot, p.x, p.y, p.w, p.h) < 0) {
            status("That window is off the screen.");
        } else {
            show_preview();
            auto_save();
        }
        uapp_redraw(a);
        return;
    }

    if (g_overlay != OVERLAY_REGION || !g_band_dragging) return;
    g_band_dragging = 0;

    // THE RECT COMES OUT BEFORE rb_end(), which retires the band: asking
    // afterwards returns 0 and touches none of the outputs, so the crop
    // ran on four uninitialised numbers (api/rubberband.h).
    int rx, ry, rw, rh;
    int have = rb_rect(&g_band, &rx, &ry, &rw, &rh);
    int was_band = rb_end(&g_band);
    if (!have || !was_band || rw < 2 || rh < 2) {
        leave_overlay(a);
        status("That region was too small.");
        uapp_redraw(a);
        return;
    }
    leave_overlay(a);
    if (ushot_crop(&g_shot, rx, ry, rw, rh) < 0) {
        status("That region was outside the screen.");
    } else {
        show_preview();
        auto_save();
    }
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    // Esc cancels the BAND, which is not the same as closing a window --
    // the rule Esc is barred from (docs/gui-guidelines.md) is closing.
    if (g_overlay && key == 27) {
        leave_overlay(a);
        status("Cancelled.");
        uapp_redraw(a);
    }
}

// **FONT-DERIVED, AND IT OUTRANKS THE LAYOUT'S NATURAL SIZE.** Left to
// the layout the window is exactly as tall as the controls, and the
// preview -- whose natural height is zero until it holds a picture --
// gets a one-pixel strip. The extra rows are the preview's; it is the
// only UUI_FILL_H child, so the whole surplus lands there.
static void on_size(int *w, int *h) {
    *w = ugfx_char_advance('n') * 46;
    *h = ugfx_char_h() * 30;
}

static void on_open(struct uapp *a) {
    int rc = ushot_open_on(&g_shot, uapp_wmchan());
    g_open = rc == 0;
    if (!g_open) status("Cannot capture: %s", ushot_strerror(rc));
    uapp_redraw(a);
}

static int on_close(struct uapp *a) {
    (void)a;
    if (g_open) ushot_close(&g_shot);
    uui_image_release(&g_preview);
    return 1;
}

int main(void) {
    // A radio list has no init(): its fields ARE the setup, the same way
    // System Settings builds one.
    g_mode.options  = MODE_LABELS;
    g_mode.count    = 3;
    g_mode.cols     = 1;
    g_mode.selected = MODE_SCREEN;
    // -1, NOT the zero a file-scope struct starts at: 0 is a valid row,
    // so a zeroed `hovered` leaves the first one drawn hovered forever
    // (System Settings sets this for the same reason).
    g_mode.hovered  = -1;
    g_mode.bg       = UUI_COLOR_UNSET;
    g_mode.fg       = UUI_COLOR_UNSET;
    uui_spinbox_init(&g_delay, 0, 0, 30, 1, "s");
    // UUI_COLOR_UNSET, not a colour picked here: a widget resolves its
    // own against the theme when it DRAWS (docs/conventions/gui.md), so
    // this one follows the panel a repaint actually uses.
    uui_checkbox_init(&g_pointer, 0, 0, 0, "Include the pointer",
                      UUI_COLOR_UNSET, UUI_COLOR_UNSET);
    uui_button_init(&g_take, 0, 0, 0, 0, "Take Screenshot",
                    UTHEME_ACCENT, UTHEME_ACCENT_TEXT, ID_TAKE);
    uui_button_init(&g_save, 0, 0, 0, 0, "Save As...",
                    UTHEME_BUTTON_BG, UTHEME_TEXT, ID_SAVE);
    uui_button_init(&g_copy, 0, 0, 0, 0, "Copy",
                    UTHEME_BUTTON_BG, UTHEME_TEXT, ID_COPY);
    uui_image_init(&g_preview, 0, UIMG_FIT_CONTAIN);
    // A CEILING ON WHAT IT MAY ASK FOR. Without these its natural size
    // is the picture's own, so a screen-sized capture would demand a
    // screen-sized window and uui_layout would overflow rather than talk
    // it down (ui/uui_image.h).
    g_preview.max_w = 320;
    g_preview.max_h = 180;
    uui_label_init(&g_mode_label, "Capture");
    uui_label_init(&g_delay_label, "Delay");
    uui_label_init(&g_status_label, g_status);

    g_form_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_delay_label,
                                          .name = "delaylabel" };
    g_form_items[1] = (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &g_delay,
                                          .id = ID_DELAY, .name = "delay" };
    g_form.dir = UUI_ROW;
    g_form.margin = 0;
    g_form.items = g_form_items;
    g_form.count = 2;

    g_opt_items[0] = (struct uui_item){ .ops = &uui_radio_list_ops, .widget = &g_mode,
                                         .id = ID_MODE, .name = "mode" };
    g_opt_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_form,
                                         .name = "delayrow" };
    g_opt_items[2] = (struct uui_item){ .ops = &uui_checkbox_ops, .widget = &g_pointer,
                                         .id = ID_POINTER, .name = "pointer" };
    g_opts.dir = UUI_COLUMN;
    g_opts.margin = 0;
    g_opts.items = g_opt_items;
    g_opts.count = 3;

    g_btn_items[0] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_take,
                                         .id = ID_TAKE, .name = "take" };
    g_btn_items[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_save,
                                         .id = ID_SAVE, .name = "saveas" };
    g_btn_items[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_copy,
                                         .id = ID_COPY, .name = "copy" };
    g_btns.dir = UUI_ROW;
    g_btns.margin = 0;
    g_btns.items = g_btn_items;
    g_btns.count = 3;

    g_root_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_mode_label,
                                          .flags = UUI_FILL_W, .name = "modelabel" };
    g_root_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_opts,
                                          .flags = UUI_FILL_W, .name = "options" };
    // THE PREVIEW ABSORBS THE SPARE ROOM, which is what stops the
    // buttons and the status line being pushed off a window that is
    // taller than the controls need (docs/conventions/gui.md).
    g_root_items[2] = (struct uui_item){ .ops = &uui_image_ops, .widget = &g_preview,
                                          .id = ID_PREVIEW,
                                          .flags = UUI_FILL_W | UUI_FILL_H,
                                          .name = "preview" };
    g_root_items[3] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_btns,
                                          .flags = UUI_FILL_W, .name = "buttons" };
    g_root_items[4] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_status_label,
                                          .flags = UUI_FILL_W, .name = "status" };
    g_root.dir = UUI_COLUMN;
    g_root.items = g_root_items;
    g_root.count = 5;

    uui_focus_init(&g_focus, g_focusables,
                   (int)(sizeof g_focusables / sizeof g_focusables[0]));

    struct uapp_desc desc = {
        .title        = "Screenshot",
        .app_id       = "screenshot",
        .on_size      = on_size,
        .flags        = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .min_w        = 320,
        .min_h        = 300,
        .layout       = &g_root,
        // The same array twice: `layout` sizes and draws, `widgets` gets
        // input (ui/uapp.h).
        .widgets      = g_root_items,
        .widget_count = 5,
        .focus        = &g_focus,
        .on_open      = on_open,
        .on_action    = on_action,
        .on_draw      = on_draw,
        .on_press     = on_press,
        .on_motion    = on_motion,
        .on_release   = on_release,
        .on_key       = on_key,
        .on_close     = on_close,
    };
    return uapp_run(&desc);
}
