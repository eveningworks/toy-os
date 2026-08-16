#ifndef WIN_PROTO_H
#define WIN_PROTO_H

#include <stdint.h>

// **TWP -- the Toy Window Protocol.** This file IS the protocol: the
// kernel<->client contract for "something happened to your window" and
// for everything a client can ask of its window. Shared by the kernel's
// dispatcher and by ring-3 clients, same as syscall_abi.h.
//
// Named because it is the piece meant to outlive its implementation.
// TWS (the Toy Window Server, kernel/proc/win_server.c +
// apps/wm/wm_client.c) is one implementation of it and is scheduled to
// become a ring-3 process; Toykit (userland/ui/) is the client library
// apps use instead of speaking it by hand. A protocol you can name is
// one you can version -- see docs/decisions.md.
//
// This is deliberately a MESSAGE FORMAT, not a set of syscall
// signatures, and that distinction is the whole architectural bet (see
// docs/roadmap.md's Milestone 41). The chosen shape is "kernel
// compositor now, userspace display server later": the modularity worth
// having comes from clients and the window server only ever talking
// through defined messages, never by calling into each other. Get that
// right and moving the server out of the kernel later is a transport
// swap; get it wrong -- by letting the syscall signature BE the
// protocol -- and every call site has to be rewritten instead.
//
// So: what a message says lives here and is expected to outlive how it
// is carried. Today it is carried by SYS_WAIT_EVENT/SYS_POLL_EVENT
// copying one struct at a time (see syscall_abi.h); the intended next
// carrier is a shared-memory ring the client maps once. Nothing in this
// header should need to change for that.

#define WIN_EV_NONE       0
#define WIN_EV_KEY        1 // a: key code (api/keyboard.h), mods: KEY_MOD_*
#define WIN_EV_MOUSE_MOVE 2 // a, b: position, window-relative
#define WIN_EV_MOUSE_DOWN 3 // a, b: position; mods: button bits
#define WIN_EV_MOUSE_UP   4 // a, b: position; mods: button bits
#define WIN_EV_CLOSE      5 // the server wants this window gone
#define WIN_EV_RESIZE     6 // a, b: PROPOSED content size -- a configure,
                            // not a command. See below.
#define WIN_EV_WHEEL      8 // a: notches, + = up/away, - = down/toward.
                            //
                            // A client had no way to receive scrolling
                            // at all before this: kernel-space apps got
                            // gui_app::on_wheel and clients got nothing,
                            // so Notepad drew a scrollbar it could never
                            // move and the wheel did nothing in any
                            // ring-3 window.
#define WIN_EV_PING       9 // a: an opaque serial the client must echo
                            // back in WIN_REQ_PONG, unchanged.
                            //
                            // LIVENESS, and the reason it is a protocol
                            // message rather than something the server
                            // can work out for itself: a client that has
                            // stopped pumping its event queue is
                            // indistinguishable, from the outside, from
                            // one that is simply idle. Both draw
                            // nothing and send nothing. So the server
                            // asks, and a client that cannot answer is
                            // wedged.
                            //
                            // This is xdg_shell's ping/pong and ICCCM's
                            // _NET_WM_PING, and it exists for the same
                            // reason both of those do: without it the
                            // only honest thing a compositor can say
                            // about an unresponsive window is nothing.
                            //
                            // A client never writes ping-handling code:
                            // uapp answers it inside the loop, which is
                            // exactly the property that made every
                            // callback optional (ui/uapp.h). An app
                            // wedged in its OWN callback therefore
                            // fails to answer, which is correct -- it
                            // really is not responding.
