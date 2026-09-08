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
                            // A PRESS. Autorepeat arrives as more of
                            // these, exactly as a terminal would send
                            // them. See WIN_EV_KEY_UP for the other
                            // edge, and why it is a separate type.
#define WIN_EV_MOUSE_MOVE 2 // a, b: position, window-relative
#define WIN_EV_MOUSE_DOWN 3 // a, b: position; mods: button bits -- 0x1
                            // primary (left), 0x2 secondary (right).
                            //
                            // BOTH BUTTONS REACH A CLIENT, inside its
                            // CONTENT area only: the frame, the title
                            // bar and the taskbar keep the WM's window
                            // menu on a right-click, and the content
                            // belongs to the app. Same split as
                            // Windows, X11 and Wayland. A MOVE while a
                            // button is held carries that button's bit,
                            // and the UP that ends the press carries 0
                            // (the buttons now down, not the one that
                            // was released).
                            //
                            // Toykit acts on 0x1 alone -- a secondary
                            // press never arms a widget or moves the
                            // focus -- and passes every button through
                            // to uapp_desc.on_press, because only the
                            // app knows what a right-click means to it.
                            //
                            // AND THE KEYBOARD MODIFIERS RIDE ALONG,
                            // shifted up by WIN_MOUSE_MODS_SHIFT. A
                            // click carries none of its own otherwise,
                            // and Ctrl+click / Shift+click are the
                            // multi-select idiom every desktop uses --
                            // so a client that had to reconstruct the
                            // modifier state from key events would be a
                            // third place for it to drift, and would be
                            // wrong the moment the window lost focus
                            // mid-chord. X11 puts both in one `state`
                            // word and Win32 puts MK_CONTROL beside the
                            // button flags in `wParam`; Wayland is the
                            // outlier that makes the client track it.
                            // Use WIN_MOUSE_BUTTONS()/WIN_MOUSE_MODS().
#define WIN_EV_MOUSE_UP   4 // a, b: position; mods: button bits
// Splitting a mouse event's `mods`. The low byte is the BUTTON mask
// (0x1 primary, 0x2 secondary, 0x4 middle); the next byte is the
// KEY_MOD_* bits held at the time. Both are bytes because both already
// were -- this widens no field and no struct.
#define WIN_MOUSE_MODS_SHIFT 8
#define WIN_MOUSE_BUTTONS(m) ((unsigned)(m) & 0xFFu)
#define WIN_MOUSE_MODS(m)    (((unsigned)(m) >> WIN_MOUSE_MODS_SHIFT) & 0xFFu)

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
// A KEY CAME UP. `a` is the code its PRESS produced and `mods` is the
// modifier state after the change (api/keyboard.h's transition queue).
//
// **A SEPARATE EVENT TYPE, NOT A FLAG ON WIN_EV_KEY.** A client written
// before this existed keeps working unchanged -- it never asked for this
// type and simply never sees one -- where a flag on the press would have
// made every existing client start receiving events it would decode as
// presses. That is the same promise uapp.h makes about optional
// callbacks, kept at the protocol layer where it has to be kept.
//
// WHY IT EXISTS: without it a client can only know a key was struck,
// never that it is HELD, and "is W down?" is the whole input model of a
// game. X11 sends KeyPress/KeyRelease, Wayland's wl_keyboard.key carries
// a pressed/released state (and wl_keyboard.enter hands a client the set
// already held), and Windows sends WM_KEYDOWN/WM_KEYUP -- press-only was
// this protocol's outlier, and text entry stays press-driven regardless,
// which is why this is an addition rather than a change.
//
// **THE FOUR MODIFIER KEYS ARRIVE ON BOTH EDGES** -- KEY_SHIFT, KEY_CTRL,
// KEY_ALT, KEY_ALTGR as WIN_EV_KEY and WIN_EV_KEY_UP -- because they
// produce no character and so have never produced an ordinary key event
// at all. Nothing else changes: a modifier still rides with the key it
// modified, and an app that only reads `mods` is unaffected.
#define WIN_EV_SCREEN    32 // a: width, b: height. THE SCREEN CHANGED
                           // SIZE (`config set resolution`). The
                           // compositor re-maps its framebuffer grant
                           // (WIN_REQ_FB_MAP again: the old addresses
                           // stay mapped, so a blit in flight lands on
                           // a page rather than a fault) and re-lays
                           // out; a client needs nothing, since its
                           // window is resized through WIN_EV_RESIZE
                           // like any other.
