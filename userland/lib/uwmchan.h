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
// **EVERY REQUEST A CLIENT MAKES OF THE COMPOSITOR IS HERE NOW** (stage
// 6b). The kernel holds no window state at all: it carries EVENTS to
// clients, delivers raw input to whoever holds the compositor role, and
// grants the framebuffer. Nothing it does requires it to know that a
// window exists.
//
// The set, and which two wait for an answer:
//
//   CREATE     window = the slot the client proposes; a/b = w/h;
//              text = its app_id. REPLIES with the granted slot in `a`,
//              or -1. The one request that must round-trip, because the
//              client cannot name its buffers until it has a slot --
//              except it already did, which is why it PROPOSES one.
//   ACTIVATE   no inputs. REPLIES 1 (a twin was raised) or 0.
//   PRESENT    a = WIN_PRESENT_B(buf, gen), b = WIN_PRESENT_SIZE(w, h).
//   DESTROY    window.
//   TITLE / HINTS / CURSOR / TIMER / PONG / CLOSE_PID -- as before.
//
// **A PRESENT CARRIES ITS OWN GEOMETRY, AND THAT IS WHY RESIZE AND
// BUFFER ARE GONE.** Both existed to keep a SECOND record of each
// buffer's size in step with the client's; the compositor reads the
// size off the frame it is about to show, so the second record had one
// reader and no purpose. A record kept in step by remembering to send a
// message is a record that goes stale -- it did, as a window sheared
// one pixel per row whenever a resize happened not to change the
// buffer's page count.
//
// **ORDERING IS NO LONGER A TRAP.** A create and a title used to travel
// on two carriages with no order between them, so a compositor could be
// handed a title for a window it had never heard of; the fix was to
// drain the kernel's event queue first, every frame. One ring per
// client orders everything that client says, and the kernel's queue now
// carries nothing about windows at all.

#define WMCHAN_SERVICE "toywm"

// **TWO MESSAGES WAIT FOR AN ANSWER: CREATE AND ACTIVATE.** Everything
// else is fire-and-forget, which is what the carriage is shaped for --
// a present runs once per frame per client and must never round-trip.
// The two that do are both once-per-window, at startup.
//
// A reply is a `struct wmchan_msg` whose `a` carries the answer. No
// channel, or no answer inside the timeout, is read as a refusal by
// both: a window that never opens is visible, where an app told its
// twin exists simply disappears.

// The same shape as `struct win_request_msg`, deliberately: a request
// that moves to this carriage should not also change what it says.
struct wmchan_msg {
    uint32_t type;      // a WIN_REQ_*
    uint32_t window;    // which of the sender's windows; on CREATE the
                        // slot it PROPOSES, and its buffers are already
                        // named after that
    int32_t  a, b, c;   // CREATE: w, h. PRESENT: WIN_PRESENT_B(buf,gen),
                        // WIN_PRESENT_SIZE(w,h). HINTS: flags, min_w,
                        // min_h. CURSOR/TIMER/PONG/CLOSE_PID: a.
                        // In a REPLY, `a` is the answer.
    char     text[WIN_TITLE_LEN];   // TITLE, and CREATE's app_id
};

_Static_assert(sizeof(struct wmchan_msg) <= UCHAN_SLOT_BYTES,
               "a wmchan message must fit one channel slot");

#endif