// --- raw input, for the registered compositor only --------------------
//
// Everything above is POST-routing: the server has already decided which
// window an event belongs to and made the coordinates window-relative. A
// compositor is the thing that makes that decision, so it needs the
// stream from BEFORE it -- screen coordinates, no focus, no hit-testing.
//
// These go only to the pid registered with WIN_REQ_SET_COMPOSITOR, and
// `window` is meaningless on all three (there is no window yet; that is
// the point).
//
// **The mouse is LEVEL STATE, not edges.** The kernel does not
// synthesise press/release events, because it does not have any: the
// PS/2 driver exposes an absolute clamped position plus a button
// bitmask, and today's WM derives edges by diffing against its own
// previous sample. Handing the compositor the same level state it would
// have read itself keeps one differ instead of two, and keeps this
// event honest about what the hardware actually reports. A compositor
// diffs it exactly as wm.c does.
//
// Sent only when something CHANGED (position or buttons). The WM loop
// runs on every timer tick, so an unconditional push would overflow a
// 32-deep queue within a fraction of a second and report constant drops
// while the user did nothing at all.
#define WIN_EV_RAW_MOUSE 10 // a, b: SCREEN position; mods: button bits
                            // (bit0 = left, bit1 = right), level state.
#define WIN_EV_RAW_KEY   11 // a: key code (api/keyboard.h), mods:
                            // KEY_MOD_*. Pre-focus: no window has been
                            // chosen yet.
#define WIN_EV_RAW_WHEEL 12 // a: notches, + = up/away, - = down/toward.
                            // Separate from RAW_MOUSE because the
                            // driver's wheel is a read-and-reset
                            // accumulator, not part of the level state.

#define WIN_EV_DEBUG_OUT 13 // One chunk of a `gui` command's reply. Rides
                            // struct win_debug_msg rather than struct
                            // win_event -- the text does not fit in 24
                            // bytes; see the diagnostic channel section
                            // below. Listed here so the event namespace
                            // stays one list.

#define WIN_EV_FOCUS      7 // a: 1 = this window gained keyboard focus,
                            // 0 = lost it.
                            //
                            // Sent because a client cannot otherwise
                            // tell: it sees keys only when focused, but
                            // "no keys have arrived" is indistinguishable
                            // from "the user is thinking". An app that
                            // draws a CARET has to know -- an unfocused
                            // window showing one claims to be taking
                            // input that is going somewhere else.

// Fixed 24-byte layout, no padding on x86-64, no pointers -- so the
// same bytes work unchanged whether they are copied out by a syscall or
// read straight out of a shared ring by the client.
struct win_event {
    uint32_t type;      // WIN_EV_*
    uint32_t window;    // which of this client's windows (0 until a
                        // client can own more than one -- see M41)
    int32_t  a;         // type-dependent, see the WIN_EV_* comments
    int32_t  b;
    uint32_t mods;      // KEY_MOD_* / button bits, per event type
    uint32_t reserved;  // must be 0; keeps the struct 8-byte aligned
                        // and leaves room to grow without a size change
};

// ---------------------------------------------------------------------
// Client -> server requests
// ---------------------------------------------------------------------
//
// The other direction of the protocol, and deliberately the same shape:
// one typed message, not one syscall per operation. That is what keeps
// the client/server boundary a protocol rather than an API -- add an
// operation and it is a new message type, carried by the same
// transport, with no new kernel entry point and nothing for a future
// userspace server to re-plumb.

#define WIN_REQ_CREATE  1 // a: width, b: height, c: x, d: y (screen).
                           // On success `window` is filled in with the
                           // new window's id and the client's buffer is
                           // mapped at win_buffer_vaddr(id).
#define WIN_REQ_PRESENT 2 // `window`: which one. The client has finished
                           // drawing into its buffer; composite it.
#define WIN_REQ_DESTROY 3 // `window`: which one. Closes it and unmaps
                           // the buffer.
#define WIN_REQ_TITLE   4 // `window`: which one; the title comes from
                           // the request's `text` field.
#define WIN_REQ_HINTS   6 // How this window should BEHAVE. a: WIN_HINT_*
                           // flags, b/c: minimum content w/h (0 = the
                           // server's floor). Sent once after create,
                           // and re-sendable if an app changes its mind.
                           //
                           // Behaviour is DECLARED, not inferred: the
                           // server does not guess "resizable" from a
                           // window's size or its title. Same principle
                           // as fs_ops.caps and display_driver's
                           // capability bits.
