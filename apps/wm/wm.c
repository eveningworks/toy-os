// The window manager: owns the screen once gui_main() hands off to it.
// Keeps a small fixed array of windows in z-order (index 0 = back,
// highest index = front/focused) and drives everything through a single
// event loop -- mouse clicks, dragging, keyboard routed to the focused
// window, and a full-screen redraw whenever something changes.
//
// This file holds the shared state, the app-facing helpers (wm.h's
// window_* functions), window lifecycle (open/close/focus), and
// wm_run()'s main loop -- the "core" a reader should start at. Mouse/
// keyboard handling is wm_input.c; all drawing is wm_render.c. See
// wm_internal.h for the private glue between the three -- none of this
// split changes behavior, it's the same single-threaded event loop as
// before, just organized into files by concern.
//
// Rendering draws the whole scene every time a window/taskbar/menu
// actually changes (see wm_render.c) rather than tracking per-window
// dirty rectangles -- much simpler to get right with overlapping movable
// windows, at the cost of being slower than a "real" compositor. It draws
// into an off-screen buffer and flips the finished frame in one pass
// (gfx_set_double_buffered / gfx_present), so the repaint isn't visible
// as flicker. gfx_present() itself only blits the sub-rectangle that
// actually changed (see its own comment in gfx.c) rather than the whole
// screen, which is what makes the other common case -- the mouse moving
// with nothing else changing -- cheap without needing real dirty-rect
// tracking of the scene: see wm_render_cursor_move() in wm_render.c.
#include "wm_internal.h"
#include "wm_debug.h"
#include "start_menu.h"
#include "context_menu.h"
#include "confirm_dialog.h"
#include "file_picker.h"
#include "desktop.h"
#include "wm_tray.h"
#include "kapi.h"

struct window windows[MAX_WINDOWS];
int window_count = 0;

int screen_w, screen_h;
int taskbar_h;

// Title-bar minimize/maximize/close button state -- see
// wm_internal.h's comment on title_btn_armed_win for the full
// press-hover-commit-on-release story.
int title_btn_armed_win = -1;
int title_btn_armed_kind = -1;
int title_btn_pressed_active = 0;
int title_hover_win = -1;
int title_hover_kind = -1;

int wm_exit_requested = 0;

int dragging = -1; // index into windows[], or -1 if not dragging
int drag_off_x, drag_off_y;

int resizing = -1; // index into windows[], or -1 if not resizing
int resize_right = 0, resize_bottom = 0;
int resize_start_mx, resize_start_my;
int resize_start_w, resize_start_h;

int content_dragging = -1; // index into windows[], or -1 -- see wm_internal.h
int content_pressed = -1; // index into windows[], or -1 -- see wm_internal.h

void *pending_write = 0; // handle from fs_write_range_begin(), or NULL -- see wm_internal.h
int pending_write_win = -1; // index into windows[] the write belongs to, or -1

void *pending_read = 0; // handle from fs_read_range_begin(), or NULL -- see wm_internal.h
int pending_read_win = -1; // index into windows[] the read belongs to, or -1

int pending_proc = 0; // pid from scheduler_spawn(), or 0 -- see wm_internal.h
int pending_proc_win = -1; // index into windows[] the process belongs to, or -1

int redraw_pending = 1;

// ---- app-facing helpers (declared in wm.h) ----

void window_set_state(struct window *win, void *state) { win->app_state = state; }
void *window_get_state(struct window *win) { return win->app_state; }

int window_content_x(const struct window *win) { return win->x + 1; }
int window_content_y(const struct window *win) { return win->y + WM_TITLEBAR_H + 1; }
int window_content_w(const struct window *win) { return win->w - 2; }
int window_content_h(const struct window *win) { return win->h - WM_TITLEBAR_H - 2; }

void window_invalidate(struct window *win) {
    wm_damage_rect(win->x, win->y, win->w, win->h);
    redraw_pending = 1;
}

int window_write_pending(void) { return pending_write != 0; }

int window_start_write(struct window *win, void *write_handle) {
    if (pending_write) return 0; // a write's already in flight -- WM-global single slot, see wm_internal.h
    pending_write = write_handle;
    pending_write_win = -1;
    for (int i = 0; i < window_count; i++) {
        if (&windows[i] == win) { pending_write_win = i; break; }
    }
    return 1;
}

