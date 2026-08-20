#ifndef WIN_PROTO_H
#define WIN_PROTO_H

#include <stdint.h>

// **TWP -- the Toy Window Protocol.** This file IS the protocol: the
// kernel<->client contract for "something happened to your window" and
// for everything a client can ask of its window. Shared by the kernel's
// dispatcher and by ring-3 clients, same as syscall_abi.h.
//
// Named because it is the piece meant to outlive its implementation.
// TWS (the Toy Window Server, kernel/proc/win_server.c +
// userland/wm/wm_client.c) is one implementation of it and is scheduled to
// become a ring-3 process; Toykit (userland/ui/) is the client library
// apps use instead of speaking it by hand. A protocol you can name is
// one you can version -- see docs/decisions.md.
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
#define WIN_EV_RESIZE     6 // a, b: PROPOSED content size -- a configure,
                            // not a command. See below.
#define WIN_EV_WHEEL      8 // a: notches, + = up/away, - = down/toward.
                            //
                            // A client had no way to receive scrolling
                            // at all before this: kernel-space apps got
                            // gui_app::on_wheel and clients got nothing,
                            // so Notepad drew a scrollbar it could never
                            // move and the wheel did nothing in any
                            // ring-3 window.
#define WIN_EV_PING       9 // a: an opaque serial the client must echo
                            // back in WIN_REQ_PONG, unchanged.
                            //
                            // LIVENESS, and the reason it is a protocol
                            // message rather than something the server
                            // can work out for itself: a client that has
                            // stopped pumping its event queue is
                            // indistinguishable, from the outside, from
                            // one that is simply idle. Both draw
                            // nothing and send nothing. So the server
                            // asks, and a client that cannot answer is
                            // wedged.
                            //
                            // This is xdg_shell's ping/pong and ICCCM's
                            // _NET_WM_PING, and it exists for the same
                            // reason both of those do: without it the
                            // only honest thing a compositor can say
                            // about an unresponsive window is nothing.
                            //
                            // A client never writes ping-handling code:
                            // uapp answers it inside the loop, which is
                            // exactly the property that made every
                            // callback optional (ui/uapp.h). An app
                            // wedged in its OWN callback therefore
                            // fails to answer, which is correct -- it
                            // really is not responding.
// --- raw input, for the registered compositor only --------------------
//
// Everything above is POST-routing: the server has already decided which
// window an event belongs to and made the coordinates window-relative. A
// compositor is the thing that makes that decision, so it needs the
// stream from BEFORE it -- screen coordinates, no focus, no hit-testing.
//
// These go only to the pid registered with WIN_REQ_SET_COMPOSITOR, and
// `window` is meaningless on all three (there is no window yet; that is
// the point).
//
// **The mouse is LEVEL STATE, not edges.** The kernel does not
// synthesise press/release events, because it does not have any: the
// PS/2 driver exposes an absolute clamped position plus a button
// bitmask, and today's WM derives edges by diffing against its own
// previous sample. Handing the compositor the same level state it would
// have read itself keeps one differ instead of two, and keeps this
// event honest about what the hardware actually reports. A compositor
// diffs it exactly as wm.c does.
//
// Sent only when something CHANGED (position or buttons). The WM loop
// runs on every timer tick, so an unconditional push would overflow a
// 32-deep queue within a fraction of a second and report constant drops
// while the user did nothing at all.
#define WIN_EV_RAW_MOUSE 10 // a, b: SCREEN position; mods: button bits
                            // (bit0 = left, bit1 = right), level state.
#define WIN_EV_RAW_KEY   11 // a: key code (api/keyboard.h), mods:
                            // KEY_MOD_*. Pre-focus: no window has been
                            // chosen yet.
#define WIN_EV_RAW_WHEEL 12 // a: notches, + = up/away, - = down/toward.
                            // Separate from RAW_MOUSE because the
                            // driver's wheel is a read-and-reset
                            // accumulator, not part of the level state.

#define WIN_EV_DEBUG_OUT 13 // One chunk of a `gui` command's reply. Rides
                            // struct win_debug_msg rather than struct
                            // win_event -- the text does not fit in 24
                            // bytes; see the diagnostic channel section
                            // below. Listed here so the event namespace
                            // stays one list.

// --- client requests, delivered TO THE COMPOSITOR (M41 stage 4d) ------
//
// The inbound half of the inversion. Each of these says "this client did
// something to this window"; `a` is the client's pid and `window` is its
// window id, so the compositor can name the window in the query below.
//
// Deliberately thin. The detail lives in the kernel -- which received it
// in the first place -- and is fetched with WIN_REQ_WINDOW_INFO, rather
// than being crammed into a 24-byte event that would have to grow for
// the first 32-byte title. A compositor that only needs to know
// SOMETHING changed does not pay for the detail.
//
// Delivered only to the registered compositor, and only while there is
// one: with no compositor these are dropped, exactly as the ring-0
// win_server_ops calls were skipped when nothing had registered.
#define WIN_EV_CLIENT_CREATED   15 // a: pid. A window exists; read it.
#define WIN_EV_CLIENT_PRESENT   16 // a: pid. Its buffer has new pixels.
#define WIN_EV_CLIENT_DESTROYED 17 // a: pid. It is going away. The
                                    // buffer is ALREADY freed when this
                                    // arrives -- unlike the ring-0
                                    // callback, which ran while it was
                                    // still valid, because there is no
                                    // way to hold a ring-3 process
                                    // inside a kernel teardown.
