#ifndef GUI_APPS_H
#define GUI_APPS_H

#include <stdint.h> // uint32_t, for on_read_complete's byte count below

struct window; // full definition in wm.h

// A GUI app is launched into its own window by the window manager (see
// wm.h). Unlike apps.h apps (which take over the whole screen and run
// their own blocking loop), GUI apps are event-driven: the window
// manager keeps control of the main loop at all times and calls these
// callbacks in response to input. By default, only one window per app is
// supported -- opening an already-open app from the Start menu just
// focuses/restores its existing window rather than creating a second one.
// Set `multi_instance` (below) to allow more than one window of the same
// app at once -- see its own comment for what that actually requires
// from an app's on_open/on_close.
//
// See apps/notepad.c for the simplest possible example, or
// apps/GUI_APPS.md for a full walkthrough of adding a new one.
struct gui_app {
    const char *name; // shown in the Start menu and the title bar

    // The entry's Icon= key, read TWO ways.
    //
    // `icon_name` is a name in /usr/share/icons -- "notepad" for
    // notepad.qoi -- which is freedesktop's rule (an Icon= is a name
    // looked up in a theme, not a path) and the same rule a font face
    // and a cursor theme already follow here.
    //
    // `icon` is the single-character fallback drawn in a tile when there
    // is no such file. A one-character Icon= means the glyph and nothing
    // else; a longer one is a name, with the first character still
    // standing in if the file is missing. That is what lets an entry
    // predate its artwork -- and Crash Test ships with no icon file on
    // purpose, so the fallback path is exercised on every boot rather
    // than merely written.
    const char *icon_name;   // "" when the entry gave a bare character
    char icon;

    // Which app_id a WINDOW of this entry reports, so the taskbar can
    // find the icon for a window it did not launch. Defaults to the
    // basename of Exec, which is right for every app here but one --
    // Shapes runs /bin/wm/demos/shapes and calls itself "gfxdemo".
    //
    // This is freedesktop's StartupWMClass, which exists for exactly
    // this mismatch: the window's own identity and the launcher's file
    // name are two different things, and only the entry can say they
    // are the same app.
    const char *app_id;

    // Computes this app's initial content-area size in pixels, from
    // whatever font size is currently active (gfx_char_w()/gfx_char_h()
    // -- see gfx_set_font_size()). Called once, at window-open time, so
    // a window is always sized for the font it's opened under rather
    // than a fixed size that's either cramped at large fonts or full of
    // wasted space at small ones. Font size can only change from the
    // shell before `gui` runs (there's no live in-GUI font picker), so
    // this is the only place size needs to be computed -- no live
    // resize-on-fontsize-change plumbing needed.
    void (*default_size)(int *w, int *h);

    // Called once EVERY TIME a window of this app is opened -- for a
    // single-instance app (the common case) that's still just once
    // ever, since open_app() reuses the existing window on any later
    // request; for a `multi_instance` app it's once per window. Most
    // apps have no heap dependency at all: state lives in a static
    // struct inside the app's .c file, and this is typically just
    // window_set_state(win, &g_state). A `multi_instance` app can't do
    // that (a static struct is shared by definition) -- it needs a
    // fresh kzalloc() per window instead (kapi.h's heap.h) and a
    // matching on_close() (below) to kfree() it. See apps/calculator.c
    // for the first real example.
    void (*on_open)(struct window *win);

    // Called whenever the window's content area needs (re)painting --
    // after open, after resize/restore, or after anything the app did
    // that changes what should be on screen. Use window_content_x/y/w/h()
    // to find where and how big the content area is.
    void (*on_draw)(struct window *win);

    // Called when this window has keyboard focus (it's the frontmost,
    // non-minimized window) and a key arrives. `key` is an ASCII char or
    // a KEY_* code from keyboard.h. May be NULL if the app takes no input.
    //
    // `mods` is the KEY_MOD_* bits held when the key was produced. Most
    // apps ignore it: Ctrl and Alt are already folded into `key` by the
    // terminal encoding (Ctrl-A IS 0x01), so the bit that actually earns
    // its place is KEY_MOD_SHIFT, which the encoding cannot express for
    // a key with no shifted variant. Shift-Tab is the case that made
    // this necessary -- see keyboard.h's "Modifier bits".
    void (*on_key)(struct window *win, int key, uint8_t mods);