#define WIN_REQ_RESIZE  7 // a/b: requested content w/h. Reallocates this
                           // window's buffer and remaps it AT THE SAME
                           // VIRTUAL ADDRESS, so the client's pointer
                           // stays valid across the call -- see
                           // win_buffer_vaddr() below, which derives
                           // that address from the window id rather
                           // than returning it. On success a/b come
                           // back as the size actually granted.
                           //
                           // A client resizes ITSELF. The server never
                           // reallocates a buffer underneath a running
                           // client; it asks, with WIN_EV_RESIZE, and
                           // this is the answer. See that event.
#define WIN_REQ_PONG    8 // a: the serial from WIN_EV_PING, echoed back
                           // unchanged. The answer to a liveness check;
                           // see that event. Sent by the toolkit, not
                           // by application code.
#define WIN_REQ_SET_COMPOSITOR 9 // a: 1 = claim the compositor role,
                           // 0 = release it. No other fields.
                           //
                           // The way IN to win_server.h's compositor
                           // registration, which existed with nothing
                           // able to call it. A registered compositor
                           // may map other processes' window buffers
                           // and receives the raw input stream
                           // (WIN_EV_RAW_*) the WM consumes today.
                           //
                           // A MESSAGE rather than a syscall of its
                           // own, because that is this protocol's whole
                           // bet -- see the header comment. That costs
                           // one exception, made in two places and
                           // worth knowing about: every other request
                           // is refused outright when no presentation
                           // layer is registered (syscall.c's
                           // win_server_active() gate, and
                           // win_server_request()'s own !g_ops guard),
                           // and this one must work without one. In
                           // stage 4 the ring-3 WM IS the compositor,
                           // so there is no kernel-side presentation
                           // layer left to register first -- gating
                           // this behind one would make it permanently
                           // unreachable at exactly the point it
                           // matters.
                           //
                           // Claiming replaces any previous holder and
                           // revokes every mapping it held; dying
                           // releases it (win_server_client_gone()).
                           // There is deliberately no arbitration --
                           // last claimant wins, the same way
                           // display_register() lets the last driver
                           // claim the screen.
#define WIN_REQ_FONT    5 // No inputs. Maps the desktop's ACTIVE font
                           // read-only into the client at
                           // WIN_FONT_VADDR and fills in the metrics:
                           // a = glyph width, b = glyph height,
                           // c = glyph count, d = the byte offset of
                           // glyph 0 within the mapping (the data does
                           // not necessarily start on a page boundary,
                           // so glyph 0 lives at WIN_FONT_VADDR + d,
                           // not at WIN_FONT_VADDR).
                           // `window` is ignored -- a font belongs to
                           // the session, not to one window.
                           //
                           // WHY THE SERVER HANDS OVER THE FONT rather
                           // than each client carrying its own: the
                           // baked glyph data is ~11,800 lines of
                           // tables (kernel/drivers/font_ttf.c), so a
                           // copy per client is both large and, worse,
                           // free to drift from the desktop's -- a
                           // client would keep rendering at the old
                           // size after `font_size` changed. One
                           // read-only mapping of the kernel's own
                           // data keeps every client's text identical
                           // to the desktop's by construction.
                           //
                           // READ-ONLY is load-bearing: this maps
                           // pages out of the kernel image itself, so
                           // a writable mapping would let any client
                           // scribble on kernel .rodata.