#define WIN_EV_CLIENT_TITLE     18 // a: pid. Title changed; re-read it.
#define WIN_EV_CLIENT_HINTS     19 // a: pid. Hints changed; re-read.
#define WIN_EV_CLIENT_RESIZED   20 // a: pid, b: new w, mods: new h. The
                                    // buffer was reallocated, so the
                                    // compositor must re-map it.
#define WIN_EV_CLIENT_PONG      21 // a: pid, b: the serial echoed back.
#define WIN_EV_CLIENT_TIMER     22 // a: pid. This window's timer is due.
#define WIN_EV_CLIENT_CLOSE     23 // a: pid, window unused. Close every
                                    // window this pid owns -- the
                                    // desktop's own "quit that app".
#define WIN_EV_CLIENT_DEBUG     25 // No payload. A `gui` command is
                                    // waiting; fetch it with
                                    // WIN_REQ_DEBUG_TAKE and answer with
                                    // WIN_REQ_DEBUG_REPLY. Thin like the
                                    // rest because the command is 128
                                    // bytes and this struct is 24.
                                    //
                                    // **The sender is BLOCKED until the
                                    // reply arrives or the deadline
                                    // passes.** Unlike every other event
                                    // here, taking your time has a cost
                                    // somebody can see: the console is
                                    // holding the line.
#define WIN_EV_CLIENT_ACTIVATE  24 // a: pid. RAISE this window: a second
                                    // copy of a single-instance app
                                    // asked for its twin, the kernel
                                    // found it, and this is the action
                                    // half. The ANSWER already went back
                                    // to the asking client, so the
                                    // compositor is being told, not
                                    // asked -- which is what removes the
                                    // round trip.

#define WIN_EV_TIMER     14 // This window's repeating timer is due. No
                            // payload: a client that wanted to know the
                            // time can ask, and putting one here would
                            // be a second clock to disagree with
                            // SYS_TICKS.
                            //
                            // The point of it is what a client does
                            // BETWEEN two of these: it blocks in
                            // SYS_WAIT_EVENT. Without a timer the only
                            // way to animate or to refresh on a
                            // schedule was to poll -- run the loop,
                            // yield, run it again -- which wakes a
                            // process 100 times a second to do work it
                            // wanted to do twice. Task Manager was the
                            // worked example (see docs/roadmap.md).
                            //
                            // ONE timer per window, not a set of them.
                            // A client wanting several derives them
                            // from one short interval, exactly as an
                            // app does on top of a single frame clock;
                            // a general timer service is a bigger
                            // feature than anything here needs.

#define WIN_EV_FOCUS      7 // a: 1 = this window gained keyboard focus,
                            // 0 = lost it.
                            //
                            // Sent because a client cannot otherwise
                            // tell: it sees keys only when focused, but
                            // "no keys have arrived" is indistinguishable
                            // from "the user is thinking". An app that
                            // draws a CARET has to know -- an unfocused
                            // window showing one claims to be taking
                            // input that is going somewhere else.

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

// ---------------------------------------------------------------------
// Client -> server requests
// ---------------------------------------------------------------------
//
// The other direction of the protocol, and deliberately the same shape:
// one typed message, not one syscall per operation. That is what keeps
// the client/server boundary a protocol rather than an API -- add an
// operation and it is a new message type, carried by the same
// transport, with no new kernel entry point and nothing for a future
// userspace server to re-plumb.

#define WIN_REQ_CREATE  1 // a: width, b: height, c: x, d: y (screen);
                           // `text`: this window's APP ID, or "" for
                           // none (see WIN_REQ_ACTIVATE).
                           // On success `window` is filled in with the
                           // new window's id and the client's buffer is
                           // mapped at win_buffer_vaddr(id).
                           //
                           // The app id rides CREATE rather than being
                           // a message of its own so that a window can
                           // never exist without it: an id registered a
                           // moment later leaves a gap in which a second
                           // copy of the same program asks "is anyone
                           // there?" and is told no. `text` was unused
                           // by CREATE, so this costs no bytes.
#define WIN_REQ_PRESENT 2 // `window`: which one. The client has finished
                           // drawing into its buffer; composite it.
#define WIN_REQ_DESTROY 3 // `window`: which one. Closes it and unmaps
                           // the buffer.
#define WIN_REQ_TITLE   4 // `window`: which one; the title comes from
                           // the request's `text` field.
#define WIN_REQ_HINTS   6 // How this window should BEHAVE. a: WIN_HINT_*
                           // flags, b/c: minimum content w/h (0 = the
                           // server's floor). Sent once after create,
                           // and re-sendable if an app changes its mind.
                           //
                           // Behaviour is DECLARED, not inferred: the
                           // server does not guess "resizable" from a
                           // window's size or its title. Same principle
                           // as fs_ops.caps and display_driver's
                           // capability bits.