    // Called on left-button-DOWN inside the content area -- NOT on
    // release, despite the name. (cx, cy) are content-relative (0,0 =
    // top-left of the content area). May be NULL.
    //
    // **Do not commit an action here if the control should be
    // cancellable.** Nothing armed by on_press exists yet when this
    // runs, so a control acting here fires the instant the button goes
    // down and can never be cancelled by dragging away before release.
    // For press-then-commit-on-release behaviour -- what every button
    // in this GUI is supposed to have -- arm in on_press and act in
    // on_release (the title bar's own wm_update_title_btn_press() is
    // the worked example; apps/control_panel.c used to be the other
    // one, and moved to ring 3). This callback suits things that
    // genuinely act on contact: placing a text cursor, focusing a
    // field. See docs/gui-guidelines.md.
    //
    // NOT called for a press on_drag_start() claims (see below) -- the
    // two are mutually exclusive per press: either a click, or a drag,
    // never both for the same button-down.
    void (*on_click)(struct window *win, int cx, int cy);

    // Called every tick while the cursor is over this window's content
    // area and NO button is held, with content-relative coordinates --
    // and once more, with (-1, -1), when the cursor leaves. May be NULL.
    //
    // Return 1 only when the hovered item actually CHANGED, so holding
    // still doesn't force a repaint every tick. That contract is copied
    // verbatim from on_press above, which has the same shape for the
    // same reason; an app implementing both usually shares one
    // "which item is under this point" helper between them.
    //
    // Delivered to whichever window is under the cursor, focused or
    // not: a control that only lights up once its window has focus
    // feels dead in exactly the moment hover exists to serve. This is
    // the only app callback that reaches an unfocused window.
    int (*on_hover)(struct window *win, int cx, int cy);

    // Called on a left-button-DOWN inside the content area, before
    // on_click -- lets an app claim a multi-tick drag gesture (e.g.
    // dragging a scrollbar thumb, see widgets.h's widget_scrollbar_*)
    // instead of a single click. (cx, cy) are the button-down position,
    // content-relative like on_click's. Return 1 to start capturing the
    // drag: on_drag() (below) will then be called every subsequent tick
    // the button stays held, and on_click is skipped for this press.
    // Return 0 to fall through to the ordinary on_click contract
    // instead. May be NULL (equivalent to always returning 0) for any
    // app that has nothing worth dragging in its content area.
    int (*on_drag_start)(struct window *win, int cx, int cy);

    // Called every tick a drag this app started via on_drag_start() is
    // still in progress (left button still held), with the CURRENT
    // mouse position, content-relative. Never called unless
    // on_drag_start returned 1 for the press currently in progress; may
    // be NULL only if on_drag_start is also NULL or always returns 0.
    void (*on_drag)(struct window *win, int cx, int cy);

    // Called when this window has keyboard focus (same "frontmost,
    // non-minimized" rule as on_key) and the mouse wheel moves, with
    // `delta` in notches -- positive scrolls up (reveals older
    // content), negative scrolls down, matching mouse.h's
    // mouse_get_wheel_delta(). Unlike on_click/on_drag_start, this
    // isn't position-gated to the content area (there's no content-area
    // wheel target concept yet, and every current wheel user -- a
    // scrollable widget -- wants the whole window's wheel input
    // regardless of exact cursor position within it). May be NULL for
    // any app with nothing scrollable.
    void (*on_wheel)(struct window *win, int delta);

    // Called every tick the left mouse button is held down starting
    // from a content-area press that on_drag_start() did NOT claim as a
    // drag (same "either a click/press, or a drag, never both" split
    // on_click has) -- including the initial button-down tick, so a
    // press shows its visual feedback immediately rather than one tick
    // late. (cx, cy) are content-relative, updated every tick so an app
    // can track the button the mouse is CURRENTLY over as it moves
    // (dragging off before releasing should un-press, like a real OS
    // button). Return 1 if this call changed which button (if any) is
    // "hot," so the window manager knows to redraw; return 0 if nothing
    // changed, so a static hold doesn't force a full-scene repaint every
    // single tick. May be NULL for any app with nothing that needs
    // press/release feedback -- first real caller: apps/calculator.c
    // (see widget_button()'s `pressed` parameter, widgets.h).
    int (*on_press)(struct window *win, int cx, int cy);