// 31 is UNUSED. It was WIN_EV_CLIPBOARD, broadcast when the kernel
// held the clipboard; the clipboard is a ring-3 service now
// (userland/lib/uclip_page.h) and a client reads its serial out of
// shared memory instead. The number is left dead rather than reused,
// so an old client cannot mistake a new event for it.

#define WIN_EV_USER      30 // a, b: whatever the CLIENT put there. The
                            // only event a client can put on its OWN
                            // queue (WIN_REQ_EVENT_PUSH with a target of
                            // 0), and the reason it exists is threads: a
                            // worker that has finished has no other way
                            // to wake a main thread parked in
                            // SYS_WAIT_EVENT, and polling for it on a
                            // tick is the cadence this whole mechanism
                            // is meant to delete.
                            //
                            // Qt's postEvent, GTK's g_idle_add, Win32's
                            // PostMessage, and the eventfd a Wayland
                            // client puts in its poll set are all this.
                            //
                            // The COMPOSITOR never sends one, and no
                            // client can send one to anybody else -- see
                            // WIN_REQ_EVENT_PUSH, where restricting the
                            // TYPE as well as the target is what keeps
                            // "a client cannot synthesise input" true.
#define WIN_EV_KEY_UP    27

#define WIN_EV_RAW_MOUSE 10 // a, b: SCREEN position; mods: button bits
                            // (bit0 = left, bit1 = right), level state.
#define WIN_EV_RAW_KEY   11 // a: key code (api/keyboard.h), mods:
                            // KEY_MOD_*. Pre-focus: no window has been
                            // chosen yet.
// The pre-focus counterpart of WIN_EV_KEY_UP, for a registered
// compositor: a key came up, and no window has been chosen yet.
// Modifier PRESSES arrive as WIN_EV_RAW_KEY, since the byte stream the
// ordinary raw keys come from cannot carry them.
#define WIN_EV_RAW_KEY_UP 28

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
#define WIN_EV_CLIENT_PRESENT   16 // a: pid, b: the FRONT buffer index
                                    // and that buffer's GENERATION,
                                    // packed by WIN_PRESENT_B(); mods:
                                    // that buffer's own WIDTH and
                                    // HEIGHT, packed by
                                    // WIN_PRESENT_SIZE(). A compositor
                                    // remembers this per window,
                                    // because it also repaints for its
                                    // own reasons (the clock, another
                                    // window) when no present has
                                    // arrived.
                                    //
                                    // **THE GENERATION NAMES THE
                                    // MEMORY.** The buffer's NAME
                                    // identifies the slot and never
                                    // changes; the object in it is
                                    // replaced on a resize, and the
                                    // generation goes up when it is. A
                                    // compositor holding a different one
                                    // re-opens the name; the object it
                                    // was reading stays alive under its
                                    // own mapping until it lets go,
                                    // which is wl_buffer.release. It
                                    // rides the present rather than an
                                    // event of its own because it is
                                    // STATE: a lost invalidation event
                                    // would be a compositor reading a
                                    // freed object forever, where a
                                    // stale generation costs one frame.
                                    //
                                    // **THE SIZE IS THE FRAME'S, NOT THE
                                    // WINDOW'S.** It is what makes a
                                    // resize invisible: a compositor
                                    // adopts the geometry of the pixels
                                    // it is about to show, so it never
                                    // has to guess whether this frame
                                    // was drawn before or after a
                                    // resize it proposed.