#define WIN_REQ_RESIZE  7 // a/b: requested content w/h. Reallocates this
                           // window's buffer and remaps it AT THE SAME
                           // VIRTUAL ADDRESS, so the client's pointer
                           // stays valid across the call -- see
                           // win_buffer_vaddr() below, which derives
                           // that address from the window id rather
                           // than returning it. On success a/b come
                           // back as the size actually granted.
                           //
                           // A client resizes ITSELF. The server never
                           // reallocates a buffer underneath a running
                           // client; it asks, with WIN_EV_RESIZE, and
                           // this is the answer. See that event.
#define WIN_REQ_PONG    8 // a: the serial from WIN_EV_PING, echoed back
                           // unchanged. The answer to a liveness check;
                           // see that event. Sent by the toolkit, not
                           // by application code.
#define WIN_REQ_SET_COMPOSITOR 9 // a: 1 = claim the compositor role,
                           // 0 = release it. No other fields.
                           //
                           // The way IN to win_server.h's compositor
                           // registration, which existed with nothing
                           // able to call it. A registered compositor
                           // may map other processes' window buffers
                           // and receives the raw input stream
                           // (WIN_EV_RAW_*) the WM consumes today.
                           //
                           // A MESSAGE rather than a syscall of its
                           // own, because that is this protocol's whole
                           // bet -- see the header comment. That costs
                           // one exception, made in two places and
                           // worth knowing about: every other request
                           // is refused outright when no presentation
                           // layer is registered (syscall.c's
                           // win_server_active() gate, and
                           // win_server_request()'s own !g_ops guard),
                           // and this one must work without one. In
                           // stage 4 the ring-3 WM IS the compositor,
                           // so there is no kernel-side presentation
                           // layer left to register first -- gating
                           // this behind one would make it permanently
                           // unreachable at exactly the point it
                           // matters.
                           //
                           // Claiming replaces any previous holder and
                           // revokes every mapping it held; dying
                           // releases it (win_server_client_gone()).
                           // There is deliberately no arbitration --
                           // last claimant wins, the same way
                           // display_register() lets the last driver
                           // claim the screen.
#define WIN_REQ_FONT    5 // No inputs. Maps the desktop's ACTIVE font
                           // read-only into the client at
                           // WIN_FONT_VADDR and fills in the metrics:
                           // a = glyph width, b = glyph height,
                           // c = glyph count, d = the byte offset of
                           // glyph 0 within the mapping (the data does
                           // not necessarily start on a page boundary,
                           // so glyph 0 lives at WIN_FONT_VADDR + d,
                           // not at WIN_FONT_VADDR).
                           // `window` is ignored -- a font belongs to
                           // the session, not to one window.
                           //
                           // WHY THE SERVER HANDS OVER THE FONT rather
                           // than each client carrying its own: the
                           // baked glyph data is ~11,800 lines of
                           // tables (kernel/drivers/font_ttf.c), so a
                           // copy per client is both large and, worse,
                           // free to drift from the desktop's -- a
                           // client would keep rendering at the old
                           // size after `font_size` changed. One
                           // read-only mapping of the kernel's own
                           // data keeps every client's text identical
                           // to the desktop's by construction.
                           //
                           // READ-ONLY is load-bearing: this maps
                           // pages out of the kernel image itself, so
                           // a writable mapping would let any client
                           // scribble on kernel .rodata.

#define WIN_REQ_CLOSE_PID  12 // a: the pid whose window(s) should be
                           // ASKED to close. Returns 1 if at least one
                           // window was asked, 0 if that pid has none.
                           //
                           // Task Manager's "End Task": the polite half
                           // of ending a process, as against SYS_KILL's
                           // immediate one. It goes through the server
                           // rather than being a syscall because the
                           // decision is the WINDOW MANAGER's -- it runs
                           // the same wm_request_close() the X button
                           // and Alt+F4 use, so a client may refuse it
                           // exactly as it may refuse those, and there
                           // is no fourth path that could drift from
                           // them (repeating that check is precisely how
                           // the context menu once drifted into seizing
                           // windows instead of asking).
                           //
                           // Unprivileged, like SYS_KILL and for the
                           // same reason -- and strictly weaker than it,
                           // since the target may decline.

