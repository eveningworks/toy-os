#ifndef ULIB_UCHAN_PAGE_H
#define ULIB_UCHAN_PAGE_H

#include <stdint.h>

// uchan -- the shared page layout for a message channel between two
// ring-3 processes. `uchan.h` is the library; this file is the contract,
// shared by both ends the way lib/uclip_page.h is.
//
// **THE KERNEL IS NOT IN THIS.** A channel is named shared memory
// (SYS_SHM_OPEN), a futex to park on and a wakeword to be woken through
// -- all of which already exist, so the channel itself is a userspace
// library over them. That is Wayland's split: the transport is general
// and the protocol on top is not, and the compositor is a process like
// any other.
//
// TWO OBJECTS, and the shape is `/bin/soundd`'s because the same two
// problems come up:
//
//   `<service>`         -- the BEACON. One page, created by the server,
//                          holding the word a client wakes it through.
//                          Its existence is also how a client knows
//                          there is a server at all.
//   `<service>.<pid>`   -- ONE RING PER CLIENT, created by the client.
//                          The server finds them by walking QUERY_SHM
//                          for the prefix.
//
// A ring per client rather than one shared ring is what makes this
// lock-free: each ring has exactly ONE writer and ONE reader, so `head`
// and `tail` are each written by a single process and neither side ever
// needs an atomic read-modify-write. There is no compare-and-swap in
// this codebase to build the shared-ring version on.

#define UCHAN_MAGIC      0x4E414843u  // "CHAN"
#define UCHAN_SLOT_BYTES 64           // one message
#define UCHAN_SLOTS      32           // messages in flight
#define UCHAN_NAME_MAX   24           // a service name, inside SHM_NAME_MAX

// The server's beacon page.
struct uchan_beacon {
    uint32_t magic;
    uint32_t version;
    int32_t  server_pid;
    uint32_t _pad;
    // THE WORD THE SERVER IS PARKED ON -- what it passed to
    // SYS_WAKEWORD. A client bumps it and calls sys_futex_wake() after
    // writing a message. The BUMP is the half that matters: a server
    // that sampled this before the message and parks after it finds the
    // value already moved and does not park.
    volatile uint32_t wake;
};

// One client's ring.
struct uchan_ring {
    uint32_t magic;
    uint32_t slot_bytes;    // UCHAN_SLOT_BYTES, checked by the reader
    uint32_t slots;         // UCHAN_SLOTS
    int32_t  client_pid;

    // FREE-RUNNING COUNTERS, not indices: `head - tail` is the depth,
    // so full and empty are never the same state. Wrapping is fine --
    // the difference stays right across a uint32 wrap, which an index
    // pair only manages by wasting a slot.
    //
    // `head` is written ONLY by the client, `tail` ONLY by the server.
    // That is what makes this safe without a lock, and it is the
    // property to preserve: a second writer to either needs an atomic
    // this system has not got.
    volatile uint32_t head;
    volatile uint32_t tail;

    // The server's answer to the message at `reply_to`, and a sequence
    // the client watches. Only the few messages that need an answer use
    // it; everything else is fire-and-forget, which is most of the
    // traffic and the reason the ring is asynchronous by default.
    volatile uint32_t reply_seq;
    uint32_t reply_to;
    uint8_t  reply[UCHAN_SLOT_BYTES];

    uint8_t  slot[UCHAN_SLOTS][UCHAN_SLOT_BYTES];
};

#endif