#define WIN_EV_CLIENT_DESTROYED 17 // a: pid. It is going away. THE
                                    // PIXELS ARE STILL READABLE: the
                                    // compositor's own mapping holds a
                                    // reference to the object, so the
                                    // frames go when it munmaps, not
                                    // when the window does
                                    // (wl_buffer.release).
#define WIN_EV_CLIENT_TITLE     18 // a: pid. Title changed; re-read it.
#define WIN_EV_CLIENT_HINTS     19 // a: pid. Hints changed; re-read.
#define WIN_EV_CLIENT_RESIZED   20 // a: pid, b: new w, mods: new h. The
                                    // client ACCEPTED a proposal: its
                                    // BACK buffer is now this size.
                                    // Nothing to re-map here -- the new
                                    // object is picked up by the
                                    // generation on the present that
                                    // first shows it. **Not the moment
                                    // to adopt the size** -- the front
                                    // buffer still holds the last frame
                                    // at the old one, and that frame is
                                    // what is on screen until the
                                    // present that carries the new size.
                                    // Useful for pacing: a compositor
                                    // resizing interactively can send
                                    // its next proposal when this
                                    // arrives.
#define WIN_EV_CLIENT_PONG      21 // a: pid, b: the serial echoed back.
#define WIN_EV_CLIENT_TIMER     22 // a: pid. This window's timer is due.
#define WIN_EV_CLIENT_CLOSE     23 // a: pid, window unused. Close every
                                    // window this pid owns -- the
                                    // desktop's own "quit that app".
#define WIN_EV_FONT      26 // No payload. THE FONT CHANGED -- a different
                           // face, or a different size. Call
                           // ugfx_font_init() again (the mapping is at a
                           // fixed address and re-mapping over it is a
                           // no-op in effect, so this needs no unmap),
                           // re-derive any layout measured from the cell,
                           // and repaint.
                           //
                           // Broadcast to EVERY window, the compositor's
                           // included, because font size is the one
                           // setting this whole UI is derived from --
                           // window chrome, the taskbar, icon pitch and
                           // every widget's natural size all come out of
                           // gfx_char_w()/gfx_char_h(). Without it a
                           // client keeps the metrics it mapped at
                           // startup and the Appearance setting appears
                           // to do nothing until the desktop restarts.
                           //
                           // This is what Wayland's wl_output scale
                           // change and X11's XSETTINGS notification are
                           // for; the shape is deliberately theirs --
                           // the server does not re-lay-out anybody, it
                           // says the metrics moved and each client
                           // decides what that means for it.
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
#define WIN_EV_CLIENT_CURSOR    29 // a: pid, b: the WIN_CURSOR_* shape.
                                    // The VALUE rides the event, unlike
                                    // the thin ones above: it is one int
                                    // and WIN_REQ_WINDOW_INFO has no
                                    // return slot left. Cost: a dropped
                                    // event is not recoverable by
                                    // re-reading, which the compositor's
                                    // content-area clamp bounds.

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
                           // `window` IN is the SLOT the client
                           // picked (0..WIN_CLIENT_MAX-1), whose buffer
                           // objects it has already created and granted;
                           // OUT it is the window's id, which is that
                           // slot. Refused if the slot is taken.                           //
                           // The app id rides CREATE rather than being
                           // a message of its own so that a window can
                           // never exist without it: an id registered a
                           // moment later leaves a gap in which a second
                           // copy of the same program asks "is anyone
                           // there?" and is told no. `text` was unused
                           // by CREATE, so this costs no bytes.
#define WIN_REQ_PRESENT 2 // `window`: which one. The client has finished
                           // drawing into its BACK buffer; make it the
                           // front one and composite it.
                           //
                           // **RETURNS THE NEW FRONT INDEX** (0 or 1),
                           // or a negative errno. The client draws into
                           // the other one from here on. A                           // single-buffered window (its second
                           // allocation failed) always answers 0, so a
                           // client needs no special case for it.
                           //
                           // The swap happens HERE rather than when the
                           // compositor gets round to reading, because
                           // a client must know which buffer is safe to
                           // draw into the moment this returns.