#define WIN_REQ_ACTIVATE   13 // NO INPUTS. "Is a window of MY program
                           // already open?" If one is, raise it
                           // (un-minimizing it if needed), give it
                           // focus, and return 1. Return 0 if not.
                           //
                           // **THE CALLER NAMES NOTHING**, and that is
                           // the point. It used to pass an app id in
                           // `text`, which made the answer depend on a
                           // string each app declared about itself --
                           // so two apps declaring the same one raised
                           // each other's windows, and since a "yes"
                           // means "my twin is up, exit now", the second
                           // app simply never appeared. No runtime check
                           // could catch it either: two copies of ONE
                           // program are SUPPOSED to match. The kernel
                           // answers from the caller's own spawn path
                           // now (win_server.c's app_identity_for()),
                           // which is a fact the asker cannot influence.
                           //
                           // The caller's own windows are skipped, or
                           // every single-instance app would refuse its
                           // own first window.
                           //
                           // SINGLE INSTANCE, and the reason it is the
                           // CLIENT that decides: a launcher launches
                           // (see apps/gui_apps.h's exec_path), because
                           // the desktop cannot know whether a second
                           // copy of a program is meaningful -- two
                           // Notepads editing two files are useful and
                           // two Task Managers are not. So a program
                           // that wants to be alone asks first, and
                           // exits quietly if the answer is yes. A
                           // program that says nothing keeps today's
                           // behaviour exactly.
                           //
                           // This is the named-mutex-plus-raise pattern
                           // every desktop ends up with (Windows'
                           // CreateMutex + SetForegroundWindow, GTK's
                           // GApplication uniqueness, Qt's
                           // QtSingleApplication) rather than anything
                           // novel -- the raise is the half that makes
                           // it feel correct instead of merely refusing
                           // to start.
                           //
                           // Addressed to an app id rather than to one
                           // of the caller's own windows, so like
                           // WIN_REQ_CLOSE_PID it skips the ownership
                           // lookup and is unprivileged. The worst a
                           // caller can do with it is raise a window
                           // the user can already click on.
                           //
                           // KNOWN GAP, and it is a real one: nothing
                           // serialises the ask against the create, so
                           // two copies launched in the same instant can
                           // both be told "nobody there" and both open.
                           // That needs the id to be CLAIMED by the ask
                           // rather than merely queried -- worth doing
                           // if it ever bites, and not worth the extra
                           // state until then, since every launch path
                           // here is a human clicking a menu.

#define WIN_REQ_FB_MAP     15 // No inputs. Maps the real linear
                           // framebuffer WRITABLE into the caller at
                           // WIN_FB_VADDR and fills in its geometry:
                           // a = width, b = height, c = pitch in
                           // BYTES, d = bits per pixel. `window` is
                           // ignored -- the screen belongs to the
                           // session, not to a window.
                           //
                           // REFUSED unless the caller is the
                           // registered compositor (WIN_REQ_SET_
                           // COMPOSITOR). That is the whole difference
                           // between this and the legacy SYS_GUI_INIT,
                           // which any process may call: this one is a
                           // grant tied to a role, revoked when the
                           // role is dropped or the process dies.
                           //
                           // The mapping is WRITE-COMBINING. Writes
                           // coalesce into burst transfers; reads are
                           // full uncached round trips, so a caller
                           // composites in its own back buffer and
                           // copies OUT to this, never reading it back.
                           //
                           // Mapping it does not make it visible on
                           // every adapter -- see WIN_REQ_FB_PRESENT.
#define WIN_REQ_EVENT_PUSH 17 // Deliver one event to a client.
                           //   a      = target pid
                           //   window = the target's window id
                           //   b      = WIN_EV_* type
                           //   c, d   = the event's a and b
                           //   mods   = the event's mods
                           //
                           // REFUSED unless the caller is the registered
                           // compositor. Routing input is the
                           // compositor's job by definition -- it is the
                           // one process that knows what is on top of
                           // what -- so this is the request that lets a
                           // RING-3 one do it. In ring 0 the WM called
                           // win_events_push() directly, which is not a
                           // thing a process can do.
                           //
                           // The event is spelled out field by field
                           // rather than copied as a struct, so the
                           // message stays a message: fixed-layout,
                           // pointer-free, and readable in a log.
                           //
                           // Returns 0 on success, -1 if refused or the
                           // target's queue is full. A FULL QUEUE IS NOT
                           // AN ERROR the compositor can fix -- the
                           // client is not draining -- so it is reported
                           // rather than retried.
#define WIN_REQ_EVENT_STATS 18 // Queue depth for a pid.
                           //   a = pid to ask about, or 0 for "me"
                           // and on return:
                           //   a = events pending, b = events dropped,
                           //   c = the registered compositor's pid
                           //
                           // For `gui compositor`, which reports exactly
                           // these. Readable by the compositor only, for
                           // the same reason the push is: it is the only
                           // process with any business knowing how far
                           // behind another one is.
#define WIN_REQ_MAP_WINDOW 20 // Map another process's window buffer into
                           // the compositor.
                           //   a      = owning pid (in)
                           //   window = its window id (in)
                           // Returns 0; the buffer appears at
                           // win_compositor_vaddr(pid, id), which the
                           // caller computes itself -- so there is
                           // nothing to return but success.
                           //
                           // Refused to anyone but the registered
                           // compositor: a window buffer is a client's
                           // private memory, and this is the request
                           // that hands it to somebody else.
                           //
                           // The kernel side (win_server_map_to_
                           // compositor()) landed in stage 1 with only
                           // KTESTs calling it -- a primitive with no
                           // protocol path, which by this repo's own
                           // rule left it UNVALIDATED against real use.
                           // This is that path.
                           //
                           // The mapping is REVOKED wherever the frames
                           // are freed or replaced (destroy, resize,
                           // client death), so a compositor re-maps on
                           // WIN_EV_CLIENT_RESIZED rather than assuming
                           // its pointer survived.
