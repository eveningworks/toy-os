#ifndef WM_H
#define WM_H

#include "gfx.h" // WM_TITLEBAR_H depends on ugfx_char_h(), which can change
                 // at runtime now (see gfx_set_font_size()) -- this used
                 // to be a plain compile-time constant.
#include "win_proto.h" // WIN_CLIENT_BUFS: the client_px[] tables below

// The window manager's public, app-facing API -- everything a GUI app
// (Notepad, About, Calculator, ...) is allowed to use. Implemented
// across three files under apps/wm/: wm.c (this header's counterpart --
// shared state, window lifecycle, wm_run()'s main loop, and the
// app-facing helpers below), wm_input.c (mouse/keyboard handling), and
// wm_render.c (all drawing). wm_internal.h is the private glue between
// those three; nothing outside apps/wm/ should ever include it.

struct gui_app; // full definition in gui_apps.h

#define WIN_TITLE_MAX 32

// Mirrors abi/win_proto.h's WIN_APP_ID_LEN, the same way WIN_TITLE_MAX
// mirrors WIN_TITLE_LEN -- this header is app-facing and deliberately
// does not pull the client/server ABI in. wm_client.c static_asserts
// that the two agree, since a silently smaller buffer here would
// truncate ids and make two different apps match each other.
#define WIN_APP_ID_MAX 32

enum window_state { WIN_NORMAL, WIN_MINIMIZED, WIN_MAXIMIZED };

// One named control inside a client's window, in CONTENT coordinates --
// the client's own frame of reference, so it survives a move. See
// abi/win_proto.h's WIN_REQ_WIDGET for why the client reports this
// rather than the compositor working it out.
struct wm_widget {
    // Same width as the wire field the name arrives in (WIN_TITLE_LEN,
    // abi/win_proto.h) -- spelled with this header's own constant
    // because wm.h does not see the ABI header, and the two are equal.
    char name[WIN_TITLE_MAX];
    int x, y, w, h;
};

struct window {
    char title[WIN_TITLE_MAX];

    // When it opened, relative to the others (wm_next_open_seq()) -- a
    // window's identity for the taskbar and peek, since windows[] is
    // z-order and an index moves on every raise.
    uint32_t open_seq;
    // Its place on the taskbar: open order until a button is dragged,
    // then renumbered from the same counter, so a window opened later
    // still lands at the end.
    uint32_t task_rank;

    // THE CLIENT'S WIDGET MAP, for `gui probe` and `gui widgets`.
    // ALLOCATED ON FIRST REPORT, not inline: a window is not otherwise
    // 2 KiB, the table grows on demand, and most windows never report
    // one at all (a kernel-space app has no toolkit behind it). NULL is
    // the ordinary state and every reader checks it.
    struct wm_widget *widgets;
    int widget_count;
    int x, y, w, h;                        // current geometry, screen coords,
                                            // (x,y) = top-left incl. title bar
    int saved_x, saved_y, saved_w, saved_h; // geometry to restore to after
                                            // un-maximizing
    enum window_state state;
    // FULLSCREEN is a flag beside the state, not a fourth state: it
    // composes with normal and maximized (fs_prev remembers which, so
    // leaving it lands where the window was), and every `state ==`
    // test in the tree stays true. The content area is the whole
    // screen and there is no chrome (window_content_*, wm.c).
    int fullscreen;
    enum window_state fs_prev;
    // The client said it draws write-only (WIN_HINT_SCANOUT), so while
    // fullscreen it may be lent the display's buffers -- wm_scanout.c.
    int scanout_ok;
    // The client asked for keys by position too (WIN_HINT_PHYS_KEYS).
    int phys_keys;

    // Can the user resize this window? A property of the WINDOW, not of
    // whatever created it -- which is the whole point of it living here.
    //
    // It used to be read as `w->app && w->app->resizable` at seven
    // separate call sites, and `app` is NULL for a ring-3 client by
    // construction (a window has either an `app` or a client, never
    // both). So no client window could EVER be resizable -- not by
    // decision, but because "resizable" was a field on a struct only
    // kernel-space apps have. That is why WIN_EV_RESIZE has been
    // defined in TWP since the protocol was written and is still never
    // sent. Both kinds of window can answer this one.
    //
    // Set from gui_app::resizable when an app window opens; a client's
    // stays 0 until it can say otherwise (TWP's hints message, and the
    // resize handshake behind it).
    int resizable;

    // Smallest content size this window will accept, 0 for "the WM's
    // own floor". Only a client sets it (through TWP's hints); a
    // kernel-space app has never needed one because nothing resizes it
    // below its chrome.
    int min_w, min_h;

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