    // Called once when the left button is released after an on_press
    // sequence (or the window closes/is minimized while one is in
    // progress) -- the app's cue to clear whatever it marked as
    // pressed so the next draw shows the button back in its normal
    // state. May be NULL only if on_press is also NULL.
    void (*on_release)(struct window *win);

    // Called once when this specific window closes -- via the title-bar
    // close button, currently the only way a GUI window closes (see
    // apps/wm/wm.c's close_window()). May be NULL, and is for most apps:
    // a single-instance app's state is a static struct with nothing to
    // release. `multi_instance` apps need this to kfree() whatever
    // on_open() kzalloc'd for this window -- without it, every open/
    // close cycle would leak. Called BEFORE the window's slot is
    // actually removed, so window_get_state(win) is still valid inside
    // this callback.
    void (*on_close)(struct window *win);

    // Called once a write started via wm.h's window_start_write() reaches
    // a terminal result -- `success` is 1 for FS_STEP_DONE, 0 for
    // FS_STEP_FAILED (fs.h). The window manager polls the write itself
    // (wm_run(), once per frame) purely as a scheduling mechanism; this
    // is where the app updates whatever UI told the user a save/write
    // was in progress. May be NULL for any app that never calls
    // window_start_write(). First (and currently only) user:
    // apps/notepad.c's Save As... (Milestone 1 phase 3, docs/roadmap.md).
    void (*on_write_complete)(struct window *win, int success);

    // Called once a read started via wm.h's window_start_read() reaches
    // a terminal result -- `success` is 1 for FS_STEP_DONE, 0 for
    // FS_STEP_FAILED (fs.h), `total` is the number of bytes actually
    // copied into the buffer passed to fs_read_range_begin() (may be
    // less than requested, at/near end-of-file -- see fs.h's
    // fs_read_range_step()). Same "WM polls purely as a scheduling
    // mechanism, this is where the app updates its UI" contract as
    // on_write_complete above. May be NULL for any app that never calls
    // window_start_read(). First (and currently only) user:
    // apps/notepad.c's Open... (Milestone 1 phase 4, docs/roadmap.md).
    void (*on_read_complete)(struct window *win, int success, uint32_t total);

    // Called once a process started via wm.h's window_start_process()
    // exits -- `exit_code` is the value it passed to the exit syscall
    // (scheduler.h's scheduler_poll() SCHED_POLL_EXITED result), or -1
    // if wm_run() got SCHED_POLL_INVALID (shouldn't happen in practice --
    // defensive only). Same "WM polls purely as a scheduling mechanism,
    // this is where the app updates its UI" contract as
    // on_write_complete/on_read_complete above -- the process's actual
    // OUTPUT already streamed into the window's scrollback in real time
    // via vga_putc()'s active sink (vga.h), independent of this
    // callback; this is purely "it's done, show the exit code and the
    // prompt again." May be NULL for any app that never calls
    // window_start_process(). First (and currently only) user:
    // apps/terminal.c's async `ls`/`run` (Milestone 1 phase 4b,
    // docs/roadmap.md).
    void (*on_process_exit)(struct window *win, int exit_code);

    // 1 (the common case) if the user can drag-resize and maximize this
    // app's window; 0 to fix it at its default_size() forever -- no
    // resize grip, hovering an edge doesn't show a resize cursor, and
    // the maximize button is drawn disabled and does nothing. First
    // used by Calculator, whose button grid has no sensible way to fill
    // extra space (see calculator.c) -- a per-app flag rather than
    // something the window manager decides on its own, since whether a
    // fixed size makes sense is an app-content question, not a WM one.
    int resizable;

    // 1 to remember this window's position and size across launches
    // (wm_geometry.h), which is the DEFAULT -- `RememberGeometry=false`
    // in the `.desktop` entry is how an app opts out. Default-on
    // because the alternative is every app having to ask for behaviour
    // people expect from all of them; the opt-out exists for a window
    // whose size is not the user's to choose, or one that should always
    // open where the app puts it.
    int remember_geometry;

