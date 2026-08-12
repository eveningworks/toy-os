#ifndef WM_H
#define WM_H

#include "gfx.h" // WM_TITLEBAR_H depends on gfx_char_h(), which can change
                 // at runtime now (see gfx_set_font_size()) -- this used
                 // to be a plain compile-time constant.

// The window manager's public, app-facing API -- everything a GUI app
// (Notepad, About, Calculator, ...) is allowed to use. Implemented
// across three files under apps/wm/: wm.c (this header's counterpart --
// shared state, window lifecycle, wm_run()'s main loop, and the
// app-facing helpers below), wm_input.c (mouse/keyboard handling), and
// wm_render.c (all drawing). wm_internal.h is the private glue between
// those three; nothing outside apps/wm/ should ever include it.

struct gui_app; // full definition in gui_apps.h

#define WIN_TITLE_MAX 32

enum window_state { WIN_NORMAL, WIN_MINIMIZED, WIN_MAXIMIZED };

struct window {
    char title[WIN_TITLE_MAX];
    int x, y, w, h;                        // current geometry, screen coords,
                                            // (x,y) = top-left incl. title bar
    int saved_x, saved_y, saved_w, saved_h; // geometry to restore to after
                                            // un-maximizing
    enum window_state state;
    const struct gui_app *app;
    void *app_state;
    int open; // 1 while this slot is in use

    // Rendering-internal bookkeeping for wm_render.c's damage-region
    // compositor (see docs/decisions.md) -- not for app callbacks to
    // touch, same "peer-level, not part of the app-facing API" spirit
    // as saved_x/y/w/h above. Tracks this window's rect + visibility
    // as of the last actual repaint, so wm_render_frame() can tell
    // whether (and where) its contribution to the scene changed since
    // then. last_w == 0 means "never rendered yet" (a real window's w
    // is always > 0) -- the sentinel for "just opened, nothing to
    // diff against".
    int last_x, last_y, last_w, last_h;
    int last_visible;
};

// Height of a window's title bar in pixels (matches the taskbar height).
// A macro rather than a cached variable so it always reflects whatever
// font size is currently active -- gfx_char_h() is cheap (just an array
// lookup), so recomputing this on every use costs nothing.
#define WM_TITLEBAR_H (gfx_char_h() + 8)

// --- helpers for use inside a gui_app's callbacks ---

// Stashes/retrieves the app's own state pointer on its window. Call
// window_set_state from on_open; call window_get_state from any other
// callback to get it back.
void window_set_state(struct window *win, void *state);
void *window_get_state(struct window *win);

// The content area is everything below the title bar, inset by the 1px
// border. All four are in screen coordinates / pixels.
int window_content_x(const struct window *win);
int window_content_y(const struct window *win);
int window_content_w(const struct window *win);
int window_content_h(const struct window *win);

// Ask the window manager to repaint on its next cycle. Rendering is
// whole-screen (see wm_render.c) so this doesn't need to know what
// changed -- call it after any on_key/on_click that changed what should
// be drawn.
void window_invalidate(struct window *win);

// Registers a steppable write (a handle from fs.h's
// fs_write_range_begin()) as the window manager's pending write --
// wm_run()'s main loop polls it once per frame via
// fs_write_range_step() instead of blocking, so a slow disk write no
// longer freezes the whole desktop (Milestone 1 phase 3, see
// docs/roadmap.md). Returns 1 if accepted, 0 if a write is already
// pending (WM-global single slot -- see wm_internal.h's pending_write;
// the caller should treat 0 the same as window_write_pending()
// returning true and not start a second write). `win` is notified of
// the outcome via gui_apps.h's on_write_complete callback, called once
// the write reaches FS_STEP_DONE or FS_STEP_FAILED -- the caller does
// NOT need to (and must not) call fs_write_range_step() itself.
int window_start_write(struct window *win, void *write_handle);

// True while any window has a write in progress via window_start_write()
// -- an app can check this before starting a new Save to avoid a
// re-entrant second write (e.g. ignore the click / leave the button
// disabled) instead of relying on window_start_write()'s return value
// alone.
int window_write_pending(void);

// Registers a steppable read (a handle from fs.h's fs_read_range_begin())
// as the window manager's pending read -- wm_run()'s main loop polls it
// once per frame via fs_read_range_step() instead of blocking, mirroring
// window_start_write()/window_write_pending() above exactly (Milestone 1
// phase 4, see docs/roadmap.md). Returns 1 if accepted, 0 if a read is
// already pending (a separate WM-global single slot from the write one --
// see wm_internal.h's pending_read). `win` is notified of the outcome via
// gui_apps.h's on_read_complete callback, called once the read reaches
// FS_STEP_DONE or FS_STEP_FAILED, with the final byte count -- the caller
// does NOT need to (and must not) call fs_read_range_step() itself.
int window_start_read(struct window *win, void *read_handle);

// True while any window has a read in progress via window_start_read()
// -- same purpose as window_write_pending() above, for the read slot.
int window_read_pending(void);

// Registers a pid from scheduler.h's scheduler_spawn() as the window
// manager's pending process -- wm_run()'s main loop polls it once per
// frame via scheduler_poll() instead of the caller ever blocking on it,
// same shape as window_start_write()/window_start_read() above
// (Milestone 1 phase 4b, see docs/roadmap.md). Returns 1 if accepted, 0
// if a process is already pending (a separate WM-global single slot
// from the write/read ones -- see wm_internal.h's pending_proc). Unlike
// the I/O pair, the spawned process's own output reaches `win`'s
// scrollback with NO help from this polling at all -- it already goes
// straight through vga_putc() to whatever apps/wm/terminal.c installed
// as the active sink (vga.h's struct vga_sink) the moment the process's
// own SYS_WRITE syscalls run, entirely independent of wm_run()'s frame
// rate. All this registration is for is knowing when to stop waiting:
// `win` is notified via gui_apps.h's on_process_exit callback, called
// once scheduler_poll() reaches SCHED_POLL_EXITED, with the process's
// real exit code.
int window_start_process(struct window *win, int pid);

// True while any window has a process in progress via
// window_start_process() -- same purpose as window_write_pending()/
// window_read_pending() above, for the process slot.
int window_process_pending(void);

// The window manager's entry point -- this is what gui_main() calls.
// Runs until the user presses Esc, then returns.
void wm_run(void);

// --- read-only window introspection (Task Manager's data source) ---
//
// windows[]/window_count themselves live in wm_internal.h, which is
// deliberately not includable outside apps/wm/ (see that header's top
// comment) -- these two accessors are the small, safe query surface an
// app like Task Manager (apps/taskmgr.c) needs instead: how many
// windows are open, and a read-only snapshot of one by index (in the
// same z-order windows[] uses, index 0 = back). Returns 0 from
// wm_get_window() for an out-of-range index rather than a garbage
// pointer -- callers should always check window_count first anyway,
// but this makes an off-by-one harmless instead of a fault.
int wm_window_count(void);
const struct window *wm_get_window(int index);

#endif
