#ifndef WIN_PROTO_H
#define WIN_PROTO_H

#include <stdint.h>

// The windowing protocol's event format -- the kernel<->client contract
// for "something happened to your window". Shared by the kernel's
// dispatcher and by ring-3 clients, same as syscall_abi.h.
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

// How many events the server will hold for one client before it starts
// dropping the OLDEST. Dropping the oldest rather than the newest is
// deliberate: for input, the most recent state is the one that matters,
// and a client that has fallen far enough behind to overflow is better
// served by current events than by a backlog it will never catch up on.
#define WIN_EVENT_QUEUE_MAX 32

#endif