    // 1 to allow more than one window of this app open at once (each
    // opened fresh from the Start menu, never reusing an existing
    // window -- see open_app()); 0 (the default) keeps the original
    // "reopen focuses the one window" behavior every app had before
    // this flag existed. An app that sets this MUST allocate its state
    // per-window (kzalloc() in on_open(), kfree() in on_close()) rather
    // than pointing every window at one shared static struct -- with a
    // static struct, two windows of the same app would silently share
    // (and stomp on) the same state. First (and, for now, only) user:
    // Calculator -- see docs/decisions.md for why it and not every app
    // got this.
    int multi_instance;

    // Non-NULL turns this entry into a LAUNCHER for a ring-3 program
    // rather than a kernel-space app: the absolute path of a binary
    // (e.g. "/bin/shapes"). Opening it spawns that process and returns
    // immediately -- no `struct window` is created here, and none of
    // the callbacks above are ever called (they must all be NULL, and
    // `default_size` is unused). The process creates its own window by
    // talking the windowing protocol (abi/win_proto.h), which
    // apps/wm/wm_client.c turns into a real window a moment later.
    //
    // This field exists because the registry was the ONLY way into the
    // Start menu and the desktop, and it could only describe apps that
    // live in the kernel. Once Calculator, Notepad and Terminal moved
    // to ring 3, the desktop had no way to launch any of them -- they
    // were reachable solely by typing `run calculator` in a Terminal,
    // while the Start menu kept opening the kernel-space versions. The
    // menu was quietly telling the truth and it looked like a bug.
    //
    // A launcher entry always SPAWNS; it never focuses an existing
    // window the way `multi_instance == 0` does. That is deliberate:
    // the WM cannot enforce single-instance on a ring-3 program
    // (a client window carries no `gui_app` pointer -- see
    // wm_client.c), and more to the point it shouldn't. Whether a
    // second copy of a program may run is the PROGRAM's decision, the
    // way it is on a real system; a launcher launches.
    const char *exec_path;

    // GUI_SHOW_* bits: which surfaces this entry appears on, from its
    // ShowIn= key. Always non-zero for a registered entry -- an entry
    // that appears nowhere is NoDisplay=1 and never reaches the
    // registry at all.
    unsigned show_in;

    // The `Category=` key verbatim, which is the FOLDER the Start menu
    // files this entry under. Never NULL -- an entry that named none
    // gets the default. Points into this file's storage like `name`,
    // so a sort that swaps two entries re-points both.
    const char *category;

    // `Comment=`, freedesktop's one-line description of what the app
    // IS -- shown under the Start menu's list for whichever row the
    // pointer or the keyboard is on. "" when the entry names none,
    // never NULL, so a caller draws an empty strip rather than
    // branching. Same storage rule as `name` and `category`.
    const char *comment;
};

// Caps on the live registry. Fixed tables rather than allocation, the
// same choice every other list in this WM makes.
#define GUI_APP_MAX      32
#define GUI_APP_NAME_MAX 32
// An Icon= name or an AppId -- both are short identifiers, and sharing
// one bound keeps the two arrays that hold them the same shape.
#define GUI_APP_ICON_MAX 24
#define GUI_APP_EXEC_MAX 64
// One line about what the app is, from `Comment=`. Long enough for a
// sentence that fits the menu's own width at the default font, and no
// longer: this is a description, not documentation -- `help` is that.
#define GUI_APP_COMMENT_MAX 64

// The registry, BUILT AT STARTUP from /usr/wm/applications/ -- see
// gui_apps.c's top comment and data/wm/applications/README.md. Not const any
// more, and not a compiled-in list: adding an app to the desktop is
// dropping a file there, not editing this file and rebuilding the
// kernel.
extern struct gui_app gui_app_registry[];
extern int gui_app_registry_count;

