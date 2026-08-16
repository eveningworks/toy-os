#ifndef WIN_TRANSPORT_H
#define WIN_TRANSPORT_H

#include "win_proto.h"

// How a TWP message gets from a client to the window server.
//
// Milestone 41, stage 3. Everything crossing the client/server boundary
// was already a typed message (abi/win_proto.h); what was still hardwired
// was the CARRIAGE -- SYS_WIN_REQUEST reached win_server_request() by a
// direct call, and the kernel's serial console reached the WM by an even
// more direct one (debug_console.c called wm_debug_dispatch() straight
// into WM internals). Stage 4 moves the WM into ring 3, at which point
// both of those have to become something that crosses an address space.
// This is the seam that makes that a swap rather than a rewrite.
//
// Same one-struct-of-function-pointers registry as display_driver and
// the VFS backend probe, and for the same reason: the implementation
// swaps, the callers do not notice.
//
// **The seam is UNVALIDATED, and that is stated rather than implied.**
// There is exactly ONE implementation (the direct in-kernel calls below),
// so nothing proves this interface is not simply syscall-shaped. This
// repo's own rule -- `ata nodma`, `nopat`, TFS3 v1 -- is that an
// unreachable path is a guess, and the same applies to an abstraction
// with a single implementation. See docs/decisions.md for what is most
// likely wrong with it (batching, and who owns the reply copy); a
// throwaway second implementation was considered and rejected, since it
// would prove the seam is not syscall-shaped without proving it fits
// anything real.

struct win_transport {
    // For `dmesg` and `gui state` -- which carriage is live is otherwise
    // invisible, and it stops being obvious the moment there are two.
    const char *name;

    // Client -> server, the ordinary request path. `pid` identifies the
    // caller; `req` is a KERNEL copy the implementation may write back
    // into (WIN_REQ_CREATE returns the new id that way).
    //
    // Returns what win_server_request() returns: 1 on success, 0 if
    // refused, -1 for an unknown type or no registered server.
    int (*request)(int pid, struct win_request_msg *req);

    // The diagnostic channel. `msg->type` selects WIN_REQ_DEBUG_CMD (run
    // `msg->text`) or WIN_REQ_DEBUG_MORE (continue the previous reply);
    // on return `msg` carries one WIN_EV_DEBUG_OUT chunk.
    //
    // Separate from request() rather than folded into it because the two
    // carry different payload structs -- see abi/win_proto.h on why the
    // hot path does not get a kilobyte-sized message.
    //
    // Returns 1 on success, 0 if there is no window manager to ask.
    int (*debug)(int pid, struct win_debug_msg *msg);
};

// Installs the carriage. Called once at boot with the direct transport;
// passing NULL restores it, so there is never a window with no transport
// at all (a request arriving then would have nowhere to go, and the
// failure would look like a dead window server rather than a missing
// transport).
void win_transport_register(const struct win_transport *t);

// The live transport's name, for diagnostics. Never NULL.
const char *win_transport_name(void);

// Send one request over the live transport. This is what SYS_WIN_REQUEST
// calls; it is not itself a syscall, which is the point -- the same call
// serves an in-kernel caller (the serial console) that has no syscall to
// make.
int win_transport_request(int pid, struct win_request_msg *req);

// Send one diagnostic message over the live transport.
int win_transport_debug(int pid, struct win_debug_msg *msg);

// `pid` for a request originating in the kernel itself rather than in a
// scheduled process -- the serial debug console is the only such client.
// Distinct from 0, which win_server_request() already treats as "not a
// scheduled process" and refuses.
#define WIN_PID_KERNEL (-1)

#endif
