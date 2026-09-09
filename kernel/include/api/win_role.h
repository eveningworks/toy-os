#ifndef WIN_SERVER_H
#define WIN_SERVER_H

#include <stdint.h>
#include "win_proto.h"

// The windowing protocol, kernel side.
//
// **THIS FILE KNOWS OF NO WINDOW.** What is left to ring 0 is the
// compositor ROLE, the framebuffer grant keyed on it, the font mapped
// into a client, the per-process event queue and the `gui` diagnostic
// channel. A window's memory is the client's own shm object and its
// geometry is the compositor's; see docs/winserver-ring3-design.md.
//
// This header lives in api/ rather than kernel/ because userland/wm/ is the
// thing that drives it, and kernel/ is deliberately off apps/'s
// include path (see kernel/include/README.md). That is the documented
// promotion trigger -- a header starts in kernel/ and moves here once
// an app genuinely needs it -- rather than a hole in the boundary.

// Whether a window server holds the role. There is one kind -- a ring-3
// compositor -- so this is `win_server_compositor_pid() != 0` under a
// name that says what a caller means by it (vga.c: is anything else
// painting the screen?).
//
// It used to cover a second kind, a registered ring-0 presentation
// layer, and the pair was a trap: two KTESTs guarded themselves with the
// narrow predicate and quietly stopped skipping when the desktop became
// a process. The ring-0 layer is gone and so is the narrow predicate.
int win_server_any(void);


// Is the hardware cursor plane armed (compositor sent
// WIN_FB_CURSOR_SHOW)? Asked by win_input.c on every pointer event --
// the one caller the arm exists for.
int win_server_hw_cursor_armed(void);

// Broadcasts WIN_EV_FONT to every window: the active face or size has
// changed and every client's cached metrics are stale. Called from
// font_config.c, which is the one place a font change is applied for the
// machine as a whole -- see WIN_EV_FONT in abi/win_proto.h.
void win_server_font_changed(void);
// The screen's size changed (WIN_EV_SCREEN, a = w, b = h). One caller:
// screen_set_mode().
void win_server_screen_changed(int w, int h);

// Handles one client request against `pid`. `req` is a KERNEL copy --
// never the client's own page, which the client could change under us
// between validation and use. WIN_REQ_CREATE writes the new id back
// into req->window on success.
//
// Returns 1 on success, 0 if the request was refused, -1 for an unknown
// request type or with no compositor holding the role. Called straight
// from SYS_WIN_REQUEST; there is no carriage in between.
int win_server_request(int pid, struct win_request_msg *req);


// One event to every WINDOWING CLIENT -- every process that has waited
// for a window event (win_events_is_client). `window` is 0 on every
// copy: what rides this (WIN_EV_FONT, WIN_EV_SCREEN) is about the
// session, never about one window.
void win_server_broadcast(uint32_t type, int32_t a, int32_t b,
                           uint32_t mods);

// The compositor role's teardown, when `pid` dies. There are no windows
// to destroy here any more -- a client's windows belong to the
// compositor, which learns of the death from its own channel scan.
void win_server_client_gone(int pid);

// --- the compositor role -------------------------------------------
//
// **THERE IS NO MAP/UNMAP PAIR.** A compositor opens a client's buffer
// object by NAME and maps it itself, so the kernel has no window
// mapping to make, revoke or poison -- see
// docs/winserver-ring3-design.md's stage 5b. What the role still gates
// is the framebuffer grant, the raw input stream and the `gui` channel.

// Registers (or clears, with pid 0) the compositor: which process
// receives raw input and may be granted the framebuffer.
//
// The address space is passed EXPLICITLY rather than looked up from the
// scheduler. The grant then never depends on which process happens to be
// current when the request arrives, and the path is reachable from a
// KTEST, which has no processes to look up.
//
// Returns 1 on success, 0 for a pid outside the supported range.
int win_server_set_compositor(int pid, uint64_t pml4);

// The registered compositor's pid, or 0 if none.
int win_server_compositor_pid(void);

// THE KTEST STAND-INS ARE GONE. They made a window with no client
// behind it so a kernel test could exercise the table; there is no
// table. What they covered is the compositor's now, and is checked from
// ring 3 -- see win_server_test.c, which names the tools.

#endif