#define WIN_REQ_CLOSE_PID  12 // a: the pid whose window(s) should be
                           // ASKED to close. Returns 1 if at least one
                           // window was asked, 0 if that pid has none.
                           //
                           // Task Manager's "End Task": the polite half
                           // of ending a process, as against SYS_KILL's
                           // immediate one. It goes through the server
                           // rather than being a syscall because the
                           // decision is the WINDOW MANAGER's -- it runs
                           // the same wm_request_close() the X button
                           // and Alt+F4 use, so a client may refuse it
                           // exactly as it may refuse those, and there
                           // is no fourth path that could drift from
                           // them (repeating that check is precisely how
                           // the context menu once drifted into seizing
                           // windows instead of asking).
                           //
                           // Unprivileged, like SYS_KILL and for the
                           // same reason -- and strictly weaker than it,
                           // since the target may decline.

#define WIN_REQ_DEBUG_CMD  10 // Run one `gui` diagnostic command. The
                           // command text and the reply both ride
                           // struct win_debug_msg, not this struct --
                           // see that struct for why.
#define WIN_REQ_DEBUG_MORE 11 // Fetch the next chunk of the reply the
                           // previous DEBUG_CMD started. No inputs.

// --- the diagnostic channel (Milestone 41, stage 3) -------------------
//
// The `gui` commands the GUI test tools drive the desktop with, carried
// as protocol messages instead of a direct call from the kernel's serial
// console into WM internals. All sixteen tools reach the WM this way, so
// the ~240 checks that prove the desktop works have to cross the
// transport before the WM itself can move to ring 3 (stage 4).
//
// **A message, not a side channel.** These are ordinary WIN_REQ_*/
// WIN_EV_* types on the one transport, so a ring-3 window server
// inherits the diagnostic path with nothing to re-plumb -- the same bet
// WIN_REQ_SET_COMPOSITOR made. A separate channel was considered (a
// diagnostic is not app-facing traffic) and rejected as a second
// mechanism to maintain and move.
//
// **Why its own struct rather than widening the two above.** A reply is
// text and runs to kilobytes -- `gui help` alone is ~1.8 KB and
// `gui windows --json` grows with the window count. struct win_event is
// a fixed 24 bytes and struct win_request_msg carries text[32], so
// neither can hold one; widening either would put a kilobyte-sized copy
// on the path of EVERY request, and WIN_REQ_PRESENT is the hot path --
// once per client frame. So the diagnostic pair carries its own payload
// and the hot path keeps its 56-byte message. Same fixed-layout, no
// pointer discipline as the other two, for the same reason: these bytes
// must work unchanged whether a syscall copies them or a client reads
// them straight out of a shared ring.
#define WIN_DEBUG_CMD_LEN 128 // longest command, including the NUL. The
                              // longest one any tool sends today is a
                              // `spawn` with arguments, ~40 bytes.
#define WIN_DEBUG_CHUNK   512 // reply bytes per message, excluding the
                              // NUL. Sized so a typical one-line answer
                              // fits in a single round trip while the
                              // struct stays well under a page.

#define WIN_DEBUG_F_MORE  0x01 // set on a reply when more chunks follow:
                               // ask again with WIN_REQ_DEBUG_MORE. The
                               // reply is NOT self-delimiting -- a chunk
                               // that exactly fills the buffer is
                               // indistinguishable from a truncated one
                               // otherwise, which is the trap a
                               // "read until short" convention sets.
#define WIN_DEBUG_F_UNKNOWN 0x02 // the WM did not recognise the
                               // subcommand; `text` holds nothing. Kept
                               // distinct from an empty reply, since a
                               // command that legitimately prints
                               // nothing is not an error.

struct win_debug_msg {
    uint32_t type;  // WIN_REQ_DEBUG_CMD / WIN_REQ_DEBUG_MORE going in,
                    // WIN_EV_DEBUG_OUT coming back
    uint32_t flags; // WIN_DEBUG_F_*, reply only
    uint32_t len;   // bytes valid in `text`, reply only
    uint32_t reserved; // must be 0; keeps the struct 8-byte aligned
    char text[WIN_DEBUG_CHUNK + 1]; // command in / reply chunk out,
                                    // always NUL-terminated
};

