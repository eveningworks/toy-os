#ifndef UAPP_INTERNAL_H
#define UAPP_INTERNAL_H

// uapp's units share this, and nothing else includes it -- an app sees
// ui/uapp.h. Split by concern, as userland/wm/ is:
//
//   uapp.c          the app: lifecycle, the event pump, the main window's
//                   events and frame, the compositor channel
//   uapp_window.c   ONE TOPLEVEL: the input routing and painting the main
//                   window and a dialog window share, and dialog windows
//   uapp_surface.c  a surface's buffers, presenting them, popup surfaces
//   uapp_log.c      the layout log and the widget map `gui probe` reads
//
// EVERYTHING DECLARED HERE IS HIDDEN: it crosses files inside libuapp and
// is never exported, so an app's own `flush` or `g_surf` cannot collide
// with -- or interpose on -- the toolkit's.

#include "rt/sys.h"   // TWP messages, sys_win_request()
#include "ui/uui_anim.h" // a frame while something animates
#include "kpath.h"    // k_path_dirname/_basename -- a drag from another window
#include "ui/uapp.h"
#include <string.h>
#include <stdio.h>
#include "lib/uchan.h"
#include "lib/uwmchan.h"
#include "lib/uregion.h"    // a present's damage, measured
#include "ui/uui_route.h"
#include "ui/uui_popup.h"   // the popup-surface provider, installed at open
#include "ui/uui_focus.h"   // desc.focus -- keyboard focus ring
#include "ui/uui_button.h"  // a lone button commits through on_action
#include "ui/uui_dialog.h"  // uapp_question_open()
#include "ui/uui_editmenu.h" // the right-click menu every text field gets
#include "ui/ulog.h"        // uapp_log_layout()
#include "setting_abi.h" // desktop.layout_log -- the gate below
#include "lib/usetting.h" // ...and the MERGED registry that can see it
#include <stdio.h>      // snprintf/vsnprintf, one layout line at a time
#include <stdarg.h>
#include "ui/utheme.h"
#include "ui/uui_caret.h"
#include "lib/uappentry.h" // uapp_display_name()
#include "lib/uclip.h"   // clip_poll() -- the clipboard is shared memory now
#include <locale.h>
#include <time.h>       // tzset()


#define UAPP_HIDDEN __attribute__((visibility("hidden")))

#define UAPP_CALL_TIMEOUT_MS 1000

// THE CLIENT'S OWN WINDOW MEMORY. A buffer is a named shm object this
// process creates and GRANTS to the compositor, which opens the name
// itself -- the kernel neither allocates it nor maps it. The name
// identifies the slot; the object in it is replaced on a resize, and
// the old one stays alive under whoever still maps it until they let go
// (docs/winserver-ring3-design.md, stage 5).
#define UAPP_BUFS WIN_CLIENT_BUFS

// ONE SURFACE THE COMPOSITOR SHOWS: the toplevel window in slot 0, a
// popup (ui/uui_popup.h) in any other. Indexed by SLOT, which is also
// the `window` every event names and the number in the buffer's shm
// name -- one number, three uses, nothing to keep in step.
struct uapp_surf {
    int used;
    void *px[UAPP_BUFS];
    uint64_t px_bytes[UAPP_BUFS];
    // WHAT EACH BUFFER IS CURRENTLY SIZED FOR. **NOT derivable from
    // px_bytes**, which is page-rounded: a one-pixel width change usually
    // leaves the rounded length identical, so a comparison on bytes
    // reports a real resize as "nothing to do" -- and the server then
    // holds the old width while the client draws at the new one, which
    // the compositor blits as a diagonal shear.
    int px_w[UAPP_BUFS], px_h[UAPP_BUFS];
    // WHICH OBJECT IS BEHIND EACH BUFFER'S NAME. The name is the slot and
    // never changes; the object under it is replaced on every resize,
    // and this is how the compositor knows to re-open. **THE CREATOR
    // COUNTS IT** -- the kernel used to, from a message the client had
    // to remember to send, which is one record too many for a fact only
    // this side can observe.
    uint32_t px_gen[UAPP_BUFS];
    // WHICH BUFFER THE COMPOSITOR IS READING. The surface handed out
    // always points at the OTHER one -- see present(). 0 until the first
    // present, and 0 forever for a single-buffered surface.
    int front;
    // WHICH BUFFERS THE COMPOSITOR MAY STILL BE READING: presented and
    // not yet handed back by WIN_EV_BUF_RELEASE (wl_buffer.release).
    // Drawing into one of those put half-painted frames on screen. `back`
    // is the one drawn into, chosen when a frame starts (surf_back()),
    // -1 between frames -- choosing at present time would always find
    // the previous front still busy and take the third buffer.
    int busy[UAPP_BUFS];
    unsigned long long busy_seq[UAPP_BUFS];  // present order
    int back;
    unsigned long long stall_ms;  // when no buffer was free; 0 otherwise
    int w, h;
    struct ugfx_surface surface;  // the back buffer, sized w x h
    int dirty;                    // a popup: drawn into since its last present
    // A POPUP'S PLACE relative to the toplevel's content origin, from
    // the compositor's reply -- what turns popup-local input back into
    // the coordinates every widget hit-tests in (ui/uui_popup.h).
    int ox, oy;
    // WHICH WINDOW THIS POPUP BELONGS TO. A dialog window is its own
    // toplevel with its own router (uapp_window_open), so a dropdown
    // inside one anchors its popup to THAT surface and its input must
    // go back to THAT router -- routing it to the main window's would
    // hit-test the press against a different window's widgets.
    uint32_t parent;
    void (*done)(void *owner);    // the widget to tell when the compositor dismisses it
    void *owner;
};