#define WIN_REQ_DESTROY 3 // `window`: which one. Closes it; the
                           // client's buffer objects are its own to
                           // unlink.
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
#define WIN_REQ_RESIZE  7 // a/b: requested content w/h, c: which BUFFER
                           // the client has already rebuilt at that
                           // size (-1 for "you choose"). On success a/b
                           // come back as the size actually granted.
                           //
                           // The client replaces the object BEFORE
                           // sending this, and names which one it
                           // replaced: both sides deriving it from
                           // `front` disagree the moment a present
                           // lands in between.                           //
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
#define WIN_REQ_FONT    5 // `window` IN: the weight wanted, 0 = regular,
                           // 1 = bold (WIN_FONT_REGULAR/WIN_FONT_BOLD).
                           // Maps that weight of the desktop's ACTIVE
                           // font read-only into the client at
                           // win_font_vaddr(weight) and fills in the
                           // metrics:
                           // a = glyph width, b = glyph BITMAP height,
                           // c = glyph count, d = the byte offset of
                           // glyph 0 within the mapping (the data does
                           // not necessarily start on a page boundary,
                           // so glyph 0 lives at the mapping base + d,
                           // not at the base).
                           // `window` OUT = the LINE PITCH.
                           //
                           // **`b` AND `window` ARE DIFFERENT NUMBERS
                           // AND CONFUSING THEM IS SILENT.** `b` is how
                           // many rows a glyph's coverage map has, so it
                           // is the stride between cells and the ONLY
                           // one win_glyph_offset() may be given -- pass
                           // the pitch and every glyph past the first is
                           // read from the wrong offset. `window` is how
                           // far apart two lines sit, which is what
                           // LAYOUT wants and what ugfx_char_h()
                           // returns.
                           //
                           // They differ because a glyph bitmap is
                           // taller than its line: the cell extends
                           // below the baseline far enough to hold a
                           // descender, while the pitch stays at the
                           // terminal-like height everything is laid out
                           // against. See struct font_atlas in
                           // api/font_face.h. For the BAKED font the two
                           // are equal -- its bitmaps were rasterized
                           // squeezed at build time, so it still clips.
                           //
                           // A font belongs to the SESSION, not to one
                           // window -- which is why `window` was free
                           // to become the weight. It is the one
                           // request whose `window` field never named
                           // a window.
                           //
                           // **A BOLD REQUEST NEVER FAILS FOR WANT OF A
                           // BOLD FILE.** A family with none gets its
                           // regular outlines emboldened (font_face.c,
                           // ttf_embolden) -- what GDI does, and what
                           // Cairo and DirectWrite fall back to. A
                           // client cannot tell, and does not need to.
                           // What DOES fail is asking for bold with no
                           // face loaded at all: the BAKED font has one
                           // weight, so the request returns 0 and the
                           // client keeps drawing regular.
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
                           //
                           // `mods` OUT carries the byte offset of the
                           // ADVANCE table within the mapping, or 0
                           // when there is none (the baked tables carry
                           // no advances -- every cell is `a` wide).
                           // 0 is unambiguous because a table can never
                           // START the mapping: the glyph data does.
                           //
                           // **THE KERN TABLE IS DERIVED, NOT
                           // RETURNED.** A runtime atlas lays its three
                           // sections back to back in a fixed order --
                           // glyphs, advances, kern -- so the kern
                           // matrix begins at `mods + c` and there is
                           // no field for it. That order is ABI (see
                           // struct font_atlas in api/font_face.h);
                           // reordering the blob silently hands every
                           // client the wrong table rather than failing.
                           // `mods == 0` means no advances AND no kern.
                           // win_font_kern_offset() below is the one
                           // place that arithmetic is written down.

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
                           // BYTES, d = bits per pixel. On return
                           // `mods` = how many SCANOUT buffers were
                           // mapped (1, or 2 on a display that can
                           // flip), buffer i at WIN_FB_VADDR + i *
                           // WIN_FB_BUFFER_STRIDE, and `window` = the
                           // index of the BACK buffer -- the one to
                           // draw into next. With one buffer both are
                           // trivially 1 and 0, and the screen belongs
                           // to the session, not to a window.
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
// **20 IS RETIRED, NOT FREE.** It was WIN_REQ_MAP_WINDOW: the kernel
// mapping a client's pixels into the compositor at an address it carved
// per (pid, window). The compositor opens the buffer's NAME itself now
// and maps it wherever its own mmap puts it, so there is nothing to ask
// the kernel for -- and no kernel-held mapping to revoke, which is what
// retired the poison page with it. Retired 2026-09-08.
#define WIN_REQ_BUFFER 27  // `window`: which one; a: which BUFFER (0 or
                           // 1); b, c: its new width and height.
                           //
                           // **THE CLIENT REPLACED THAT BUFFER'S
                           // OBJECT** -- unlinked the old one and made a
                           // new one under the same name -- and this is
                           // what makes the server re-adopt it. The name
                           // identifies the SLOT; the object in it is
                           // what changes, which is why the server
                           // cannot notice on its own.
                           //
                           // The server used to grow the stale buffer
                           // itself at present time. It cannot: the
                           // memory is the client's, and only the client
                           // can replace an object it created.
                           //
                           // A buffer is replaced BEFORE it is drawn
                           // into, never while it is the front one --
                           // the front still holds the last finished
                           // frame, and taking it away is the window of
                           // black the configure/ack handshake exists to
                           // avoid.