// --- window behaviour hints (WIN_REQ_HINTS's `a`) ---------------------
#define WIN_HINT_RESIZABLE 0x01 // the user may resize this window

// --- resize is a CONFIGURE/ACK HANDSHAKE ------------------------------
//
// The obvious implementation -- the server resizes the window when the
// user drags, and the client finds out afterwards -- is wrong here, and
// visibly so: the server composites client_w * client_h pixels into
// whatever content rectangle the chrome now describes, so the window
// would grow with the content stuck at the old size in one corner. The
// buffer belongs to the client; only the client can decide when it
// changes.
//
// So, following Wayland's xdg_toplevel configure/ack -- the same
// problem, solved the same way, for the same reason:
//
//   1. The user drags the resize grip. The WM tracks a proposed size
//      and draws a rubber-band outline; the window itself does not
//      change yet.
//   2. On release the WM clamps to the client's hinted minimum and
//      sends WIN_EV_RESIZE(w, h) -- a proposal.
//   3. The client answers with WIN_REQ_RESIZE. The server frees the old
//      frames, allocates new ones and maps them at the same virtual
//      address, so the client's buffer pointer survives.
//   4. The client redraws and presents. The WM adopts the new content
//      size when that present arrives.
//   5. If the server refuses (out of contiguous memory, over
//      WIN_CLIENT_MAX_*), the client keeps the size it had and the
//      window does not change. A refusal is a normal outcome, not an
//      error path.
//
// A client that ignores WIN_EV_RESIZE simply does not resize, which is
// the same politeness WIN_EV_CLOSE has: see "a client window's close
// button is a handshake, not a seizure" in docs/decisions.md.

// Longest window title a client may set, including the NUL. Matches the
// window manager's own WIN_TITLE_MAX -- a client that sends more gets
// it truncated at this boundary rather than refused, since a too-long
// title is a cosmetic problem and not worth failing a request over.
#define WIN_TITLE_LEN 32

// Largest client window, in pixels -- the 1280x720 mode boot.asm asks
// for, so a client can be dragged to fill the screen and the window
// manager's own screen-bounds clamp (wm_update_drag_resize()) becomes
// the effective limit rather than this one. That is the point: at 640x480
// a resize past the cap was refused SILENTLY, since a refusal is a normal
// protocol outcome and looks identical to a client that simply declined.
//
// Still bounded, because the server allocates and maps the whole buffer
// up front and does it CONTIGUOUSLY (see create_window()): at 4 bytes
// per pixel this is 900 frames from pmm_alloc_contiguous(), the most
// this kernel is willing to hand one window without a growable-mapping
// story. Growing past the display, or dropping the contiguity
// requirement so fragmentation can't refuse a resize, is docs/roadmap.md's
// growable client buffers item -- not this constant getting bigger again.
#define WIN_CLIENT_MAX_W 1280
#define WIN_CLIENT_MAX_H 720

// Same fixed-layout discipline as struct win_event: no pointers, so the
// identical bytes work whether copied by a syscall or read out of a
// shared ring.
struct win_request_msg {
    uint32_t type;   // WIN_REQ_*
    uint32_t window; // in for PRESENT/DESTROY/TITLE, out for CREATE
    int32_t  a, b, c, d;
    char     text[WIN_TITLE_LEN]; // WIN_REQ_TITLE only; NUL-terminated
};

// A client's window buffers are mapped at fixed, per-window addresses
// so a client never has to be told where its buffer landed -- it can
// compute the address from the window id the server handed back.
//
// Spaced WIN_BUFFER_STRIDE apart, which is comfortably more than the
// largest buffer WIN_CLIENT_MAX_W * WIN_CLIENT_MAX_H * 4 can need
// (3.52 MiB), so two windows' mappings can never overlap regardless of
// their sizes. The stride is the SECOND cap on window size and the one
// that is easy to miss -- it bounded windows to 2 MiB of pixels no
// matter what WIN_CLIENT_MAX_* said. Kept at a comfortable multiple
// rather than the tight fit, since virtual address space costs nothing
// here: nothing else in a client's address space lives above
// WIN_CLIENT_BASE (the stack tops out at 0x8000200000, the heap below
// that), so the whole region and the font above it are free to grow.
#define WIN_CLIENT_BASE   0x8001000000ULL
#define WIN_BUFFER_STRIDE 0x0000800000ULL // 8 MiB per window slot
#define WIN_CLIENT_MAX    4 // windows one client may hold at once