int window_read_pending(void) { return pending_read != 0; }

int window_start_read(struct window *win, void *read_handle) {
    if (pending_read) return 0; // a read's already in flight -- WM-global single slot, see wm_internal.h
    pending_read = read_handle;
    pending_read_win = -1;
    for (int i = 0; i < window_count; i++) {
        if (&windows[i] == win) { pending_read_win = i; break; }
    }
    return 1;
}

int window_process_pending(void) { return pending_proc != 0; }

int window_start_process(struct window *win, int pid) {
    if (pending_proc) return 0; // a process's already in flight -- WM-global single slot, see wm_internal.h
    pending_proc = pid;
    pending_proc_win = -1;
    for (int i = 0; i < window_count; i++) {
        if (&windows[i] == win) { pending_proc_win = i; break; }
    }
    return 1;
}

// --- read-only window introspection (see wm.h's declarations) ---

int wm_window_count(void) { return window_count; }

const struct window *wm_get_window(int index) {
    if (index < 0 || index >= window_count) return 0;
    return &windows[index];
}

// ---- window lifecycle ----

void bring_to_front(int idx) {
    if (idx == window_count - 1) return;

    // A real reorder -- this window's own footprint is now drawn on
    // top of whatever it overlaps instead of wherever it was in
    // z-order before, so it needs repainting even though its geometry
    // isn't changing (compute_window_damage()'s geometry diff,
    // wm_render.c, wouldn't notice this on its own). This alone is
    // sufficient for the floating windows' OCCLUSION: bring_to_front()
    // only ever promotes ONE window to the front, shifting others down
    // a slot without changing their relative order to each OTHER, so
    // no other window's stacking relative to another changes. But the
    // previously-frontmost window's own titlebar tint (focused blue vs.
    // unfocused gray, draw_window_chrome()'s `focused` param) DOES
    // change even though its geometry doesn't -- it needs its own rect
    // damaged too, not just the newly-promoted window's. (Was missed
    // when this file's damage reporting first shipped -- harmless back
    // then since every window's chrome still got *drawn*, just clipped
    // away outside the damage rect; became a real visible bug once
    // wm_render.c's Phase 3 started skipping the draw call entirely for
    // undamaged windows -- see docs/decisions.md.) The taskbar strip is
    // a separate matter too -- draw_taskbar()'s per-button tint depends
    // on which window is frontmost, so both the newly- and
    // previously-frontmost buttons need a repaint.
    struct window *prev_front = &windows[window_count - 1];
    wm_damage_rect(prev_front->x, prev_front->y, prev_front->w, prev_front->h);
    wm_damage_rect(windows[idx].x, windows[idx].y, windows[idx].w, windows[idx].h);
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);

    struct window tmp = windows[idx];
    for (int i = idx; i < window_count - 1; i++) windows[i] = windows[i + 1];
    windows[window_count - 1] = tmp;

    // Reordering shifts every slot between idx and the old last slot
    // down by one, and moves idx's own window to the end -- keep
    // pending_write_win pointing at the same window (see close_window()
    // for why this has to stay accurate: the QMP test this phase ships
    // with explicitly clicks a DIFFERENT window mid-save, which lands
    // exactly here).
    if (pending_write_win == idx) pending_write_win = window_count - 1;
    else if (pending_write_win > idx) pending_write_win--;

    // Same bookkeeping for a pending read -- see wm_internal.h's
    // pending_read.
    if (pending_read_win == idx) pending_read_win = window_count - 1;
    else if (pending_read_win > idx) pending_read_win--;

    // Same bookkeeping for a pending process -- see wm_internal.h's
    // pending_proc.
    if (pending_proc_win == idx) pending_proc_win = window_count - 1;
    else if (pending_proc_win > idx) pending_proc_win--;
}

static int find_window_for_app(const struct gui_app *app) {
    for (int i = 0; i < window_count; i++)
        if (windows[i].app == app) return i;
    return -1;
}