#define WIN_REQ_WINDOW_INFO 19 // Read one client window's details.
                           //   a      = owning pid (in)
                           //   window = its window id (in)
                           // and on return:
                           //   a, b   = width, height
                           //   c      = WIN_HINT_* flags
                           //   d      = min_w in the low 16 bits,
                           //            min_h in the high 16
                           //   text   = the title, NUL-terminated
                           //
                           // The compositor's half of the thin events
                           // above: it is told a window changed and
                           // reads what it needs. Refused to anyone but
                           // the registered compositor -- these are
                           // another process's window's details.
                           //
                           // Returns 0, or -1 if there is no such window
                           // (which is not an error the compositor can
                           // avoid: a client may destroy a window
                           // between the event and this call, and the
                           // right response is to drop the window rather
                           // than to retry).
#define WIN_REQ_WINDOW_APPID 23 // Read one client window's IDENTITY.
                           //   a      = owning pid (in)
                           //   window = its window id (in)
                           // and on return:
                           //   a      = the application identity, an
                           //            opaque number that is equal for
                           //            two windows of the same PROGRAM
                           //            and different otherwise; -1 for
                           //            a window whose owner has no
                           //            identity, which matches nothing
                           //   text   = the app id the client declared,
                           //            a DISPLAY NAME with no
                           //            correctness role
                           //
                           // The identity is the owning process's spawn
                           // path, interned by the kernel -- see
                           // win_server.c. A number rather than the path
                           // because `text` is WIN_TITLE_LEN and a path
                           // is FS_PATH_MAX, so shipping the path would
                           // truncate it and two long paths sharing a
                           // prefix would collide silently.
                           //
                           // A SECOND request rather than a second field
                           // on WIN_REQ_WINDOW_INFO because that message
                           // carries exactly one `text`, and the title
                           // and the app id are both WIN_TITLE_LEN --
                           // so they do not both fit and widening the
                           // struct would cost every WIN_REQ_PRESENT on
                           // the hot path (see struct win_request_msg).
                           //
                           // Asked ONCE, when the window is created:
                           // neither an identity nor an app id ever
                           // changes, unlike the title.
                           //
                           // Compositor only, and same -1-means-gone
                           // contract as WIN_REQ_WINDOW_INFO.
#define WIN_REQ_FB_PRESENT 16 // a, b, c, d: x, y, w, h of the region
                           // just written. Publishes it.
                           //
                           // Required, not advisory, and not something
                           // a client may skip after checking the
                           // driver: a display_driver may declare
                           // DISPLAY_CAP_NEEDS_FLUSH (vmsvga does), and
                           // on one of those the adapter shows NOTHING
                           // until told which region changed. On a
                           // continuously-scanned adapter the kernel's
                           // display_flush() is already a no-op, so one
                           // code path serves both and the flush path
                           // stays exercised rather than becoming
                           // reachable on one driver only.
                           //
                           // A rect rather than the whole screen
                           // because that is what the caller knows and
                           // what the adapter wants; a rect outside the
                           // screen is clamped, and an empty one is a
                           // legal no-op rather than an error.
#define WIN_REQ_TIMER      14 // `window`: which one; a: the repeat
                           // interval in MILLISECONDS, or 0 to cancel.
                           // Delivers WIN_EV_TIMER every `a` ms until
                           // cancelled or the window closes.
                           //
                           // Milliseconds, not ticks, because the tick
                           // rate is the kernel's business and a client
                           // asking to be woken "every 500ms" should
                           // not have to know it is 100Hz today. The
                           // server rounds to whole ticks and to a
                           // minimum of one, so an interval faster than
                           // the timer resolution becomes "every tick"
                           // rather than an error -- the same
                           // clamp-don't-refuse rule the resize path
                           // follows.
                           //
                           // A DEADLINE, not a queue: if the client is
                           // slow the timer does not accumulate a debt
                           // of missed firings, it simply fires again
                           // once the next interval is due. A client
                           // that cannot keep up should not be punished
                           // with a backlog it will never drain -- the
                           // same reasoning as the event queue dropping
                           // the OLDEST rather than the newest.

#define WIN_REQ_DEBUG_CMD  10 // Run one `gui` diagnostic command. The
                           // command text and the reply both ride
                           // struct win_debug_msg, not this struct --
                           // see that struct for why.
#define WIN_REQ_DEBUG_MORE 11 // Fetch the next chunk of the reply the
                           // previous DEBUG_CMD started. No inputs.
#define WIN_REQ_DEBUG_TAKE 21 // Compositor only. Copies the pending
                           // `gui` command into `text` and clears it, so
                           // a second TAKE gets nothing rather than
                           // running the same command twice.
                           // Returns 1 with the command, 0 if none is
                           // pending.
#define WIN_REQ_DEBUG_REPLY 22 // Compositor only. `text`/`len` are the
                           // command's output; `flags` may carry
                           // WIN_DEBUG_F_UNKNOWN for a command the
                           // compositor did not recognise, which the
                           // caller must be able to tell from one that
                           // legitimately printed nothing.
                           //
                           // Unblocks whoever asked. A reply with no
                           // pending command is ignored rather than
                           // refused -- it means the deadline already
                           // passed, and the compositor has no way to
                           // have known that.