// --- which SURFACE an entry appears on --------------------------------
//
// The desktop icons and the Start menu are built from ONE directory
// (/usr/wm/applications), and an entry appears on both unless its ShowIn=
// key says otherwise. A second directory was the obvious alternative and
// is rejected: an app wanted in both places would need its file
// duplicated, and the two copies drift -- a renamed app or a changed
// Exec= silently updates one surface only. freedesktop.org answered the
// same question with a key (OnlyShowIn/NotShowIn) for the same reason.
//
// NoDisplay=1 still means NEITHER, and is not the same statement as
// ShowIn: "this is not a launchable thing" versus "this is, but only
// over there".
#define GUI_SHOW_DESKTOP   0x1
#define GUI_SHOW_STARTMENU 0x2
#define GUI_SHOW_ALL       (GUI_SHOW_DESKTOP | GUI_SHOW_STARTMENU)

// How many entries appear on `surface`, and the n'th of them.
//
// **Use these for anything positional.** The Start menu's rows are
// indexed by position, so drawing from a filtered list while hit-testing
// against the unfiltered registry lands every click on the wrong app --
// the exact drift this repo's "one geometry helper shared by draw and
// hit-test" rule exists to stop (see start_menu_geometry()). Routing
// both through one accessor makes that disagreement unrepresentable.
//
// gui_app_visible_at() returns NULL for an out-of-range index rather
// than clamping, so a stale index is a visible no-op instead of a
// launch of whatever happens to be last.
// ONE PARSED .desktop FILE, as either surface reads it: the application
// database builds its registry from these, and the desktop reads a
// launcher dropped in /home/desktop the same way (desktop.c).
struct gui_app_entry {
    char name[GUI_APP_NAME_MAX];
    char exec[GUI_APP_EXEC_MAX];
    char icon[GUI_APP_ICON_MAX];   // "" when Icon= was a bare character
    char glyph;                    // that character, else 0
    char app_id[GUI_APP_ICON_MAX];
    char category[16];
    char comment[GUI_APP_COMMENT_MAX];
    unsigned show_in;
    int remember_geometry;
};
// Reads `path` into *e. 0 when the file is missing, has no Name= or
// Exec=, says NoDisplay=1, or still names the retired builtin: form.
int gui_app_read_entry(const char *path, struct gui_app_entry *e);

int gui_app_visible_count(unsigned surface);
struct gui_app *gui_app_visible_at(unsigned surface, int n);

// --- CATEGORIES: the Start menu's folders -----------------------------
//
// The categories PRESENT among the entries a surface shows, in display
// order -- not a fixed list, so a folder exists exactly when something
// is in it and an empty one is unrepresentable. Derived per call rather
// than cached: gui_apps_load() can rewrite the registry between any two
// calls, and eleven entries make a scan cheaper than a cache that can
// go stale.
//
// Apps are addressed by category KEY rather than by category index, so
// a caller holding a selection across a reload cannot silently point at
// a different folder than the one it named.
int gui_app_cat_count(unsigned surface);
const char *gui_app_cat_key(unsigned surface, int n);     // "utility"
const char *gui_app_cat_label(unsigned surface, int n);   // "Utilities"
int gui_app_cat_size(unsigned surface, const char *key);
struct gui_app *gui_app_cat_at(unsigned surface, const char *key, int n);

// The entry with this AppId on `surface`, or NULL. The id is the
// `.desktop` entry's own stable handle, which is what anything
// PERSISTED must key on -- a name can be edited and a position moves
// on every reload (see start_store.h).
struct gui_app *gui_app_by_id(unsigned surface, const char *app_id);

// The display label for a key, for a caller that already holds one.
// An UNKNOWN key is its own label: a typo shows up as an oddly-named
// folder rather than as an app nobody can find.
const char *gui_app_cat_label_for(const char *key);

// Does this entry appear on `surface`? For a consumer that must keep
// registry indexing for its own reasons -- desktop.c's icon positions
// are registry-indexed and persisted by NAME, so it skips in place
// rather than re-indexing.
int gui_app_shows_in(const struct gui_app *app, unsigned surface);

// Scans the desktop-entry directory and fills the registry. Called once
// as the desktop starts, before anything draws a menu.
void gui_apps_load(void);

// "Could anything in the entry directory have changed?" -- one directory
// listing, no file reads, so it is cheap enough to ask on every
// filesystem change. Equal values mean nothing worth reloading; see
// gui_apps.c for the one edit it cannot see (a size-preserving one).
uint64_t gui_apps_dir_fingerprint(void);

#endif
