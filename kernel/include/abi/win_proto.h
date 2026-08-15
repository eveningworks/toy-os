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
#define WIN_EV_RESIZE     6 // a, b: new content size

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

// Longest window title a client may set, including the NUL. Matches the
// window manager's own WIN_TITLE_MAX -- a client that sends more gets
// it truncated at this boundary rather than refused, since a too-long
// title is a cosmetic problem and not worth failing a request over.
#define WIN_TITLE_LEN 32

// Largest client window, in pixels. Bounded because the server
// allocates and maps the whole buffer up front: at 4 bytes per pixel
// this is 300 pages, which is the most this kernel is willing to hand
// one window without a growable-mapping story.
#define WIN_CLIENT_MAX_W 640
#define WIN_CLIENT_MAX_H 480

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
// (1.17 MiB), so two windows' mappings can never overlap regardless of
// their sizes.
#define WIN_CLIENT_BASE   0x8001000000ULL
#define WIN_BUFFER_STRIDE 0x0000200000ULL // 2 MiB per window slot
#define WIN_CLIENT_MAX    4 // windows one client may hold at once

static inline uint64_t win_buffer_vaddr(uint32_t window) {
    return WIN_CLIENT_BASE + (uint64_t)window * WIN_BUFFER_STRIDE;
}

// Where WIN_REQ_FONT maps the shared glyph data. Placed above every
// window's buffer slot so the two regions can never collide however
// many windows a client opens.
#define WIN_FONT_VADDR (WIN_CLIENT_BASE + (uint64_t)WIN_CLIENT_MAX * WIN_BUFFER_STRIDE)

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