// --- the diagnostic channel (Milestone 41, stage 3) -------------------
//
// The `gui` commands the GUI test tools drive the desktop with, carried
// as protocol messages instead of a direct call from the kernel's serial
// console into WM internals. Every GUI tool reaches the WM this way, so
// the ~240 checks that prove the desktop works have to cross the
// transport before the WM itself can move to ring 3 (stage 4).
//
// **A message, not a side channel.** These are ordinary WIN_REQ_*/
// WIN_EV_* types on the one transport, so a ring-3 window server
// inherits the diagnostic path with nothing to re-plumb -- the same bet
// WIN_REQ_SET_COMPOSITOR made. A separate channel was considered (a
// diagnostic is not app-facing traffic) and rejected as a second
// mechanism to maintain and move.
//
// **Why its own struct rather than widening the two above.** A reply is
// text and runs to kilobytes -- `gui help` alone is ~1.8 KB and
// `gui windows --json` grows with the window count. struct win_event is
// a fixed 24 bytes and struct win_request_msg carries text[32], so
// neither can hold one; widening either would put a kilobyte-sized copy
// on the path of EVERY request, and WIN_REQ_PRESENT is the hot path --
// once per client frame. So the diagnostic pair carries its own payload
// and the hot path keeps its 56-byte message. Same fixed-layout, no
// pointer discipline as the other two, for the same reason: these bytes
// must work unchanged whether a syscall copies them or a client reads
// them straight out of a shared ring.
#define WIN_DEBUG_CMD_LEN 128 // longest command, including the NUL. The
                              // longest one any tool sends today is a
                              // `spawn` with arguments, ~40 bytes.
#define WIN_DEBUG_CHUNK   512 // reply bytes per message, excluding the
                              // NUL. Sized so a typical one-line answer
                              // fits in a single round trip while the
                              // struct stays well under a page.

// The longest reply a `gui` command may produce, in total. One MESSAGE
// carries WIN_DEBUG_CHUNK bytes, so a reply larger than that arrives in
// several -- in both directions now: the kernel already chunked it out
// to the console, and a ring-3 compositor chunks it IN the same way,
// setting WIN_DEBUG_F_MORE on every piece but the last.
//
// In the ABI rather than private to win_server.c because both ends size
// a buffer by it, and two ends disagreeing about a maximum is how a
// reply gets silently truncated at whichever end guessed smaller.
//
// **IT WAS 4096, and that is about twenty-five windows' worth of
// `gui windows --json`** -- past which the reply was cut mid-object and
// every tool asking for the window list got a parse error rather than a
// short answer. Two separate things were wrong and both are fixed: the
// emitters reserve room for their own ending and roll back a partial
// element (dbg_out_reserve()/dbg_out_rollback()), so what comes back is
// always VALID and says `"truncated":true`; and this bound is now
// 16 KiB, which is roughly a hundred windows. The first fix is the one
// that matters -- a bound can always be reached, and a reply that
// cannot be parsed at its bound is a bug at any size.
#define WIN_DEBUG_REPLY_MAX 16384

#define WIN_DEBUG_F_MORE  0x01 // set on a reply when more chunks follow:
                               // ask again with WIN_REQ_DEBUG_MORE. The
                               // reply is NOT self-delimiting -- a chunk
                               // that exactly fills the buffer is
                               // indistinguishable from a truncated one
                               // otherwise, which is the trap a
                               // "read until short" convention sets.
#define WIN_DEBUG_F_UNKNOWN 0x02 // the WM did not recognise the
                               // subcommand; `text` holds nothing. Kept
                               // distinct from an empty reply, since a
                               // command that legitimately prints
                               // nothing is not an error.

struct win_debug_msg {
    uint32_t type;  // WIN_REQ_DEBUG_CMD / WIN_REQ_DEBUG_MORE going in,
                    // WIN_EV_DEBUG_OUT coming back
    uint32_t flags; // WIN_DEBUG_F_*, reply only
    uint32_t len;   // bytes valid in `text`, reply only
    uint32_t reserved; // must be 0; keeps the struct 8-byte aligned
    char text[WIN_DEBUG_CHUNK + 1]; // command in / reply chunk out,
                                    // always NUL-terminated
};

// --- window behaviour hints (WIN_REQ_HINTS's `a`) ---------------------
#define WIN_HINT_RESIZABLE 0x01 // the user may resize this window

// --- resize is a CONFIGURE/ACK HANDSHAKE ------------------------------
//
// The obvious implementation -- the server resizes the window when the
// user drags, and the client finds out afterwards -- is wrong here, and
// visibly so: the server composites client_w * client_h pixels into
// whatever content rectangle the chrome now describes, so the window
// would grow with the content stuck at the old size in one corner. The
// buffer belongs to the client; only the client can decide when it
// changes.
//
// So, following Wayland's xdg_toplevel configure/ack -- the same
// problem, solved the same way, for the same reason:
//
//   1. The user drags the resize grip. The WM tracks a proposed size
//      and draws a rubber-band outline; the window itself does not
//      change yet.
//   2. On release the WM clamps to the client's hinted minimum and
//      sends WIN_EV_RESIZE(w, h) -- a proposal.
//   3. The client answers with WIN_REQ_RESIZE. The server frees the old
//      frames, allocates new ones and maps them at the same virtual
//      address, so the client's buffer pointer survives.
//   4. The client redraws and presents. The WM adopts the new content
//      size when that present arrives.
//   5. If the server refuses (out of contiguous memory, over
//      WIN_CLIENT_MAX_*), the client keeps the size it had and the
//      window does not change. A refusal is a normal outcome, not an
//      error path.
//
// A client that ignores WIN_EV_RESIZE simply does not resize, which is
// the same politeness WIN_EV_CLOSE has: see "a client window's close
// button is a handshake, not a seizure" in docs/decisions.md.

