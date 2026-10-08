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
//
// **RETURNS HOW MANY CLIENTS DEPARTED**, and writes their pids into
// `gone` (at most `gone_cap`; pass NULL to ignore them). A server that
// holds state per client needs this: a dead client's name stops
// resolving, and if the slot is reclaimed silently, whatever that
// client owned is left with nothing to retire it. The compositor's
// windows are exactly that.
int uchan_server_scan(struct uchan_server *s, int *gone, int gone_cap);

// The next message from any client, round-robin. Returns the sending
// pid and fills at most `cap` bytes of `out`, or 0 when every ring is
// empty.
//
// **`cap` IS NOT OPTIONAL AND IS NOT THE SLOT SIZE.** A slot is
// UCHAN_SLOT_BYTES and a message struct is usually smaller; a version of
// this that copied a whole slot regardless wrote past every caller's
// buffer, which the ring-3 stack canary caught as a compositor exiting
// with code 2 the instant a client connected. Sized here so a protocol
// whose messages are 40 bytes cannot be handed 64.
int uchan_server_recv(struct uchan_server *s, void *out, unsigned long cap);

// Answers the message just received from `pid`. Optional -- only the
// messages that ask for one. `len` bytes are sent and the rest of the
// slot is zeroed, so a reader asking for more sees zeros rather than
// whatever the last message left.
void uchan_server_reply(struct uchan_server *s, int pid, const void *msg,
                        unsigned long len);

// Parks until a client sends, the kernel queues an event, or `timeout_ms`
// passes. Returns immediately if anything is already waiting.
void uchan_server_wait(struct uchan_server *s, int timeout_ms);

void uchan_server_close(struct uchan_server *s);

// --- server -> client: the inbox -------------------------------------
//
// The other direction, on the same ring. Asynchronous like a send:
// the server writes a slot and wakes the client. A compositor's events
// ride this, which is the Wayland shape -- one connection per client,
// carrying both ways, and the kernel in neither.

// Queues one message (at most UCHAN_IN_BYTES) for `pid` and wakes it.
// Returns 0, or -1 when `pid` has no ring or its inbox is FULL -- a
// real outcome, since the server cannot evict what the client has not
// read: drop the message, or keep it and try again next frame
// (uchan_server_room() says which is coming).
int uchan_server_send(struct uchan_server *s, int pid, const void *msg,
                      unsigned long len);

// Free inbox slots for `pid`; 0 for a pid with no ring.
int uchan_server_room(struct uchan_server *s, int pid);

// --- client ----------------------------------------------------------

// Finds `name`'s beacon and publishes this process's ring. Returns 0, or
// -1 when there is no server -- which a caller must report rather than
// treat as success, the same rule lib/uclip.h states.
int uchan_client_open(struct uchan_client *c, const char *name);

// Queues one message (UCHAN_SLOT_BYTES) and wakes the server. Returns 0,
// or -1 if the ring is FULL -- which is a real outcome, not an error to
// swallow: a server that has stopped draining is exactly what a client
// needs to find out about.
int uchan_send(struct uchan_client *c, const void *msg, unsigned long len);

// The same, WAITING for room while the ring is full -- up to `timeout_ms`,
// and only while the server is alive. What a client that sends a burst
// wants: Xlib blocks on a full output buffer and a Wayland client waits
// for its socket, rather than dropping the request. -1 when it gave up.
int uchan_send_wait(struct uchan_client *c, const void *msg, unsigned long len, int timeout_ms);

// Send, then wait for the server's answer. The explicit round trip, for
// the few messages that have one. Returns 0, or -1 on timeout.
int uchan_call(struct uchan_client *c, const void *msg, unsigned long len,
               void *reply, unsigned long reply_cap, int timeout_ms);

void uchan_client_close(struct uchan_client *c);

// The next inbox message, at most `cap` bytes into `out`. 1 if one was
// copied, 0 when the inbox is empty.
int uchan_client_recv(struct uchan_client *c, void *out, unsigned long cap);

// How many inbox messages are waiting.
int uchan_client_pending(const struct uchan_client *c);

// Parks until the inbox has something, another thread kicks, or
// `timeout_ms` passes -- and returns at once if it already has
// something. `timeout_ms` <= 0 does not park at all (the futex reads 0
// as NO deadline, and a caller that meant "do not wait" would park
// until somebody happened to send).
void uchan_client_wait(struct uchan_client *c, int timeout_ms);

// Wakes a thread parked in uchan_client_wait() from ANOTHER thread of
// the same process. The one thing a worker may do to the loop.
void uchan_client_kick(struct uchan_client *c);

// Messages the server could not fit since the ring was opened.
uint32_t uchan_client_dropped(const struct uchan_client *c);

// Is the server this ring was opened against still there? Its beacon
// is unlinked when it dies and re-made by a successor with a new pid,
// so "the name resolves" is not enough: the pid has to be the one the
// ring was granted to. Cheap enough to ask on a wait timeout, not on
// every wake.
int uchan_client_server_alive(const struct uchan_client *c);

#endif