void open_app(const struct gui_app *app) {
    // Single-instance apps (the default -- see gui_apps.h's
    // `multi_instance` flag): reopening from the Start menu just
    // focuses/restores the one window that can ever exist, same as
    // always. Multi-instance apps (Calculator) skip this check
    // entirely and always fall through to opening a brand new window
    // -- MAX_WINDOWS is still the only cap on how many.
    if (!app->multi_instance) {
        int existing = find_window_for_app(app);
        if (existing >= 0) {
            if (windows[existing].state == WIN_MINIMIZED) windows[existing].state = WIN_NORMAL;
            bring_to_front(existing);
            redraw_pending = 1;
            return;
        }
    }
    if (window_count >= MAX_WINDOWS) return; // no room -- silently ignore

    struct window *win = &windows[window_count];
    k_memset(win, 0, sizeof(*win));

    int cascade = (window_count % 5) * 24;
    win->x = 60 + cascade;
    win->y = 40 + cascade;
    // The window that is ABOUT to stop being frontmost repaints its
    // title bar (focused blue -> unfocused grey), so it has to be
    // damaged too -- exactly what bring_to_front() below already does
    // for the same reason. Opening a window used to damage only the new
    // one, leaving the old title bar showing the focused colour until
    // something else happened to repaint it.
    //
    // Found by `gui damage verify on` (wm_render.c), seconds after that
    // mode first ran: "4350 px changed outside the damage rect, first at
    // (61,41)" -- (61,41) being the previously-focused window's title
    // bar. Nothing else had noticed it.
    if (window_count > 0) {
        struct window *losing_focus = &windows[window_count - 1];
        wm_damage_rect(losing_focus->x, losing_focus->y,
                        losing_focus->w, losing_focus->h);
    }

    int content_w, content_h;
    app->default_size(&content_w, &content_h);
    win->w = content_w + 2;
    win->h = content_h + WM_TITLEBAR_H + 2;
    win->state = WIN_NORMAL;
    win->app = app;
    win->open = 1;

    int i = 0;
    for (; app->name[i] && i < WIN_TITLE_MAX - 1; i++) win->title[i] = app->name[i];
    win->title[i] = '\0';

    window_count++;
    if (app->on_open) app->on_open(win);
    redraw_pending = 1;
    // A new taskbar button appears -- compute_window_damage()
    // (wm_render.c) will damage the new window's own rect (its
    // last_w == 0 sentinel), but the taskbar strip is a separate area.
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);

    klog_write("wm: opened ");
    klog_write(app->name);
    klog_write("\n");
}

void close_window(int idx) {
    // Refuse to close a window with a write in flight -- pending_write's
    // handle is polled by index (pending_write_win), and the callback it
    // eventually fires (gui_apps.h's on_write_complete) is delivered to
    // &windows[pending_write_win]; closing mid-write would either shift
    // that slot to point at a DIFFERENT window by the time the write
    // finishes, or (if this is the last window) leave it dangling. Same
    // "block while pending" choice as window_start_write() refusing a
    // second concurrent write. See wm.h's window_write_pending().
    if (idx == pending_write_win) return;

    // Same refusal for a read in flight -- see wm.h's window_read_pending().
    if (idx == pending_read_win) return;

    // Same refusal for a process in flight -- see wm.h's window_process_pending().
    if (idx == pending_proc_win) return;

    // Give the app a chance to release whatever it allocated for this
    // window (multi-instance apps kmalloc/kzalloc their own per-window
    // state -- see gui_apps.h's `multi_instance` flag and
    // apps/calculator.c) BEFORE the slot shifts out of existence.
    // Single-instance apps generally leave this NULL -- their state is
    // a static struct with nothing to free.
    if (windows[idx].app && windows[idx].app->on_close) {
        windows[idx].app->on_close(&windows[idx]);
    }

    // Report the closing window's own rect as damage BEFORE the array
    // shift below removes it -- whatever's now revealed there (desktop
    // or another window) needs repainting. Skipped if it was minimized
    // (not drawn there in the first place, so nothing to reveal). The
    // taskbar strip always needs it too, regardless of minimized state
    // -- its button disappears and every later button shifts left.
    if (windows[idx].state != WIN_MINIMIZED) {
        wm_damage_rect(windows[idx].x, windows[idx].y, windows[idx].w, windows[idx].h);
    }
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);

    // Closing the FRONTMOST window hands focus to the one below it, and
    // that window's title bar changes (focused blue vs. unfocused gray,
    // draw_window_chrome()'s `focused` param) without its geometry
    // changing at all -- so compute_window_damage() (wm_render.c) can't
    // notice it, and the rect damaged above only covers it where the
    // two happened to overlap. This is bring_to_front()'s prev_front gap
    // seen from the other end, and it has the same consequence now that
    // wm_render.c's Phase 3 skips an undamaged window's chrome entirely:
    // the inheriting window keeps drawing as unfocused until something
    // else repaints it. Found by `gui damage verify on` (via
    // tools/damage_sweep.py): "11038 px changed outside the damage rect,
    // first at (109,89)" on closing the front window, (109,89) being the
    // title bar of the one underneath.
    //
    // Only the last slot matters -- closing any other window shifts
    // slots down without changing which one is frontmost. A minimized
    // inheritor isn't drawn, so it needs nothing beyond the taskbar
    // strip already damaged above.
    if (idx == window_count - 1 && window_count >= 2) {
        struct window *inheritor = &windows[window_count - 2];
        if (inheritor->state != WIN_MINIMIZED) {
            wm_damage_rect(inheritor->x, inheritor->y, inheritor->w, inheritor->h);
        }
    }

    klog_write("wm: closed ");
    klog_write(windows[idx].app ? windows[idx].app->name : windows[idx].title);
    klog_write("\n");

    for (int i = idx; i < window_count - 1; i++) windows[i] = windows[i + 1];
    window_count--;
    redraw_pending = 1;

    // A different window closing shifts every later slot down by one --
    // keep pending_write_win pointing at the same window if it moved.
    if (pending_write_win > idx) pending_write_win--;
    if (pending_read_win > idx) pending_read_win--;
    if (pending_proc_win > idx) pending_proc_win--;
}