static inline uint64_t win_buffer_vaddr(uint32_t window) {
    return WIN_CLIENT_BASE + (uint64_t)window * WIN_BUFFER_STRIDE;
}

// Where WIN_REQ_FONT maps the shared glyph data. Placed above every
// window's buffer slot so the two regions can never collide however
// many windows a client opens.
#define WIN_FONT_VADDR (WIN_CLIENT_BASE + (uint64_t)WIN_CLIENT_MAX * WIN_BUFFER_STRIDE)

// --- the compositor's view of OTHER processes' windows ----------------
//
// A ring-3 compositor has to read the pixels of windows it does not own,
// which is the one thing the addresses above cannot express: they are
// per-CLIENT, and two clients both hold window 0. So a compositor sees
// every window in a region of its own, at an address DERIVED from the
// pair (owner pid, window id) exactly as a client's own buffer is
// derived from the id alone.
//
// **Derived rather than returned, for the same reason it was a good
// idea the first time.** A resize reallocates a window's frames and
// re-maps them AT THE SAME ADDRESS, so the compositor's pointer stays
// valid across a resize it did not initiate and never has to be told
// where the pixels moved. It also means the kernel can revoke a mapping
// without being told where it is -- it computes the address the same
// way -- which is what makes "the mapping is gone after the client
// dies" checkable rather than a matter of bookkeeping the two sides
// might disagree about.
//
// Placed well above the font so the three regions cannot collide: a
// client's own buffers end at WIN_FONT_VADDR, and this starts far
// enough above that the whole compositor region (MAX_PROCS x
// WIN_CLIENT_MAX slots) fits underneath the next round address. Virtual
// space costs nothing here.
#define WIN_COMPOSITOR_BASE 0x8010000000ULL

// How many processes' windows the region has room for. Must be >=
// SCHED_MAX_PROCS (api/scheduler.h) -- win_server.c static_asserts
// exactly that, since this header is ABI and cannot include a kernel
// one. The server refuses a pid outside it rather than computing an
// address that overlaps someone else's.
//
// Costs nothing but virtual address space: 64 x WIN_CLIENT_MAX x 8 MiB
// is 2 GiB of vaddr in a region with nothing above it.
#define WIN_COMPOSITOR_MAX_PIDS 64

static inline uint64_t win_compositor_vaddr(int pid, uint32_t window) {
    return WIN_COMPOSITOR_BASE
         + ((uint64_t)(pid - 1) * WIN_CLIENT_MAX + (uint64_t)window)
           * WIN_BUFFER_STRIDE;
}

// Glyph layout in that mapping, so a client can index it without being
// told anything beyond the metrics WIN_REQ_FONT returns: glyphs are
// stored back to back, each `h` rows of `w` bytes, row-major, one byte
// of coverage per pixel (0 = background, 255 = fully ink). Glyph 0 is
// ASCII 32 (space) and they run contiguously from there.
#define WIN_FONT_FIRST_CHAR 32

static inline uint64_t win_glyph_offset(uint32_t index, int w, int h) {
    return (uint64_t)index * (uint64_t)w * (uint64_t)h;
}

// How many events the server will hold for one client before it starts
// dropping the OLDEST. Dropping the oldest rather than the newest is
// deliberate: for input, the most recent state is the one that matters,
// and a client that has fallen far enough behind to overflow is better
// served by current events than by a backlog it will never catch up on.
#define WIN_EVENT_QUEUE_MAX 32

#endif