// ONE TOPLEVEL -- the main window, or a dialog window (uapp_window_open):
// what both ARE, so a frame and an input event take ONE path whichever
// it is. Two paths drifted: popups were presented on the main window's
// frame only, and a dropdown in a dialog over an idle app never showed.
// Qt's QDialog is a top-level QWidget like the main window; GTK keeps a
// list of equal GtkWindows. What differs -- the main window's draw
// hooks, its button group, drag and drop -- wraps these, never copies.
struct uapp_top;
struct uapp_top_ops {
    // A widget did something: on_action for a lone button's commit, else
    // on_widget (tell_app's rule, which both kinds of window keep).
    void (*tell)(struct uapp_top *t, int id, int reason, int committed);
    // A key, after the widgets have had it (on_key).
    void (*key)(struct uapp_top *t, int key, unsigned mods);
};
struct uapp_top {
    uint32_t slot;              // the window its events name; a closed dialog's is 0
    struct uui_router router;
    struct uui_focus *focus;    // the app's focus ring, or NULL
    int dirty;                  // something asked for a repaint since the last present
    // The last cursor position seen, because a WHEEL event carries
    // notches and no coordinates -- and "which widget is under the
    // cursor" is the only sane answer to where a wheel goes.
    int mouse_x, mouse_y;
    // The WIN_CURSOR_* last named, so a repeat is dropped -- motion
    // names one on every move.
    int cursor;
    int hears;                  // the app has an on_widget: a focused key is reported
    const struct uapp_top_ops *ops;
};
extern const struct uapp_top_ops APP_OPS, WIN_OPS;   // uapp_window.c

struct uapp {
    struct uapp_top top;        // FIRST: uapp_of() casts back from it
    const struct uapp_desc *desc;
    uint32_t window;
    int w, h;
    // The TOPLEVEL's back buffer -- a copy of TOPLEVEL->surface kept
    // here because every draw callback is handed `&a->surface` and the
    // two are refreshed together in present() and uapp_resize().
    struct ugfx_surface surface;
    int shown;    // presented at least once, so the compositor maps it
    // THE FIRST FRAME HELD for a fullscreen request made before it, until
    // the compositor's size proposal (or this deadline, in ms). xdg-shell's
    // initial configure: otherwise the window appears at its desc size for
    // a frame and then jumps, which a screensaver shows as a black flash.
    unsigned long long hold_until_ms;
    int focused;  // keyboard focus, per WIN_EV_FOCUS
    int running;
    int status;
    int timer_armed; // TWS accepted a WIN_REQ_TIMER, so on_tick arrives
                     // as an event and the loop can block
    int poll_paused; // uapp_poll_pause(): park in the blocking wait instead
    unsigned tick_ms; // on_tick's interval; 0 for UAPP_POLL or no on_tick
    int tick_local;   // TWS declined the timer: uapp_run() keeps the time
    uint64_t tick_due_ms;

    // Pointer routing (ui/uui_route.h) is top.router: empty unless the
    // app declared widgets, so an app that does its own hit-testing is
    // untouched. uapp_set_cursor() drops a no-op through top.cursor.
    int cursor_before_busy;
    // While a motion is being dispatched: the cursor it has settled on so
    // far, sent once when it ends. -1 when not in a motion. Without it the
    // tree's arrow and an app's I-beam over the same spot were BOTH sent
    // on every move.
    int motion_cursor;
    int fullscreen;    // as last asked for -- the compositor's proposal follows
    // The lease (WIN_EV_SCANOUT): the toplevel draws into the display's
    // buffer at WIN_FB_VADDR + lease_back * WIN_FB_BUFFER_STRIDE and
    // presents with WIN_REQ_FB_PRESENT, which answers the next back.
    int lease_on;
    uint32_t lease_pitch;
    int lease_count, lease_back;
};


