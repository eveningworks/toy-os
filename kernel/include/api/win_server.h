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

// Presentation callbacks. Every one takes `pid` explicitly rather than
// consulting the scheduler, so a server never has to assume it is being
// called in the requesting process's own context -- a batched
// shared-ring transport would break that assumption.
struct win_server_ops {
    // A client window was created and its buffer mapped. `buf` is a
    // kernel-visible pointer to `w` * `h` pixels, 32bpp, no padding --
    // the same memory the client sees at win_buffer_vaddr(id).
    // Return 1 to accept it, 0 to refuse (no free slot in the window
    // list); on 0 the caller frees the buffer and the request fails.
    //
    // `app_id` is the client's own name for what this window IS (see
    // WIN_REQ_ACTIVATE), already truncated to fit WIN_APP_ID_LEN, and
    // "" when the client did not give one. Passed HERE rather than
    // through a slot of its own so a window is never briefly visible
    // without it -- the gap is exactly long enough for a second copy of
    // the same program to look for its twin and miss.
    int (*window_created)(int pid, uint32_t id, uint32_t *buf,
                           int w, int h, int x, int y,
                           const char *app_id);

    // The client finished drawing into `id`'s buffer.
    void (*window_present)(int pid, uint32_t id);

    // `id` is going away -- drop it from the window list. Called both
    // for an explicit WIN_REQ_DESTROY and for every window still open
    // when a client dies. `buf` stays valid until this returns.
    void (*window_destroyed)(int pid, uint32_t id);

    // Set `id`'s title (already truncated to fit WIN_TITLE_LEN).
    void (*window_title)(int pid, uint32_t id, const char *title);

    // The client declared how this window should behave --
    // WIN_HINT_* flags plus a minimum content size. Behaviour is
    // DECLARED, never inferred (see abi/win_proto.h).
    void (*window_hints)(int pid, uint32_t id, unsigned flags,
                          int min_w, int min_h);

    // `id`'s buffer has been reallocated at `w` x `h`; `buf` is the new
    // kernel-visible pointer and the old one is already freed. The
    // client asked for this -- see the configure/ack handshake in
    // abi/win_proto.h -- so the presentation layer is being told, not
    // asked.
    void (*window_resized)(int pid, uint32_t id, uint32_t *buf, int w, int h);

    // The client answered a liveness ping with this serial. OPTIONAL,
    // like every slot here -- a presentation layer that does not care
    // about liveness leaves it NULL and clients simply never get
    // pinged, because nothing asks.
    //
    // Note which side owns what: win_server relays the pong (it owns
    // the protocol), and the WM decides what a missing one MEANS (it
    // owns presentation -- the title, the dialog, the policy). The
    // timeout is not in here for that reason.
    void (*window_pong)(int pid, uint32_t id, uint32_t serial);

    // Run one `gui` diagnostic command and write its reply into `out`
    // (`cap` bytes including the NUL). Returns the number of bytes
    // written, or -1 if the subcommand was not recognised.
    //
    // OPTIONAL, like every slot here: a presentation layer with no
    // diagnostics leaves it NULL and the channel answers "no window
    // manager", rather than this file having to know what a `gui`
    // command is. It does not -- the reply is opaque text, and which
    // subcommands exist is entirely the WM's business.
    //
    // The WHOLE reply is produced in one call and chunked by the caller.
    // The alternative -- letting the WM stream chunks as it formats --
    // would make every `gui` command re-entrant with respect to the
    // transport, and these are dispatched from inside wm_run() itself.
    int (*debug_command)(const char *line, char *out, int cap);

    // Ask every window belonging to `pid` to close, through whatever
    // path the presentation layer already uses for its own close button
    // -- so a client may refuse, and there is no second close policy.
    // Returns how many windows were asked. OPTIONAL like every slot.
    int (*close_pid)(int pid);

