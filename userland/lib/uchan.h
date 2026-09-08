#ifndef ULIB_UCHAN_H
#define ULIB_UCHAN_H

#include <stdint.h>
#include "lib/uchan_page.h"

// uchan -- a message channel between two ring-3 processes.
//
// **ASYNCHRONOUS BY DEFAULT.** A send writes a slot and returns; it does
// not wait for the server to look. That is the shape the traffic has --
// almost everything one process tells another is fire-and-forget, and
// making every message a round trip through the scheduler is what makes
// a synchronous transport the wrong one for a per-frame path. Wayland's
// requests are asynchronous for this reason; only an explicit sync
// round-trips. `uchan_call()` is that explicit case.
//
// **NO KERNEL SUPPORT WAS ADDED FOR THIS.** It is shm, a futex and a
// wakeword, all of which exist for their own reasons. See
// lib/uchan_page.h for the layout and why there is a ring per client.
//
// A SERVER WAITS ON ITS WAKEWORD, NOT ON A CHANNEL, which is the whole
// point of the wakeword: the same wait also returns when the kernel
// queues a window or input event, so a compositor can serve clients and
// draw without two waits it cannot combine.

struct uchan_server {
    struct uchan_beacon *beacon;
    // One mapped ring per client, and the pid it belongs to. Sized to
    // the process table: a channel a process cannot reach is a channel
    // with a queue nobody drains.
    struct uchan_ring *ring[64];
    int                pid[64];
    int                count;
    char               name[UCHAN_NAME_MAX];
    unsigned           next;   // round-robin, so one busy client cannot
                                // starve the others
};

struct uchan_client {
    struct uchan_beacon *beacon;
    struct uchan_ring   *ring;
    char                 ring_name[32];
};

// --- server ----------------------------------------------------------

// Publishes `name` as a beacon and registers its wake word with the
// kernel, so a client's wake and a window event arrive through the same
// wait. Returns 0, or -1 (the name is taken, or out of memory).
int uchan_server_open(struct uchan_server *s, const char *name);

// Adopts any client ring that has appeared and drops any whose client
// has gone. Cheap enough to call once a frame; it walks QUERY_SHM.
void uchan_server_scan(struct uchan_server *s);

// The next message from any client, round-robin. Returns the sending
// pid and fills `out` (UCHAN_SLOT_BYTES), or 0 when every ring is empty.
int uchan_server_recv(struct uchan_server *s, void *out);

// Answers the message just received from `pid`. Optional -- only the
// messages that ask for one.
void uchan_server_reply(struct uchan_server *s, int pid, const void *msg);

// Parks until a client sends, the kernel queues an event, or `timeout_ms`
// passes. Returns immediately if anything is already waiting.
void uchan_server_wait(struct uchan_server *s, int timeout_ms);

void uchan_server_close(struct uchan_server *s);

// --- client ----------------------------------------------------------

// Finds `name`'s beacon and publishes this process's ring. Returns 0, or
// -1 when there is no server -- which a caller must report rather than
// treat as success, the same rule lib/uclip.h states.
int uchan_client_open(struct uchan_client *c, const char *name);

// Queues one message (UCHAN_SLOT_BYTES) and wakes the server. Returns 0,
// or -1 if the ring is FULL -- which is a real outcome, not an error to
// swallow: a server that has stopped draining is exactly what a client
// needs to find out about.
int uchan_send(struct uchan_client *c, const void *msg);

// Send, then wait for the server's answer. The explicit round trip, for
// the few messages that have one. Returns 0, or -1 on timeout.
int uchan_call(struct uchan_client *c, const void *msg, void *reply,
               int timeout_ms);

void uchan_client_close(struct uchan_client *c);

#endif