struct uapp_window {
    struct uapp_top top;    // FIRST: window_of() casts back from it; top.slot 0 = closed
    struct uapp *app;
    struct uapp_window_desc desc;
    char title[WIN_TITLE_LEN];
};

// --- uapp.c ----------------------------------------------------------------
UAPP_HIDDEN extern struct uapp g_app;
UAPP_HIDDEN extern struct uchan_client g_wmchan;
UAPP_HIDDEN int  wmchan(void);
UAPP_HIDDEN int  comp_pid(void);
UAPP_HIDDEN int  wmchan_send(uint32_t type, uint32_t window, int aa, int bb, int cc, const char *text);
UAPP_HIDDEN int  wmchan_call(uint32_t type, uint32_t window, int aa, int bb, int cc,
                             const char *text, int fail);
UAPP_HIDDEN int  wmchan_send_damage(uint32_t type, uint32_t window, int aa, int bb, int cc,
                                    const char *text, const struct win_damage *dmg);
UAPP_HIDDEN void clamp_to_screen(int *w, int *h);

// --- uapp_surface.c ----------------------------------------------------------
UAPP_HIDDEN extern struct uapp_surf g_surf[WIN_CLIENT_MAX];
#define TOPLEVEL (&g_surf[0])
UAPP_HIDDEN extern uint32_t g_popup_parent;
UAPP_HIDDEN extern uint32_t g_press_slot;
UAPP_HIDDEN extern struct win_event g_press_up;
UAPP_HIDDEN extern int g_release_owed;
UAPP_HIDDEN extern const struct uui_popup_ops g_popup_ops;
UAPP_HIDDEN int  slot_of(const struct uapp_surf *s);
UAPP_HIDDEN int  buf_make(struct uapp_surf *s, int buf, int w, int h);
UAPP_HIDDEN int  bufs_create(struct uapp_surf *s, int w, int h);
UAPP_HIDDEN void bufs_release(struct uapp_surf *s);
UAPP_HIDDEN struct ugfx_surface *surf_back(struct uapp_surf *s);
UAPP_HIDDEN int  surf_present(struct uapp_surf *s);
UAPP_HIDDEN void present_popups(uint32_t parent);
UAPP_HIDDEN void popup_close(void *ctx, int id);
UAPP_HIDDEN void popup_dismissed(int id);

// --- uapp_window.c -----------------------------------------------------------
UAPP_HIDDEN extern struct uapp_window g_dlg[WIN_CLIENT_MAX];
UAPP_HIDDEN struct uapp_window *dlg_for(uint32_t slot);
UAPP_HIDDEN void dlg_flush(struct uapp_window *w);
UAPP_HIDDEN void dlg_dispatch(struct uapp_window *w, const struct win_event *ev);
UAPP_HIDDEN void top_paint(struct uapp_top *t, struct ugfx_surface *s, struct uui_layout *layout,
                           struct uapp *a);
UAPP_HIDDEN void top_key(struct uapp_top *t, const struct win_event *ev);
UAPP_HIDDEN void top_press(struct uapp_top *t, int x, int y, unsigned kmods);
UAPP_HIDDEN void top_motion(struct uapp_top *t, int x, int y, unsigned held, unsigned kmods);
UAPP_HIDDEN void top_release(struct uapp_top *t, int x, int y);
UAPP_HIDDEN int  top_secondary(struct uapp_top *t, int x, int y, int up);
UAPP_HIDDEN int  top_routes(const struct uapp_top *t);
UAPP_HIDDEN int  top_wheel(struct uapp_top *t, int notches);
UAPP_HIDDEN void tell_app(struct uapp *a, int id, int reason, int committed);
UAPP_HIDDEN int  ids_unique(const struct uui_router *r, const char *who);
UAPP_HIDDEN int  buttons_heard(const struct uui_router *r, int has_on_action, const char *who);

// --- uapp_log.c --------------------------------------------------------------
UAPP_HIDDEN extern int g_log_seen_n;
UAPP_HIDDEN int  layout_log_enabled(void);
UAPP_HIDDEN void layout_log_flush(int src);
UAPP_HIDDEN void log_items(const char *prefix, struct uui_item *items, int count);
UAPP_HIDDEN void wmap_sync(struct uapp *a);

#endif