// Longest window title a client may set, including the NUL. Matches the
// window manager's own WIN_TITLE_MAX -- a client that sends more gets
// it truncated at this boundary rather than refused, since a too-long
// title is a cosmetic problem and not worth failing a request over.
#define WIN_TITLE_LEN 32

// Longest app id, including the NUL. Rides the same `text` field as a
// title (they are never both in flight -- one is CREATE's, the other
// TITLE's), so it is the same size by construction rather than by
// coincidence: a separate, larger constant would silently truncate the
// moment `text` stayed 32 bytes.
//
// An id is an opaque token matched byte for byte, not a path or a
// display name -- "taskmgr", not "/bin/wm/system/taskmgr" or "Task
// Manager". A path would break the moment a binary moved, and a display
// name would collide with the title the user sees.
#define WIN_APP_ID_LEN WIN_TITLE_LEN

// Largest client window, in pixels -- the 1280x720 mode boot.asm asks
// for, so a client can be dragged to fill the screen and the window
// manager's own screen-bounds clamp (wm_update_drag_resize()) becomes
// the effective limit rather than this one. That is the point: at 640x480
// a resize past the cap was refused SILENTLY, since a refusal is a normal
// protocol outcome and looks identical to a client that simply declined.
//
// Still bounded, because the server allocates and maps the whole buffer
// up front and does it CONTIGUOUSLY (see create_window()): at 4 bytes
// per pixel this is 900 frames from pmm_alloc_contiguous(), the most
// this kernel is willing to hand one window without a growable-mapping
// story. Growing past the display, or dropping the contiguity
// requirement so fragmentation can't refuse a resize, is docs/roadmap.md's
// growable client buffers item -- not this constant getting bigger again.
#define WIN_CLIENT_MAX_W 1280
#define WIN_CLIENT_MAX_H 720

// Same fixed-layout discipline as struct win_event: no pointers, so the
// identical bytes work whether copied by a syscall or read out of a
// shared ring.
struct win_request_msg {
    uint32_t type;   // WIN_REQ_*
    uint32_t window; // in for PRESENT/DESTROY/TITLE, out for CREATE
    int32_t  a, b, c, d;
    // Mirrors struct win_event's own `mods`, and exists for the same
    // reason it does there: WIN_REQ_EVENT_PUSH carries a whole event,
    // and a/b/c/d plus `window` is exactly one field short of one.
    //
    // Four bytes on a 56-byte message. The rule this does not break is
    // the one about the DIAGNOSTIC channel (WIN_DEBUG_CMD_LEN, 128
    // bytes) carrying its own payload struct rather than widening this
    // one -- that would have put a kilobyte-sized copy on the path of
    // every request, and WIN_REQ_PRESENT is the hot path.
    uint32_t mods;
    char     text[WIN_TITLE_LEN]; // WIN_REQ_TITLE only; NUL-terminated
};

// A client's window buffers are mapped at fixed, per-window addresses
// so a client never has to be told where its buffer landed -- it can
// compute the address from the window id the server handed back.
//
// Spaced WIN_BUFFER_STRIDE apart, which is comfortably more than the
// largest buffer WIN_CLIENT_MAX_W * WIN_CLIENT_MAX_H * 4 can need, so
// two windows' mappings can never overlap regardless of their sizes.
// The stride is the SECOND cap on window size and the one that is easy
// to miss -- it bounded windows to 2 MiB of pixels no matter what
// WIN_CLIENT_MAX_* said.
//
// **It is 64 MiB so that a 4K window is an allocator question rather
// than an addressing one.** 3840x2160x4 is 31.6 MiB, which the previous
// 8 MiB stride could not hold however the other caps were set -- so
// every future step toward a 4K desktop would have had to move these
// addresses first. Address space is the cheap part; it is reserved now
// and nothing is spent until a window is actually that big. What still
// bounds a 4K window is `pmm_alloc_contiguous()` (31.6 MiB is 8192
// CONTIGUOUS frames, which fragmentation can refuse, silently and by
// design) and WIN_CLIENT_MAX_W/H below -- see docs/roadmap.md.
//
// Kept at a comfortable multiple rather than the tight fit, since
// virtual address space costs nothing
// here: nothing else in a client's address space lives above
// WIN_CLIENT_BASE (the stack tops out just below it, the heap below
// that), so the whole region and the font above it are free to grow.
//
// **This address is what bounds the HEAP, and it has been moved up once
// already for exactly that reason.** It was 0x8001000000, which left a
// process ~14 MiB of heap -- enough for a 1080p compositor back buffer
// and not much else, and a hard ceiling with no mechanism behind it.
// Moving it to 0x8080000000 leaves ~2 GiB, which is more than any
// machine this OS boots on has, so PHYSICAL memory is the limit now
// rather than a constant. It stays below WIN_FB_VADDR (0x8100000000)
// with the font region in between; see kernel/uaddr.h for the map.
#define WIN_CLIENT_BASE   0x8080000000ULL
#define WIN_BUFFER_STRIDE 0x0004000000ULL // 64 MiB per window slot
#define WIN_CLIENT_MAX    4 // windows one client may hold at once

