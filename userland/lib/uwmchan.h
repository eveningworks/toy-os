#ifndef ULIB_UWMCHAN_H
#define ULIB_UWMCHAN_H

#include <stdint.h>
#include "win_proto.h"
#include "lib/uchan_page.h"

// TWP over a channel: a client's request reaching the compositor
// DIRECTLY instead of through the kernel.
//
// **THE MESSAGE TYPE IS A `WIN_REQ_*`.** The protocol is still TWP --
// only the carriage changes, which is the whole bet abi/win_proto.h
// describes: "what a message says lives here and is expected to outlive
// how it is carried". A request that moves to this channel keeps its
// number and its meaning.
//
// WHY ANY OF IT MOVES. The kernel stores `title` for each window and
// answers WIN_REQ_WINDOW_INFO with it, purely because `struct
// win_event` is a fixed 24 bytes and a 32-byte title does not fit --
// so a compositor is told "the title changed" and reads it back. The
// kernel has no use for the string. A channel has room for the payload,
// so the round trip and the kernel's copy both go.
//
// **ORDERING IS THE TRAP.** A window is created through the KERNEL
// (WIN_REQ_CREATE allocates its pixels) and announced on the event
// queue; a title arrives on this channel. Two carriages have no order
// between them, so a compositor that drained this first could be handed
// a title for a window it has never heard of. It drains the EVENT QUEUE
// FIRST, every frame, which is enough: a client cannot send a title
// before its create returned, and the create was queued before that.

#define WMCHAN_SERVICE "toywm"

struct wmchan_msg {
    uint32_t type;      // WIN_REQ_TITLE today
    uint32_t window;    // which of the sender's windows
    char     text[WIN_TITLE_LEN];
};

_Static_assert(sizeof(struct wmchan_msg) <= UCHAN_SLOT_BYTES,
               "a wmchan message must fit one channel slot");

#endif