    // A CLIENT window: one owned by a ring-3 process rather than by a
    // kernel-space `struct gui_app`. `client_pid` is 0 for an ordinary
    // app window, and the two are mutually exclusive -- a window has
    // either an `app` or a client, never both.
    //
    // The WM's job for one of these is narrower than for an app window:
    // it owns the chrome, the geometry and the z-order exactly as
    // always, but the content is simply the client's pixels, blitted.
    // There is no on_draw() to call and no app state to keep, because
    // the client is a separate process that draws on its own schedule
    // and tells the WM when it's done (WIN_REQ_PRESENT).
    //
    // `client_buf` points at client_w * client_h 32bpp pixels -- the
    // client's OWN memory, a named shm object this process opened and
    // mapped (abi/win_proto.h's WIN_BUF_NAME_FMT). It stays valid as
    // long as this mapping does, whatever happens to the window: the
    // object's reference count is what keeps the frames alive.
    // What this window IS, as its own client named it -- "taskmgr", not
    // a path and not the title. Empty for a kernel-space app window and
    // for any client that did not give one. The saved geometry is keyed
    // on it and the taskbar labels a group with it; nothing MATCHES on
    // it -- see app_identity below.
    char app_id[WIN_APP_ID_MAX];

    // WHAT THIS WINDOW'S APPLICATION IS, as an opaque number interned
    // from the owning process's SPAWN PATH (QUERY_PROCPATH) -- equal for
    // two windows of the same PROGRAM, different otherwise, -1 for a
    // process the kernel has no path for. The taskbar groups by THIS and
    // single instance matches on it, not on app_id: an identity an app
    // declares about itself is one two apps can collide on, and the
    // collision is silent. See wm_client.c's identity_for_pid().
    int app_identity;

    int client_pid;
    uint32_t client_win;
    // WHICHEVER BUFFER IS CURRENTLY THE FRONT ONE. A window has two
    // (abi/win_proto.h's WIN_BUFFER_HALF) and the client draws into the
    // one this does NOT point at, which is what stops the compositor
    // reading a half-drawn frame.
    //
    // Kept as a pointer to the front rather than as base-plus-index so
    // that every reader here -- the blitter, the damage tests, the
    // not-responding check -- is unchanged: the only code that knows
    // there are two buffers is the present handler that moves this.
    uint32_t *client_buf;
    // The buffers as this process mapped them, and WHICH OBJECT
    // each mapping is of. A client replaces the object behind a name on
    // a resize; the present that first shows it carries a higher
    // generation, and that is when the name is re-opened. The old
    // mapping stays readable until then -- it holds the old object
    // alive by itself, which is wl_buffer.release.
    uint32_t *client_px[WIN_CLIENT_BUFS];
    uint64_t  client_bytes[WIN_CLIENT_BUFS];   // what was mapped, page-rounded
    uint32_t  client_gen[WIN_CLIENT_BUFS];     // 0 until the buffer has been opened
    int client_mapped[WIN_CLIENT_BUFS];        // client_px[b] may legitimately be 0
    int client_front;
    // THE FRONT BUFFER'S SIZE, which is the size of the pixels on
    // screen. A client that has accepted a new size has not necessarily
    // DRAWN it yet; this follows the frames, adopted in the present
    // handler.
    int client_w, client_h;
    // HOW LONG THIS WINDOW LAST TOOK to become a size it was asked for,
    // in milliseconds; 0 until one has been measured. `auto` reads it
    // when a drag begins, which is what lets a slow app be outlined from
    // the first pixel of the second drag rather than lagging through the
    // first. Per window rather than global: two windows of different
    // apps have nothing to say about each other.
    unsigned resize_lag_ms;
    // Last cursor position delivered to this client, so a stationary
    // cursor doesn't generate a WIN_EV_MOUSE_MOVE every single frame.
    // Without this the WM wakes the client once per frame forever, and
    // a client that is doing nothing still burns half the CPU under the
    // scheduler's rotation. INT32_MIN means "nothing sent yet".
    int client_last_mx, client_last_my;

    // --- liveness (see apps/wm/wm_client.c's "Is it still there?") ---
    //
    // A client that has stopped pumping its queue looks exactly like an
    // idle one from out here: both draw nothing and send nothing. So the
    // WM asks -- WIN_EV_PING with a serial, WIN_REQ_PONG back -- and
    // these track that exchange. `ping_serial` is 0 when no ping is
    // outstanding.
    // --- the client's repeating timer (TWP's WIN_REQ_TIMER) ----------
    //
    // `timer_period_ns` is the interval, 0 for none; `timer_due_ns` is
    // the sys_monotonic_ns() it next fires at. A firing ADVANCES the
    // deadline by one period, so the cadence does not drift by however
    // late each check ran -- but never leaves it in the past, so a
    // client that fell behind gets one firing, not a backlog.
    uint64_t timer_period_ns;
    uint64_t timer_due_ns;

    uint32_t ping_serial;
    uint64_t ping_sent_tick;
    uint64_t ping_sent_ns;
    uint64_t ping_sent_tsc;  // rdtsc, for a figure finer than the clocksource under an emulator   // for the round-trip figure `gui compositor` reports
    int not_responding;   // no answer within WM_PING_TIMEOUT_TICKS

