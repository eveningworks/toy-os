#ifndef WIN_EVENTS_H
#define WIN_EVENTS_H

#include <stdint.h>
#include "win_proto.h"

// Per-process event queues -- the server side of the windowing
// protocol's delivery path (abi/win_proto.h has the message format and
// the reasoning behind separating the two).
//
// Ring 3 reaches these queues through SYS_WAIT_EVENT/SYS_POLL_EVENT and
// never touches this header. The one thing that does is the window
// server (userland/wm/wm_client.c), which is why this lives in api/ rather
// than kernel/ -- see win_server.h's note on the same promotion.
// Nothing else should be pushing events at a client.

// Drops anything queued for `pid` and resets its queue. Called when a
// process is spawned and when its slot is reaped, so a new tenant of a
// recycled pid can never inherit the previous one's events.
void win_events_reset(int pid);

// Queues `ev` for `pid` and wakes it if it is blocked in
// SYS_WAIT_EVENT. Returns 1 if queued, 0 for a bad pid.
//
// Safe to call from an interrupt handler: the wake it performs only
// flips scheduler state and writes an already-saved trapframe (see
// scheduler_wake()). If the queue is full the OLDEST event is dropped
// -- see WIN_EVENT_QUEUE_MAX's comment for why that direction.
int win_events_push(int pid, const struct win_event *ev);

// Pops the oldest event for `pid` into `out`. Returns 1 if one was
// waiting, 0 if the queue is empty or `pid` is bad.
int win_events_pop(int pid, struct win_event *out);

// The wait channel a client parks on for its own events. 0 for a pid
// with no queue.
const void *win_events_wait_chan(int pid);

// How many events are queued for `pid` (0 for a bad pid). For tests and
// `gui state`, not for delivery decisions.
int win_events_pending(int pid);

// Is `pid` a WINDOWING CLIENT -- has it ever waited for a window event?
//
// The kernel's answer to "who are the GUI processes", which it needs in
// exactly two places and in neither of them because of a window: the
// font broadcast, and asking everyone to close when the compositor
// dies. Both used to walk the window table; the table is gone, and this
// is the part of it that was never really about windows.
//
// A process that waited once and no longer has a window still answers
// yes. That is the safe direction for both callers -- a font event it
// ignores costs nothing, and a close it ignores is what a close is.
int win_events_is_client(int pid);

// Records that `pid` asked for a window event. Called from the two
// syscalls that do; nothing else should.
void win_events_mark_client(int pid);

// How many events have been dropped to overflow for `pid` since its
// last reset. A client that sees this climbing is not keeping up --
// worth surfacing rather than losing silently, which is exactly the
// class of bug that makes input feel mysteriously wrong.
int win_events_dropped(int pid);

#endif
