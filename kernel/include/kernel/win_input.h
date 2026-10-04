#ifndef KERNEL_WIN_INPUT_H
#define KERNEL_WIN_INPUT_H

#include <stdint.h>
#include "win_proto.h"

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
// compositor -- WIN_EV_FONT, WIN_EV_SCREEN, window lifecycle -- and a
// `gui` diagnostic command. Notices have their own queue, popped first,
// so input pressure never costs one; in the input queue a release is
// never shed (win_input.c says how).

// Polls the mouse and keyboard and queues WIN_EV_RAW_*. A no-op with no
// compositor. Called from scheduler_idle(), the kernel's one owner of
// idle work.
void win_input_poll(void);

// Queues `ev` for the compositor and wakes it (its wait channel and its
// wakeword both). Returns 1 if queued, 0 with no compositor. Safe from
// an interrupt handler: the wake only flips scheduler state. Motion
// with the same buttons as the newest queued motion REPLACES it; a full
// input queue drops its oldest event that is not a release, or refuses
// the new one. A notice goes to the notice queue.
int win_input_push(const struct win_event *ev);

// win_input_poll()'s key half: the key and positional streams, in turn,
// as far as the input queue has room. Exported for its KTEST.
void win_input_drain_keys(void);

// Pops the oldest notice, else the oldest input, into `out`; 1 if one
// was waiting, 0 if empty.
int win_input_pop(struct win_event *out);

// The wait channel the compositor parks on -- the queue's own address.
const void *win_input_wait_chan(void);

int win_input_pending(void);   // events queued
int win_input_dropped(void);   // overflow drops since the last reset

// Empties the queue and its counters. Called by the role change.
void win_input_reset(void);

#endif