static inline uint64_t win_buffer_vaddr(uint32_t window) {
    return WIN_CLIENT_BASE + (uint64_t)window * WIN_BUFFER_STRIDE;
}

// Where WIN_REQ_FONT maps the shared glyph data. Placed above every
// window's buffer slot so the two regions can never collide however
// many windows a client opens.
#define WIN_FONT_VADDR (WIN_CLIENT_BASE + (uint64_t)WIN_CLIENT_MAX * WIN_BUFFER_STRIDE)

// --- the compositor's view of OTHER processes' windows ----------------
//
// A ring-3 compositor has to read the pixels of windows it does not own,
// which is the one thing the addresses above cannot express: they are
// per-CLIENT, and two clients both hold window 0. So a compositor sees
// every window in a region of its own, at an address DERIVED from the
// pair (owner pid, window id) exactly as a client's own buffer is
// derived from the id alone.
//
// **Derived rather than returned, for the same reason it was a good
// idea the first time.** A resize reallocates a window's frames and
// re-maps them AT THE SAME ADDRESS, so the compositor's pointer stays
// valid across a resize it did not initiate and never has to be told
// where the pixels moved. It also means the kernel can revoke a mapping
// without being told where it is -- it computes the address the same
// way -- which is what makes "the mapping is gone after the client
// dies" checkable rather than a matter of bookkeeping the two sides
// might disagree about.
//
// Placed well above the font so the three regions cannot collide: a
// client's own buffers end at WIN_FONT_VADDR, and this starts far
// enough above that the whole compositor region (MAX_PROCS x
// WIN_CLIENT_MAX slots) fits underneath the next round address. Virtual
// space costs nothing here.
//
// It was 0x8010000000, and that is the trap this whole map has to be
// read as a whole to avoid: the region below it looked spare, and it
// was not -- 64 pids x 4 windows x the stride is GIGABYTES, so the
// region's END is what the next thing has to clear, never its base. A
// change that grew the heap into 0x8080000000 landed inside it.
#define WIN_COMPOSITOR_BASE 0x80A0000000ULL

// How many processes' windows the region has room for. Must be >=
// SCHED_MAX_PROCS (api/scheduler.h) -- win_server.c static_asserts
// exactly that, since this header is ABI and cannot include a kernel
// one. The server refuses a pid outside it rather than computing an
// address that overlaps someone else's.
//
// Costs nothing but virtual address space: 64 x WIN_CLIENT_MAX x the
// 64 MiB stride is 16 GiB of vaddr in a region with nothing mapped in
// it until a window exists.
#define WIN_COMPOSITOR_MAX_PIDS 64

// --- the compositor's framebuffer grant -------------------------------
//
// Where WIN_REQ_FB_MAP maps the real linear framebuffer. Above the
// compositor region (64 x WIN_CLIENT_MAX slots, 16 GiB) so the three
// mapped regions -- own buffers, other windows, the screen itself --
// cannot collide. A 4K screen is 31.6 MiB here, which is why the gap
// above this address matters as much as the one below it.
//
// A FIXED address rather than one the kernel returns, for the reason
// the derived addresses above give: an address that varies per boot is
// one nothing can assert about, and a test then cannot tell a wrong
// mapping from a moved one. There is exactly one registered compositor,
// so unlike a window buffer this needs no per-process derivation --
// hence a plain constant rather than a function.
//
// Stated here rather than in kernel/uaddr.h (which holds the rest of
// the ring-3 address map) because a CLIENT needs this number and
// uaddr.h is kernel-internal -- the same reason WIN_FONT_VADDR lives
// here. uaddr.h carries a pointer to it.
#define WIN_FB_VADDR 0x8500000000ULL

static inline uint64_t win_compositor_vaddr(int pid, uint32_t window) {
    return WIN_COMPOSITOR_BASE
         + ((uint64_t)(pid - 1) * WIN_CLIENT_MAX + (uint64_t)window)
           * WIN_BUFFER_STRIDE;
}

// Glyph layout in that mapping, so a client can index it without being
// told anything beyond the metrics WIN_REQ_FONT returns: glyphs are
// stored back to back, each `h` rows of `w` bytes, row-major, one byte
// of coverage per pixel (0 = background, 255 = fully ink). Glyph 0 is
// ASCII 32 (space) and they run contiguously from there.
#define WIN_FONT_FIRST_CHAR 32

static inline uint64_t win_glyph_offset(uint32_t index, int w, int h) {
    return (uint64_t)index * (uint64_t)w * (uint64_t)h;
}

// How many events the server will hold for one client before it starts
// dropping the OLDEST. Dropping the oldest rather than the newest is
// deliberate: for input, the most recent state is the one that matters,
// and a client that has fallen far enough behind to overflow is better
// served by current events than by a backlog it will never catch up on.
#define WIN_EVENT_QUEUE_MAX 32

#endif
