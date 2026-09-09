#ifndef WIN_SERVER_H
#define WIN_SERVER_H

#include <stdint.h>
#include "win_proto.h"

// Client windows: the kernel-side half of the windowing protocol.
//
// THE SPLIT, AND WHY IT FALLS HERE
// --------------------------------
// A client window is two things at once: a chunk of memory shared
// between a ring-3 process and whoever composites it, and a rectangle
// on screen with a title bar and a z-order. Those belong to different
// layers, and this header is where they meet.
//
// This file owns the MEMORY half -- window ids, the pixel buffers, the
// per-process mappings, ownership, and tearing all of it down when a
// client dies. Those are page-table and frame-allocator concerns, and
// `apps/` cannot reach them at all: kernel/include/kernel is not on its
// include path, deliberately (see kernel/include/README.md).
//
// Whoever registers below owns the PRESENTATION half -- where the
// window sits, what its title bar says, when it is composited, who has
// focus. Today that is the kernel-space window manager (userland/wm/);
// under Milestone 41's plan it can later be a ring-3 display server
// without this file changing, because everything crossing the boundary
// is already a typed message (abi/win_proto.h) rather than a call into
// WM internals.
//
// The dependency deliberately points WM -> kernel: syscall.c must not
// include userland/wm/wm.h (apps/ is a peer of the kernel here, not a
// library beneath it), so the WM registers itself as it starts. Same
// shape as kernel/include/kernel/display.h's display_driver registry,
// and for the same reason -- the implementation swaps, the callers
// don't notice.
//
// This header lives in api/ rather than kernel/ because userland/wm/ is the
// thing that implements it, and kernel/ is deliberately off apps/'s
// include path (see kernel/include/README.md). That is the documented
// promotion trigger -- a header starts in kernel/ and moves here once
// an app genuinely needs it -- rather than a hole in the boundary.

// **ONE SLOT IS LEFT, AND THE STRUCT IS A REMNANT.** This was the
// ring-0 presentation layer's interface -- eleven callbacks the kernel
// invoked as it served a client's window requests. It serves none: the
// requests reach the compositor over its own channel and the window
// table is gone (stage 6b), so ten of the eleven had no call site left.
// They are removed rather than kept "in case", because a slot nothing
// can reach is a slot a future reader will fill and wonder about.
//
// `debug_command` survives because the `gui` diagnostic channel is
// genuinely still the kernel's: a serial console types a command and
// the reply is chunked back through win_transport.h.
struct win_server_ops {
    // Run one `gui` diagnostic command and write its reply into `out`
    // (`cap` bytes including the NUL). Returns the number of bytes
    // written, or -1 if the subcommand was not recognised.
    //
    // OPTIONAL: a presentation layer with no diagnostics leaves it NULL
    // and the channel answers "no window manager", rather than this file
    // having to know what a `gui` command is. It does not -- the reply
    // is opaque text, and which subcommands exist is the WM's business.
    //
    // The WHOLE reply is produced in one call and chunked by the caller.
    // The alternative -- letting the WM stream chunks as it formats --
    // would make every `gui` command re-entrant with respect to the
    // transport, and these are dispatched from inside wm_run() itself.
    int (*debug_command)(const char *line, char *out, int cap);
};

// Registers the presentation layer. The WM calls this with its ops as
// it starts and with NULL as it exits, so a client request made while
// no desktop is running is refused rather than dispatched into a WM
// that isn't there.
void win_server_register(const struct win_server_ops *ops);

// Whether a presentation layer is registered. The normal state before
// `gui` is entered is "no".
//
// NOTE THE NARROWNESS, because it has bitten three times: this asks
// about a RING-0 layer only. The desktop is a ring-3 compositor and is
// therefore NOT one, so a machine with a perfectly good window server
// answers 0 here. Anything meaning "is there a window server at all"
// wants win_server_any() below.
int win_server_active(void);