// **26 IS RETIRED, NOT FREE.** It was WIN_REQ_UNMAP_WINDOW, the release
// half of the pair above: the compositor's mapping held the frames of a
// destroyed window alive, so it had to tell the kernel when it was done
// with them. It munmaps its own mapping now, and the shm object's own
// reference count does the rest. Retired 2026-09-08.
// **WIN_REQ_TITLE, _HINTS AND _CURSOR ARE NOT CARRIED BY THE KERNEL.**
// They travel client -> compositor over a channel
// (userland/lib/uwmchan.h), keeping their numbers and their meanings --
// the protocol is TWP either way, and which carriage a message takes is
// exactly what this header says is expected to change.
//
// They moved because the kernel was STORING them. A payload does not fit
// in a 24-byte struct win_event, so the kernel kept the title, the hints
// and the cursor shape only to hand them back through
// WIN_REQ_WINDOW_INFO -- state it had no use for, held so that a
// compositor could ask a second time for something a client had already
// said.
// **19 IS RETIRED, NOT FREE.** It was WIN_REQ_WINDOW_INFO: a window's
// title, hints and geometry, read back by a compositor that had been
// told only that something changed. The title and the hints went to the
// channel with their payloads; the geometry was the last thing left, and
// its only caller was asking for a size WIN_EV_CLIENT_CREATED had
// already handed it. Retired 2026-09-08. A number reused here would
// land an old client's request on a different message, which is the
// same reason abi/syscall_abi.h keeps its own holes.
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
                           // just written. Publishes it. On a display
                           // with two scanouts this FLIPS to the buffer
                           // the caller was told to draw into and
                           // returns the NEW back index in `window`;
                           // the caller must then treat that buffer's
                           // contents as two frames old (Wayland's
                           // buffer_age) and repaint the union of the
                           // last two frames' damage into it.
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
// --- the cursor a client wants under the pointer -----------------------
//
// The client NAMES a shape, the compositor draws it: Wayland's
// cursor-shape-v1, Win32's SetCursor.
//
// THE RESIZE SHAPES ARE THE FRAME'S *AND* A CLIENT'S. They were the
// frame's alone until a client had a divider of its own to drag
// (ui/uui_splitter.h); the compositor still wins wherever the two
// overlap, because a window edge is geometry it owns.
#define WIN_CURSOR_DEFAULT  0
#define WIN_CURSOR_TEXT     1 // I-beam: an insertion point lives here
#define WIN_CURSOR_WAIT     2 // busy: this window is working, wait for it
#define WIN_CURSOR_RESIZE_H 3 // a divider that moves left/right
#define WIN_CURSOR_RESIZE_V 4 // a divider that moves up/down
#define WIN_CURSOR_COUNT    5