    // Raise the window carrying `app_id` -- un-minimize it, bring it to
    // the front and focus it -- and return 1. Return 0 if no window has
    // that id. OPTIONAL like every slot here; a presentation layer that
    // leaves it NULL simply answers "nobody there", which degrades to
    // today's behaviour (every launch opens a new copy) rather than to
    // an error.
    //
    // The MATCH is here and the POLICY is in the client: this says
    // nothing about whether a second copy may run, only where the first
    // one is. See WIN_REQ_ACTIVATE in abi/win_proto.h.
    int (*window_activate)(const char *app_id);

    // Arm (or, with ms == 0, cancel) this window's repeating timer.
    // OPTIONAL like every slot: a presentation layer without one simply
    // never delivers WIN_EV_TIMER, and a client that asked for one waits
    // forever -- which is why Toykit falls back to polling rather than
    // assuming the event will come.
    //
    // The INTERVAL lives with the presentation layer because that is
    // what has a frame loop to check it against; win_server owns no
    // clock of its own.
    void (*window_timer)(int pid, uint32_t id, unsigned ms);
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

// One event to every window AND to the compositor -- see the definition
// for why the compositor needs saying separately.
void win_server_broadcast(uint32_t type, int32_t a, int32_t b,
                           uint32_t mods);

// Destroys every window `pid` still owns, freeing and unmapping their
// buffers. Called from process teardown -- a dead client's windows must
// leave the screen immediately, or they sit there drawing stale pixels
// and answering no input.
void win_server_client_gone(int pid);

// How many windows `pid` currently owns. For tests and `gui state`.
int win_server_window_count(int pid);

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
// Registering a different compositor, or clearing it, drops every
// mapping the previous one held.
int win_server_set_compositor(int pid, uint64_t pml4);

// The registered compositor's pid, or 0 if none.
int win_server_compositor_pid(void);

// Maps `owner_pid`'s window `id` into the compositor's address space.
// `requester_pid` must BE the registered compositor -- that check is the
// access control, and it is why this takes a requester at all.
//
// Idempotent: mapping a window that is already mapped succeeds and
// reports the same address, so a compositor may ask again after a
// resize without unmapping first (it does not need to -- see below --
// but asking twice must not be an error).
//
// On success `*out_vaddr` is win_compositor_vaddr(owner_pid, id), which
// the caller could have computed itself; it is returned so the address
// has exactly one definition at the call site rather than two.
// Returns 1 on success, 0 if refused.
int win_server_map_to_compositor(int requester_pid, int owner_pid, uint32_t id,
                                  uint64_t *out_vaddr);

// Drops that mapping. Returns 1 if one was removed, 0 if there was
// nothing mapped (not an error -- the same contract
// vmm_unmap_user_page() uses).
int win_server_unmap_from_compositor(int requester_pid, int owner_pid, uint32_t id);

// Whether `owner_pid`'s window `id` is mapped into the compositor right
// now. For tests and for `gui state`; a compositor knows its own state.
int win_server_is_mapped_to_compositor(int owner_pid, uint32_t id);

// Creates a window for `pid` in the address space `pml4`, with no live
// process and without going through the protocol.
//
// **For KTESTs.** The mapping and revocation paths above are otherwise
// reachable only by spawning a real client and driving TWP at it, which
// a test running inside the kernel cannot do -- and the property most
// worth testing (a destroyed window's mapping is gone) is exactly the
// one that is invisible from userland. Returns 1 and fills `*out_id` on
// success.
//
// Not a back door around the guard: everything it produces is an
// ordinary window, subject to the same ownership checks as any other.
int win_server_create_raw(int pid, uint64_t pml4, int w, int h, uint32_t *out_id);

// Destroys a window created by win_server_create_raw(). Same teardown
// the protocol's WIN_REQ_DESTROY performs, addressable from a test.
int win_server_destroy_raw(int pid, uint32_t id);

// Resizes one, running the same reallocate-and-remap the protocol's
// WIN_REQ_RESIZE runs. Separate from the request path for one blunt
// reason: win_server_request() refuses EVERYTHING when no presentation
// layer is registered, and a `ktest` run has no desktop -- so a test
// driving resize through the protocol tests only that refusal.
int win_server_resize_raw(int pid, uint32_t id, int w, int h);

#endif