    // --- events the client's inbox could not take (stage 8) --------
    //
    // The inbox is a ring this process writes and only the client
    // drains, so a full one cannot be evicted from. Input is dropped
    // (counted in `ev_dropped`); a STATE is kept as a bit here and sent
    // again next frame, with whatever the state is by then -- the latest
    // one is all the client needs. `ev_popup_done` is per popup SLOT,
    // held on the toplevel because the popup's own row is gone by then.
    uint32_t ev_pending;      // WM_PEND_* bits
    uint32_t ev_popup_done;   // bit per client slot
    int ev_resize_w, ev_resize_h;
    unsigned ev_dropped;

    // When this window was ASKED to close (0 = not asked). A client is
    // entitled to take its time, or to refuse outright, so this alone
    // is never grounds for anything -- it is paired with a missing pong,
    // which is what separates "declined" from "wedged".
    uint64_t close_asked_tick;

    // The close_asked_tick a force-quit dialog was already offered for.
    // The offer used to ride the TRANSITION into not_responding, which
    // stopped working the moment pings became periodic: a window can now
    // be flagged long before anyone asks it to close, and the transition
    // has then already happened. Comparing against the ASK is what makes
    // "offer once per request" independent of when the flag was set.
    uint64_t force_quit_offered_tick;

    // WIN_CURSOR_*, as this client asked. Honoured only inside its
    // CONTENT area (client_cursor_at(), wm_render.c) -- that clamp is
    // what stops a wedged client stranding a shape over the desktop.
    int client_cursor;

    // A POPUP SURFACE (abi/win_proto.h's WIN_REQ_POPUP): a second window
    // of the same client with NO CHROME -- window_content_*() answer the
    // whole rect -- no taskbar button, no saved geometry, placed by the
    // compositor against the work area and dismissed by a press outside
    // every surface of its client. `popup_parent` is the slot it was
    // anchored to. Created at the top of the list; it never needs to
    // follow its parent because the press that would move the parent
    // dismisses it first.
    //
    // `popup_grab` is WIN_POPUP_GRAB: only a GRABBING popup takes the
    // pointer and is dismissed by a press outside. A tooltip sets it to
    // 0 and input routes as if it were not there -- so a popup with no
    // grab is dismissed by nobody but its own client.
    int popup;
    int popup_grab;
    uint32_t popup_parent;

    // A DIALOG WINDOW (abi/win_proto.h's WIN_REQ_DIALOG): a second
    // toplevel of the same client, WITH chrome, owned by one of its
    // client's windows -- centred on it, stacked with it, and carrying
    // no taskbar button of its own. `dialog_owner` is the owner's slot.
    //
    // `modal` means the owner takes no input while this is up: a press
    // on it raises this instead. Win32 disables the owner HWND for the
    // same effect; KDE raises and flashes.
    //
    // ONE LEVEL: a dialog never owns a dialog, so nothing here recurses.
    int dialog;
    uint32_t dialog_owner;
    int modal;
};

// Height of a window's title bar in pixels. NOT the taskbar's, which is
// a setting (`desktop.taskbar_height`, wm_taskbar.h) -- a panel and a
// title bar are separate measurements on every desktop.
// A macro rather than a cached variable so it always reflects whatever
// font size is currently active -- gfx_char_h() is cheap (just an array
// lookup), so recomputing this on every use costs nothing.
#define WM_TITLEBAR_H (ugfx_char_h() + 8)

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

// --- taskbar notification area (tray) ---
//
// A small right-to-left strip of text items in the taskbar, next to
// the clock (which is itself tray item 0 -- see wm_tray.c). An app
// registers an item once (e.g. from on_open), then updates its text
// as often as it likes (e.g. once per tick, or in response to its own
// state changing) -- there's no polling/callback needed on the app's
// side, unlike window_start_write()'s "WM polls a handle" shape above,
// since a tray item's text is push-only: the app decides when it has
// changed. Unregister it (e.g. from on_close) so a closed app doesn't
// leave a stale item behind.
#define TRAY_TEXT_MAX 16

// Registers a new tray item with the given initial text (truncated to
// TRAY_TEXT_MAX - 1 chars). Returns a handle to pass to
// tray_set_text()/tray_unregister(), or -1 if the tray is full (see
// TRAY_MAX_ITEMS in wm_tray.c).
int tray_register(const char *initial_text);

// Updates a tray item's text in place (truncated to TRAY_TEXT_MAX - 1
// chars) -- damages just the taskbar strip and asks for a redraw. A
// no-op on an invalid/unregistered id.
void tray_set_text(int tray_id, const char *text);

// Releases a tray item. A no-op on an invalid/already-unregistered id.
void tray_unregister(int tray_id);

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