// THE CLIPBOARD IS NOT HERE ANY MORE. It was a buffer in
// kernel/proc/win_server.c reached by SYS_WIN_CLIP, holding untrusted
// user data and a piece of desktop policy in ring 0. It is a named
// shared-memory object owned by /bin/clipboardd now, so it is a
// userland<->userland contract and lives in userland/lib/uclip_page.h,
// which records why. Request numbers 26 and 27 are left dead.

#define WIN_REQ_FB_CURSOR  25 // COMPOSITOR ONLY. The hardware cursor
                           // plane (virtio-gpu's cursorq, vmsvga's
                           // FIFO cursor). a: a WIN_FB_CURSOR_* op.
                           // DEFINE: the sprite's pixels are straight
                           // ARGB, b/c = user pointer low/high 32 bits
                           // (the request fields are int32_t and a
                           // ring-3 address is not), d packs the
                           // geometry as (w<<24)|(h<<16)|(hotx<<8)|hoty
                           // -- each fits in 8 bits because the plane
                           // is 64x64. QUERY returns 1 when a plane
                           // exists; HIDE/SHOW flip it, and SHOW also
                           // ARMS the kernel to move the plane from
                           // win_input.c on every pointer event -- the
                           // zero-syscall path that makes motion cost
                           // the compositor nothing at all.
#define WIN_FB_CURSOR_HIDE   0
#define WIN_FB_CURSOR_SHOW   1
#define WIN_FB_CURSOR_DEFINE 2
#define WIN_FB_CURSOR_QUERY  3

#define WIN_REQ_CURSOR     24 // `window`: which one; a: a WIN_CURSOR_*.
                           // Honoured only inside that window's content
                           // area, so a client that never resets cannot
                           // strand a shape elsewhere.
                           //
                           // Set per MOTION, not once: a window is not
                           // uniformly one thing. Idempotent, and
                           // uapp_set_cursor() drops the no-op rather
                           // than sending one per mouse move. An unknown
                           // shape is refused, not clamped.
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
// A RING-3 CALLER IS NEVER MADE TO WAIT IN THE KERNEL. When the
// answering window manager is itself a ring-3 process, the command is
// POSTED to it and this flag comes straight back with no reply text:
// ask again with WIN_REQ_DEBUG_MORE until it clears. The serial console
// does not see it -- it has no scheduler slot, so it can and does wait
// in place, which a syscall may not (api/scheduler.h: waiting in place
// with interrupts on "was tried, and hangs after one event"). Handing a
// process that same wait is a #GP inside isr_common, measured.
#define WIN_DEBUG_F_PENDING 0x04 // no answer yet; poll with DEBUG_MORE

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
//   1. The user drags the resize grip. The WM clamps to the client's
//      hinted minimum and sends WIN_EV_RESIZE(w, h) -- a proposal, one
//      in flight at a time, the last one on release.
//   2. The client answers with WIN_REQ_RESIZE. The server rebuilds the
//      BACK buffer at the new size and maps it at the same virtual
//      address, so the client's buffer pointer survives. **The FRONT
//      buffer is left alone**, still holding the last finished frame at
//      the size it was drawn at -- which is what the compositor goes on
//      showing.
//   3. The client redraws at the new size and presents. The server
//      flips, and the present event carries the new front buffer's own
//      dimensions (WIN_PRESENT_SIZE). The WM adopts the geometry THERE,
//      with the pixels in hand.
//   4. If the server refuses (out of contiguous memory, over
//      WIN_CLIENT_MAX_*), the client keeps the size it had and the
//      window does not change. A refusal is a normal outcome, not an
//      error path.
//
// Step 2's asymmetry is the reason a resize here does not flash. The
// obvious implementation rebuilds both buffers and tells the compositor
// at once -- and then the compositor paints a freshly zeroed window for
// the whole round trip, which measured 100-240 ms under TCG and reads
// as the window going black. Keeping the size ON THE BUFFER is
// Wayland's answer to the same problem (a wl_buffer carries its
// dimensions; a surface adopts them at commit); X11's resize-then-
// repaint is the shape that flickers.
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