// Whether there is a window server of EITHER kind -- a registered
// ring-0 presentation layer or a registered compositor. This is the
// predicate `win_server_request()` itself gates on, and the one a caller
// almost always means.
//
// It exists because three places open-coded it and two of them got it
// wrong by omitting the compositor half: two KTESTs guarded themselves
// with win_server_active() so they would skip while the desktop was up,
// and quietly stopped skipping the moment the desktop became a process.
// Nothing noticed until init started the desktop at boot and `make test`
// finally ran with one registered.
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

// The registered presentation layer, or NULL. For KTESTs, which swap in
// a stub and must put the live desktop's back -- the suite runs inside
// the LIVE kernel, so a test that leaves a stub registered takes the
// desktop's diagnostics down for the rest of the boot.
const struct win_server_ops *win_server_ops_current(void);

// Handles one client request against `pid`. `req` is a KERNEL copy --
// never the client's own page, which the client could change under us
// between validation and use. WIN_REQ_CREATE writes the new id back
// into req->window on success.
//
// Returns 1 on success, 0 if the request was refused (bad size, no free
// window, not this client's window, out of memory), -1 for an unknown
// request type or with no server registered.
int win_server_request(int pid, struct win_request_msg *req);

// Handles one diagnostic message -- WIN_REQ_DEBUG_CMD runs `msg->text`
// and answers with the first chunk of its reply; WIN_REQ_DEBUG_MORE
// answers with the next one. On return `msg` is a WIN_EV_DEBUG_OUT
// carrying `len` bytes and WIN_DEBUG_F_* flags.
//
// The reply is buffered HERE, between the WM that formatted it and the
// transport that carries it, because chunking is a property of the
// carriage and not of the diagnostic. A transport that could pass the
// whole reply in one message (a shared ring) fetches it in one call and
// never asks for a second chunk, with nothing on the WM side to change.
//
// Returns 1 on success, 0 if no presentation layer is registered or it
// offers no debug_command.
int win_server_debug(int pid, struct win_debug_msg *msg);

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

// --- cross-process buffer sharing (Milestone 41, stage 1) ------------
//
// A ring-3 window manager cannot composite what it cannot read, and a
// client's pixels live in that client's address space. These map a
// window's frames into a SECOND address space -- the compositor's --
// and, more importantly, revoke that mapping when the frames stop being
// the window's.
//
// **Revocation is the substance here, not mapping.** Mapping the same
// frames twice is three lines; what makes it safe is that a destroyed
// or resized window's mapping goes away in the same operation that
// frees the frames. A stale mapping leaves the compositor reading
// memory the allocator has handed to someone else, which shows up as a
// compositing GLITCH -- flickering garbage in one window -- and gets
// diagnosed as a drawing bug for as long as that takes.
//
// **Guarded, because a window buffer is private memory.** Only the
// registered compositor may ask, and only for a window that really
// exists. Mapping an arbitrary process's pages into another process on
// request is a hole, not a feature.
//
// These live in api/ next to the rest of this header rather than in a
// second one: `userland/wm/` is the intended caller from stage 4 onward,
// and splitting one subsystem's declarations across two headers to
// delay that by a stage would cost more than it protects.

// Registers (or clears, with pid 0) the compositor: which process may
// map other processes' windows, and the address space its mappings go
// into.
//
// The address space is passed EXPLICITLY rather than looked up from the
// scheduler at map time. Two reasons: the mapping then never depends on
// which process happens to be current when the request arrives (a
// batched or ring transport breaks that assumption -- the same argument
// win_server_ops makes about taking `pid`), and it makes the whole path
// reachable from a KTEST, which has no processes to look up.
//
// Returns 1 on success, 0 for a pid outside the supported range.
int win_server_set_compositor(int pid, uint64_t pml4);

// The registered compositor's pid, or 0 if none.
int win_server_compositor_pid(void);

// **THERE IS NO MAP/UNMAP PAIR ANY MORE.** A compositor opens a
// client's buffer object by NAME and maps it itself, so the kernel has
// no window mapping to make, revoke or poison -- see
// docs/winserver-ring3-design.md's stage 5b.

// THE KTEST STAND-INS ARE GONE. They made a window with no client
// behind it so a kernel test could exercise the table; there is no
// table. What they covered is the compositor's now, and is checked from
// ring 3 -- see win_server_test.c, which names the tools.

#endif