// ---- main loop ----

void wm_run(void) {
    if (!gfx_init()) {
        vga_write("gui: no linear RGB framebuffer available from GRUB\n");
        klog_write("wm: cannot enter GUI mode -- no linear RGB framebuffer from GRUB\n");
        return;
    }

    screen_w = gfx_width();
    screen_h = gfx_height();
    taskbar_h = WM_TITLEBAR_H;

    klog_write("wm: entering GUI mode (");
    klog_write_dec((uint32_t)screen_w);
    klog_write("x");
    klog_write_dec((uint32_t)screen_h);
    klog_write(")\n");

    // Draw off-screen and flip completed frames -- without this the
    // full-screen repaint below is visible as flicker while it happens.
    // If the mode is too large to buffer we just draw directly; it still
    // works, it just flickers (see gfx_set_double_buffered).
    gfx_set_double_buffered(1);

    mouse_set_bounds(screen_w, screen_h);
    mouse_init();

    // Start accepting client windows. Registered here rather than at
    // boot so a ring-3 client that runs outside GUI mode is refused
    // (SYS_WIN_REQUEST returns -1) instead of drawing into a buffer
    // nothing will ever composite -- see wm_client.c.
    wm_client_init();

    window_count = 0;
    start_menu_open = 0;
    context_menu_close();
    dragging = -1;
    resizing = -1;
    content_dragging = -1;
    content_pressed = -1;
    title_btn_armed_win = -1;
    title_btn_armed_kind = -1;
    title_btn_pressed_active = 0;
    title_hover_win = -1;
    title_hover_kind = -1;
    redraw_pending = 1;
    wm_exit_requested = 0;
    pending_write = 0;
    pending_write_win = -1;
    pending_read = 0;
    pending_read_win = -1;
    pending_proc = 0;
    pending_proc_win = -1;

    wm_render_reset(); // first frame must be a full repaint -- see wm_render.c
    tray_init();

    int mx, my;
    uint8_t buttons;
    mouse_get_state(&mx, &my, &buttons);
    int prev_mx = mx, prev_my = my;
    uint8_t prev_buttons = buttons;

    for (;;) {
        __asm__ volatile ("hlt");

        // Same idle-wakeup piggyback keyboard_getchar() does for the
        // physical shell -- see kapi.h's include comment and
        // docs/decisions.md. Keeps the serial debug console responsive
        // while the GUI desktop is up, not just at the physical prompt.
        debug_console_poll();

        mouse_get_state(&mx, &my, &buttons);

        // Synthetic input from the serial debug console's `gui click` /
        // `gui drag` (apps/wm/wm_debug.c), consumed at most one event
        // per iteration so each becomes its own tick -- which is what
        // puts a press and its release on separate frames, as every
        // arm-on-press/commit-on-release control here requires.
        //
        // Overrides the real mouse for this iteration only; it does not
        // move the actual cursor, so once the queue drains the pointer
        // is wherever the user left it. Fine for the scripted-test use
        // this exists for, and worth knowing before reading a `gui
        // state` cursor position mid-sequence.
        {
            int ix, iy;
            uint8_t ib;
            if (wm_debug_next_input(&ix, &iy, &ib)) {
                mx = ix; my = iy; buttons = ib;
            }
        }
        int mouse_moved = (mx != prev_mx || my != prev_my);

        int left_edge_down = (buttons & 0x1) && !(prev_buttons & 0x1);
        if (left_edge_down) wm_handle_left_click(mx, my);

        // bit1 = right button (see mouse.h's mouse_get_state()) -- same
        // edge-triggered pattern as the left button above, dispatching
        // to context_menu.h's popup instead of a window action.
        int right_edge_down = (buttons & 0x2) && !(prev_buttons & 0x2);
        if (right_edge_down) wm_handle_right_click(mx, my);

        // A Start-menu action (currently just "Exit to shell") may have
        // just set this -- bail out the same way Esc used to, before
        // touching drag/resize state for a click that was never about a
        // window in the first place. Deferred while a write is pending
        // (below still runs, so the write keeps stepping to completion
        // -- wm_exit_requested stays set and this fires on the very next
        // tick after it finishes) rather than abandoning it mid-write:
        // pending_write's handle owns kernel heap state (see tfs.c's
        // struct tfs_write_step) that only gets freed on a terminal
        // fs_write_range_step() result. Same reasoning for pending_read
        // (struct tfs_read_step), and for pending_proc: whichever window
        // started it (apps/terminal.c) has an active vga_sink installed
        // (vga.h) pointing at ITS scrollback -- exiting GUI mode with
        // that still installed would silently swallow the physical
        // shell's own prompt output the moment control returned to it
        // (see vga.h's struct vga_sink doc comment on why a stale sink
        // is dangerous), not just abandon the process.
        if (wm_exit_requested && !pending_write && !pending_read && !pending_proc) {
            klog_write("wm: exiting GUI mode, returning to shell\n");
            // Stop accepting client windows before the desktop stops
            // drawing them -- same reasoning as the pending_* guards
            // above, one layer down: a request serviced after this
            // point would attach a window to a list nobody repaints.
            wm_client_shutdown();
            gfx_set_double_buffered(0); // console draws straight to screen
            return;
        }

        // Poll one step of a pending write, once per frame, instead of
        // wm_run() ever calling fs_write_range() and blocking (Milestone
        // 1 phase 3, docs/roadmap.md) -- Notepad's Save is the first
        // caller (see wm.h's window_start_write()). One step is one
        // filesystem block's worth of work (see fs.h's
        // fs_write_range_step() contract), so this bounds each frame's
        // extra blocking time to a single block write, not the whole
        // file. On a terminal result the handle is already freed by
        // fs_write_range_step() itself -- clear the slot and hand the
        // outcome to whichever window started it, if it's still open
        // (see close_window()/bring_to_front() for why pending_write_win
        // is guaranteed to still be accurate here even if other windows
        // closed or reordered while this write was in flight).
        if (pending_write) {
            enum fs_step_result r = fs_write_range_step(pending_write);
            if (r != FS_STEP_PENDING) {
                void *finished_win = (pending_write_win >= 0 && pending_write_win < window_count)
                                          ? &windows[pending_write_win] : 0;
                const struct gui_app *app = finished_win ? windows[pending_write_win].app : 0;
                pending_write = 0;
                pending_write_win = -1;
                if (finished_win && app && app->on_write_complete) {
                    app->on_write_complete((struct window *)finished_win, r == FS_STEP_DONE);
                }
                redraw_pending = 1;
            }
        }

        // Same per-frame polling for a pending read (Milestone 1 phase
        // 4, docs/roadmap.md) -- mirrors the pending_write block just
        // above exactly, including the pending_read_win accuracy
        // guarantee from bring_to_front()/close_window(). The extra
        // `total` out-param (fs_read_range_step()'s only difference
        // from fs_write_range_step()) is handed to on_read_complete
        // alongside success/failure so the app knows how many bytes it
        // actually got.
        if (pending_read) {
            uint32_t total = 0;
            enum fs_step_result r = fs_read_range_step(pending_read, &total);
            if (r != FS_STEP_PENDING) {
                void *finished_win = (pending_read_win >= 0 && pending_read_win < window_count)
                                          ? &windows[pending_read_win] : 0;
                const struct gui_app *app = finished_win ? windows[pending_read_win].app : 0;
                pending_read = 0;
                pending_read_win = -1;
                if (finished_win && app && app->on_read_complete) {
                    app->on_read_complete((struct window *)finished_win, r == FS_STEP_DONE, total);
                }
                redraw_pending = 1;
            }
        }

        // Per-frame poll for a pending process (Milestone 1 phase 4b,
        // docs/roadmap.md) -- same pending_proc_win accuracy guarantee
        // from bring_to_front()/close_window() as the two blocks above.
        // Unlike those, this poll isn't what makes the process's own
        // output appear -- that already streams straight into the
        // owning window's scrollback via vga_putc()'s active sink
        // (vga.h) the instant each SYS_WRITE syscall runs, driven by
        // scheduler_tick() on every timer interrupt regardless of
        // whether wm_run() happens to be looping right now. This poll
        // only detects completion (SCHED_POLL_EXITED) so the window can
        // be told, and redraws unconditionally while a process is
        // pending so output that streamed in between two mouse-move
        // events still shows up promptly instead of waiting for some
        // unrelated redraw to happen to fire.
        if (pending_proc) {
            redraw_pending = 1;
            int exit_code = -1;
            enum sched_poll_result r = scheduler_poll(pending_proc, &exit_code);
            if (r != SCHED_POLL_RUNNING) {
                void *finished_win = (pending_proc_win >= 0 && pending_proc_win < window_count)
                                          ? &windows[pending_proc_win] : 0;
                const struct gui_app *app = finished_win ? windows[pending_proc_win].app : 0;
                pending_proc = 0;
                pending_proc_win = -1;
                if (finished_win && app && app->on_process_exit) {
                    app->on_process_exit((struct window *)finished_win, exit_code);
                }
            }
        }

        // Mouse movement alone normally takes wm_render_cursor_move()'s
        // cheap path below (cursor sprite only, not a full scene
        // repaint) -- fine everywhere except an open Start menu, whose
        // hover highlight is derived fresh from (mx, my) every full
        // repaint (see start_menu.c's start_menu_draw()). Force the
        // full path while the menu's open so hovering a different row
        // actually updates the highlight instead of only refreshing on
        // the next unrelated redraw.
        if (mouse_moved && start_menu_open) redraw_pending = 1;

        // Closes the Start menu once a just-clicked row's brief flash
        // has shown long enough -- independent of clicks/movement, so
        // it still fires even if the mouse hasn't moved since the
        // click. See start_menu.c's own comment on this.
        start_menu_update();

        // Title-bar button hover/press feedback -- hover only matters
        // when the mouse actually moved (same cheap-path reasoning as
        // everything else in this loop); the press-commit check runs
        // every tick regardless, same as content_pressed below, since
        // it also needs to react to the button being released even if
        // the mouse hasn't moved since.
        if (mouse_moved) wm_update_title_hover(mx, my);
        wm_update_title_btn_press(mx, my, buttons);

        // Modal overlays track their own press/hover the same way, and
        // for the same reason: their buttons arm on press and commit on
        // release, so they need the live cursor every tick, not just the
        // button-down edge wm_handle_left_click() sees. Both are no-ops
        // while their dialog is closed.
        confirm_dialog_update_press(mx, my, buttons);
        file_picker_update_press(mx, my, buttons);
        if (!(buttons & 0x1)) {
            if (confirm_dialog_update_hover(mx, my)) redraw_pending = 1;
            if (file_picker_update_hover(mx, my)) redraw_pending = 1;
        }

        // Content hover, on the same only-when-the-mouse-moved cheap
        // path as the title-bar hover above. It also has to run once
        // after a button is RELEASED (the suppression inside it lifts
        // then, and the control under the cursor should light up again
        // without needing a further wiggle) -- hence the `|| !buttons`.
        if (mouse_moved || !(buttons & 0x1)) wm_update_content_hover(mx, my, buttons);

        wm_update_drag_resize(mx, my, buttons);
        desktop_update_drag(mx, my, buttons); // desktop icon drag, if one's in progress -- see desktop.h

        static uint64_t last_second = (uint64_t)-1;
        uint64_t ticks = pit_ticks();
        uint64_t this_second = ticks / 100;
        if (this_second != last_second) {
            last_second = this_second;
            tray_update_clock(); // also sets redraw_pending + damages the taskbar strip
        }

        // Esc used to always exit the window manager here -- replaced by
        // the Start menu's "Exit to shell" (see wm_system_actions above),
        // which is discoverable instead of a hidden key. Esc itself is
        // deliberately unclaimed at the WM level now, free for a future
        // per-window or modal use (e.g. canceling a confirm dialog)
        // instead of double-booking it as "exit everything".
        uint8_t key_mods = 0;
        int key = keyboard_try_getchar_mods(&key_mods);
        // A `gui key` from the debug console, if the real keyboard had
        // nothing -- deliberately second, so a human at the keyboard is
        // never pre-empted by a queued test keystroke.
        if (key == -1) {
            int injected = wm_debug_next_key_mods(&key_mods);
            if (injected) key = injected;
        }

        int wheel = mouse_get_wheel_delta();
        if (wheel == 0) wheel = wm_debug_next_wheel(); // `gui wheel`, same
                                                        // second-place rule as
                                                        // the injected key above

        if (key != -1 || wheel != 0) {
            // A modal file picker (e.g. Notepad's Save As...) captures
            // keyboard input first, same "most modal" priority
            // wm_handle_left_click() already gives confirm_dialog/
            // file_picker over ordinary window clicks -- see
            // file_picker.h. It has no wheel handling yet, so a wheel
            // event while it's open is just dropped rather than
            // reaching the window behind it.
            if (key != -1 && file_picker_handle_key(key)) {
                // consumed by the picker -- modal, its own rect isn't
                // reported as damage yet, falls back to a full-screen
                // repaint same as other dialogs (see wm_render.c's
                // damage-region comment)
            } else {
                int f = -1;
                for (int i = window_count - 1; i >= 0; i--) {
                    if (windows[i].state != WIN_MINIMIZED) { f = i; break; }
                }
                if (f >= 0 && key != -1 && !file_picker_open && wm_client_is_client_window(&windows[f])) {
                    // Focused window belongs to a ring-3 client: the
                    // key becomes a protocol message rather than a
                    // callback. Same focus rule either way -- who gets
                    // the key is the WM's decision, and it doesn't
                    // change because the recipient is a process.
                    wm_client_send_key(&windows[f], key, key_mods);
                    redraw_pending = 1;
                } else if (f >= 0 && key != -1 && !file_picker_open && windows[f].app && windows[f].app->on_key) {
                    windows[f].app->on_key(&windows[f], key, key_mods);
                }
                if (f >= 0 && wheel != 0 && !file_picker_open && windows[f].app && windows[f].app->on_wheel) {
                    windows[f].app->on_wheel(&windows[f], wheel);
                }
                // Typing/scrolling is the single most common redraw
                // trigger this loop sees besides mouse movement -- worth
                // reporting precisely rather than falling back to a full
                // repaint for every keystroke.
                if (f >= 0) wm_damage_rect(windows[f].x, windows[f].y, windows[f].w, windows[f].h);
            }
            redraw_pending = 1;
        }

        prev_mx = mx; prev_my = my; prev_buttons = buttons;

        // With double buffering there's no flicker to hide, so redraw as
        // soon as anything changes -- throttling here only added cursor lag.
        //
        // Two tiers: anything that actually changed the scene (a click,
        // a drag/resize in progress, a window opening/closing, a key or
        // wheel event delivered to an app, the once-a-second clock tick)
        // sets redraw_pending and gets a full repaint. Mouse movement
        // alone -- by far the most common event this loop sees -- takes
        // wm_render_cursor_move()'s cheap path instead: it doesn't touch
        // redraw_pending at all, so a plain full redraw still happens
        // exactly when it used to. See wm_render.c's dirty-rectangle
        // comments for why this split is worth having.
        if (redraw_pending) {
            redraw_pending = 0;
            wm_render_frame(mx, my);
        } else if (mouse_moved) {
            wm_render_cursor_move(mx, my);
        }
    }
}