// Largest client window, in pixels. **It tracks the largest mode the
// display layer will select (DISPLAY_MAX_W/H in
// kernel/drivers/display/display.c), not the mode boot.asm requests** --
// so a client can be dragged or maximized to fill the screen and the
// window manager's own screen-bounds clamp (wm_update_drag_resize())
// becomes the effective limit rather than this one. That is the point:
// a resize past the cap is refused SILENTLY, since a refusal is a normal
// protocol outcome and looks identical to a client that simply declined.
//
// **This is the constant a bigger screen breaks FIRST, and it breaks
// quietly.** It was 1280x720, matching what GRUB was asked for; the
// moment a modesetting driver (bochs.c, vmsvga.c) came up at 1920x1080,
// maximize asked for 1918x1038, the server refused, and the window
// wore full-screen chrome around a stale 1280x720 buffer with undrawn
// desktop filling the difference -- exactly the failure this header
// describes for the resize grip. So: raise it WITH DISPLAY_MAX_W/H,
// or the display gains pixels no window can use.
//
// It is 1080p rather than the 4K DISPLAY_MAX_W/H now allows, and that
// gap is deliberate. The server allocates and maps the whole buffer up
// front and does it CONTIGUOUSLY (see create_window()): 1920x1080x4 is
// 2025 frames from pmm_alloc_contiguous() per window, and a resize
// allocates the new buffer BEFORE freeing the old, so a 4K window would
// ask a fragmented allocator for 8100 contiguous frames twice over.
// Dropping the contiguity requirement is docs/roadmap.md's growable
// client buffers item, and it is the prerequisite for raising this to
// the display ceiling -- not this constant getting bigger on its own.
#define WIN_CLIENT_MAX_W 1920
#define WIN_CLIENT_MAX_H 1080

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

// --- a window's pixels: a NAMED OBJECT, not an address ----------------
//
// A window buffer is a shared-memory object (SYS_SHM_OPEN) the CLIENT
// creates, maps wherever its own mmap put it, and grants to the
// compositor with SYS_SHM_GRANT. Neither side derives an address: the
// client knows where it mapped its own, and the compositor opens the
// name. This is wl_shm_pool -- the buffer is the client's, handed over
// rather than reached into.
//
// **THE NAME IDENTIFIES THE SLOT; THE GENERATION IDENTIFIES THE
// MEMORY.** A resize unlinks the object and creates a new one under the
// same name, so the name is stable for the life of the window while the
// object behind it is not -- see WIN_EV_CLIENT_PRESENT. The name is
// guessable and that is harmless: an object belongs to its creator
// (abi/syscall_abi.h's SHM_PUBLIC), so nothing but a grant gets in.
//
// A FORMAT rather than a function because the two sides that build it
// are in different rings with different snprintf()s, and because the
// kernel builds it at all any more.
#define WIN_BUF_NAME_FMT "win.%d.%d.%d"  // owner pid, window id, buffer
#define WIN_BUF_NAME_MAX 32              // and <= SHM_NAME_MAX

// The addresses these buffers used to live at are GONE: WIN_CLIENT_BASE
// with its 64 MiB per-window stride, WIN_BUFFER_HALF's second-buffer
// offset, and WIN_COMPOSITOR_BASE's 16 GiB carve-out of one slot per
// (pid, window). A client mmaps its own pixels and a compositor mmaps
// what it was granted, so a fixed address would only be something for
// the two to disagree about.

