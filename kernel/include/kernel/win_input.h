#ifndef KERNEL_WIN_INPUT_H
#define KERNEL_WIN_INPUT_H

#include <stdint.h>
#include "win_proto.h"
#include "fswatch.h"   // FSWATCH_MAX: how many FSWATCH notices can be pending

// The compositor's input path -- evdev's job: the kernel reads the
// devices, queues what it read, and ONE process drains the queue. That
// process is whoever holds the compositor role (win_role.h); the queue
// is reset when the role changes hands, so a successor never inherits
// a predecessor's keystrokes.
//
// Kernel-internal. Ring 3 reaches the queue only through
// SYS_WAIT_EVENT / SYS_POLL_EVENT / SYS_WAIT_READY, and only as the
// compositor. A CLIENT's events never pass through here: the
// compositor writes them into the client's own channel ring
// (userland/lib/uchan.h's inbox).
//
// What rides it besides raw input: the kernel's own notices to the
// compositor -- WIN_EV_FONT, WIN_EV_SCREEN, WIN_EV_SETTING, one
// WIN_EV_FSWATCH per watch -- all in ARRIVAL ORDER with the input. Each
// notice is idempotent and coalesces, so at most WIN_INPUT_NOTICE_RESERVE
// are ever queued, and input never holds more than WIN_INPUT_MAX: A
// NOTICE IS NEVER REFUSED.
//
// INPUT AT WIN_INPUT_MAX evicts its OLDEST event that is not a release --
// the old end, because for input the most recent state is what matters
// -- and with only releases queued refuses the NEW event. A QUEUED
// RELEASE IS NEVER EVICTED. win_input_poll() takes an event from its
// source only when there is room, so none of ITS events -- releases
// included -- is ever refused; only a direct win_input_push() can be,
// and a refused release is logged.
#define WIN_INPUT_MAX 32                             // raw input's share
#define WIN_INPUT_NOTICE_RESERVE (3 + FSWATCH_MAX)   // FONT, SCREEN, SETTING, the watches
#define WIN_EVENT_QUEUE_MAX (WIN_INPUT_MAX + WIN_INPUT_NOTICE_RESERVE)

// Polls the mouse and keyboard and queues WIN_EV_RAW_*. A no-op with no
// compositor. Called from scheduler_idle(), the kernel's one owner of
// idle work.
void win_input_poll(void);

// Queues `ev` for the compositor and wakes it (its wait channel and its
// wakeword both). Returns 1 if queued (or merged), 0 if there is no
// compositor OR the event was refused for lack of room -- input at
// WIN_INPUT_MAX with only releases queued (a refused release is logged).
// Safe from an interrupt handler: the wake only flips scheduler state.
// Motion with the same buttons as the newest queued motion REPLACES it;
// input past WIN_INPUT_MAX drops its oldest event that is not a release.
// A notice replaces an older copy of itself, moving to the end. A
// RAW_MOUSE pushed here is MOTION; a button edge goes through
// win_input_push_mouse_edge(), which says which kind it is.
int win_input_push(const struct win_event *ev);

// A RAW_MOUSE button edge: WIN_INPUT_EDGE_DOWN (a press) or
// WIN_INPUT_EDGE_UP (a release -- kept like a key release, never shed).
// Never merged with motion. win_input_poll() reports its own edges
// through this. Returns 0 for a non-RAW_MOUSE event or any other edge
// value, else as win_input_push().
#define WIN_INPUT_EDGE_DOWN 1
#define WIN_INPUT_EDGE_UP   2
int win_input_push_mouse_edge(const struct win_event *ev, int edge);

// For a KTEST that must see a refused release's log line: lets the next
// one through the once-a-second limit at once.
void win_input_refuse_log_reset(void);

// win_input_poll()'s key half: the key and positional streams, in turn,
// as far as input's share of the queue allows. Exported for its KTEST.
void win_input_drain_keys(void);

// Pops the oldest event into `out`; 1 if one was waiting, 0 if empty.
int win_input_pop(struct win_event *out);

// The wait channel the compositor parks on -- the queue's own address.
const void *win_input_wait_chan(void);

int win_input_pending(void);   // events queued
int win_input_dropped(void);   // overflow drops since the last reset

// Empties the queue and its counters. Called by the role change.
void win_input_reset(void);

#endif