// A PRESENT EVENT'S `mods`: the front buffer's own size, width in the
// high half and height in the low one. Both are bounded by
// WIN_CLIENT_MAX_W/H (1920x1080), so 16 bits each is room to spare --
// and packing beats widening struct win_event, which is on the path of
// every event the system delivers.
#define WIN_PRESENT_SIZE(w, h) ((((uint32_t)(w) & 0xFFFFu) << 16) | \
                                 ((uint32_t)(h) & 0xFFFFu))
#define WIN_PRESENT_W(m)       ((int)(((uint32_t)(m) >> 16) & 0xFFFFu))
#define WIN_PRESENT_H(m)       ((int)((uint32_t)(m) & 0xFFFFu))
#define WIN_CLIENT_MAX    4 // windows one client may hold at once

// A PRESENT EVENT'S `b`: which buffer is now the front one, and that
// buffer's GENERATION -- the number that goes up each time the client
// replaces the object behind the name. One field because struct
// win_event has no spare one, and a buffer index is one bit.
#define WIN_PRESENT_B(buf, gen) ((int32_t)(((uint32_t)(gen) << 1) \
                                            | ((uint32_t)(buf) & 1u)))
#define WIN_PRESENT_BUF(b)      ((int)((uint32_t)(b) & 1u))
#define WIN_PRESENT_GEN(b)      ((uint32_t)(b) >> 1)


// Where WIN_REQ_FONT maps the shared glyph data. A literal since the
// window region it used to be derived from went away; it stays where it
// was so a client built against either side of that change maps the
// font at the same address.
#define WIN_FONT_VADDR 0x8090000000ULL

// The weights a client may ask for, and the stride between their
// mappings. Both are mapped AT ONCE and stay mapped -- that is what
// distinguishes a weight from a size here: the machine is only ever at
// one size, and a widget picks a weight per run of text.
#define WIN_FONT_REGULAR 0
#define WIN_FONT_BOLD    1
#define WIN_FONT_WEIGHTS 2

// 4 MiB per weight, which is font_face.c's whole atlas cache budget --
// so no single atlas can overrun its slot. Costs nothing but address
// space: nothing is mapped until a client asks.
#define WIN_FONT_STRIDE  0x0000400000ULL

static inline uint64_t win_font_vaddr(int weight) {
    return WIN_FONT_VADDR + (uint64_t)weight * WIN_FONT_STRIDE;
}

// Where the kern matrix starts within a font mapping, given the
// advance-table offset WIN_REQ_FONT reported in `mods` and the glyph
// count it reported in `c`. 0 when the mapping has no advance table,
// which means it has no kern table either (the baked font).
//
// Written once, here, because the alternative is the same `mods + c`
// appearing in gfx code, in the toolkit, and in every test -- and a
// layout change would then have to find all of them.
static inline uint64_t win_font_kern_offset(uint32_t adv_off, int count) {
    return adv_off ? adv_off + (uint64_t)count : 0;
}

// The kern matrix is count x count SIGNED BYTES indexed
// [left * count + right] by glyph SLOT -- the same slot index the
// glyph and advance tables use, not a glyph id (a client has no cmap).
static inline int win_font_kern(const signed char *kern, int count,
                                 int left_slot, int right_slot) {
    if (!kern || left_slot < 0 || right_slot < 0
        || left_slot >= count || right_slot >= count) return 0;
    return kern[left_slot * count + right_slot];
}

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
// Scanout i of the grant is at WIN_FB_VADDR + i * this. 64 MiB holds a
// 4K buffer (31.6 MiB) with room.
#define WIN_FB_BUFFER_STRIDE 0x4000000ULL

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
// dropping the OLDEST INPUT event (and only with none of those queued,
// the oldest of all). Dropping the oldest rather than the newest
// is deliberate: for input, the most recent state is the one that
// matters, and a client that has fallen far enough behind to overflow
// is better served by current events than by a backlog it will never
// catch up on. Mouse motion (raw, and a client's WIN_EV_MOUSE_MOVE) is
// coalesced into one slot before any of this applies (win_events.c),
// and a notification such as WIN_EV_SCREEN is never the one shed while
// input is waiting.
#define WIN_EVENT_QUEUE_MAX 32

#endif
