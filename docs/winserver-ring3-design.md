# The window server out of the kernel

A staged plan, in the shape `docs/query-design.md` and
`docs/wm-ring3-design.md` used. It answers "what is still in ring 0 that
belongs to the compositor, and in what order does it leave?"

`docs/wm-ring3-design.md` closed the GUI-in-ring-3 milestone: the *presentation* half
of TWS became a process (`userland/wm/`) and `apps/wm/` was deleted.
What that milestone deliberately did not move is the *memory* half --
`kernel/proc/win_server.c` and the files beside it -- and that is what
this plan is about.

**The per-stage markers are the authority**, not any status sentence at
the top of this file.

## What already exists, measured

`kernel/proc/win_*.c`, tests excluded, 2026-09-08:

| file | lines | what it owns |
|---|---|---|
| `win_server.c` | 1912 | window ids, pixel buffers, per-process mappings, ownership, teardown |
| `win_syscalls.c` | 443 | `SYS_WIN_REQUEST`, the event queue syscalls, and a legacy single-window path |
| `win_surface.c` | 195 | the compositor's framebuffer grant |
| `win_events.c` | 151 | the per-process event queue |
| `win_input.c` | 126 | raw input to whoever holds the compositor role |
| `win_transport.c` | 59 | the carriage registry |

Three facts shape everything below.

**The ring-0 presentation layer is already dead.** Nothing calls
`win_server_register()` outside a KTEST; `g_ops` is NULL on every real
boot. The compositor is told about a window through
`tell_compositor()`, which queues a `WIN_EV_CLIENT_*` event.

**The kernel stores presentation state it has no use for.** `struct
client_window` holds `title`, `app_id`, `hint_flags` and `cursor`, and
its own comment says why: a ring-3 compositor "is TOLD a window changed
and reads the detail back (`WIN_REQ_WINDOW_INFO`), so the detail has to
live somewhere". That is not laziness. `struct win_event` is a fixed 24
bytes and is on the path of every key and every pointer motion -- a
present already *packs* width and height into its `mods` field to avoid
widening it. A 32-byte title does not fit, so the kernel holds it and
answers a second round trip.

**The transport seam has one implementation and is therefore
unvalidated**, which `win_transport.h` and `docs/decisions/kernel.md`
both say in those words, naming batching and reply-ownership as what is
most likely wrong with it.

## The carriage: a shared-memory ring, and why not the other two

Settled with the maintainer, 2026-09-08. Recorded here so it is not
reopened.

**The traffic is overwhelmingly one-way.** Only `WIN_REQ_CREATE` and
`WIN_REQ_WINDOW_INFO` need an answer. `PRESENT`, `TITLE`, `HINTS`,
`CURSOR`, `TIMER` and `PONG` are fire-and-forget, and `PRESENT` runs
once per frame per client.

So a *synchronous* carriage -- forward the request to the compositor
process and block the client for its reply -- is the wrong shape for
almost all of it. It would turn every frame into a scheduler round trip,
which is worse than today, where the present syscall queues an event and
returns at once.

**No real system does that.** Wayland's requests are asynchronous by
design; only an explicit `wl_display.sync` round-trips. Android is the
sharper precedent because it has both mechanisms and chose between them:
control goes over Binder, frames go over a shared-memory BufferQueue.
Binder is not used for buffer submission.

A general Binder-shaped IPC port is still worth having -- `/bin/service`
implements `start`/`stop` by appending to `/run/init.ctl` and sending
`SIGHUP`, with a source comment naming what it would rather have been:
"systemd's D-Bus and `/run/initctl`'s FIFO both need transports this
system has not got". It does report an outcome, by polling
`/run/init.status` until the state changes -- an earlier revision of
this file said it could not, which its own header disproves. What a
channel replaces is the file plus the doorbell, not the reporting. The
decision is that the ring is built as a **general named channel** --
async messages plus a reply slot -- rather than something window-shaped,
so that caller can retire onto it later without a second mechanism.
Wayland's split: the transport is general, the protocol on top is not.

## Staging

Each stage builds, boots and passes the existing suites on its own.

### Stage 0 -- shm sized for a window buffer -- DONE 2026-09-08

`SYS_SHM_OPEN`'s objects capped at 16 objects of 64 pages (256 KiB),
because each object carried a fixed `frames[64]` array. A 1920x1080
window's two buffers are ~4050 pages.

The frame array is allocated to the size asked for now, so the caps are
free to be what the callers need: 288 objects (256 is every window this
kernel can hold, `windows[64][4]`, plus the services beside them), 32
MiB each, 640 live mappings.

### Stage 1 -- window buffers are shm objects -- DONE 2026-09-08

`create_window()` and `rebuild_buffer()` took their pixels from
`pmm_alloc_contiguous()`. Nothing needed the frames adjacent -- every
mapping on both sides is page-granular -- and the demand was itself a
failure mode: `docs/wm-ring3-design.md` listed "growable,
non-contiguous client buffers" as out of scope precisely because a
fragmented allocator could refuse a large window, silently by design.

A window's frames are a nameless shm object now. **Nameless, not named**,
and that is load-bearing: the shm namespace has no permissions ("any
process may open any name"), so a window buffer with a name would be a
way for any process to map somebody else's window. `shm_lookup()`
refuses an anonymous object and `shm_create_anon()` is the only way to
make one.

What it buys beyond the fragmentation fix: the frames are **refcounted**,
which is the mechanism stage 2 needs.

### Stage 2 -- the compositor holds its own reference -- DONE 2026-09-08

The kernel revoked the compositor's view of a dying window
synchronously, and the compositor may still blit that slot before it
drains `WIN_EV_CLIENT_DESTROYED` -- so the slot was remapped to a shared
read-only poison page rather than left as a hole, and read as BLACK for
a frame on every close.

The compositor's mapping takes a reference to the window's shm object
now, and the frames go at the last one. A destroyed window's slot is
RETIRED: still mapped, still carrying the last frame the window drew,
and out of use until the compositor sends `WIN_REQ_UNMAP_WINDOW`. That
request is what `win_server_unmap_from_compositor()` had been waiting
for since stage 1 -- it existed with no protocol path and no caller.
This is `wl_buffer.release`.

**The poison page did NOT retire, and that is the honest finding.** The
slot address is derived from (pid, window id) and the slots are recycled,
so a client that closes and reopens faster than the compositor drains
its queue can find every slot retired. Taking one back is right; taking
it back as a HOLE would fault the compositor mid-frame. So the reclaim
path poisons, exactly as every close used to -- it is the rare path now
instead of the common one, and it has a KTEST of its own because an
unreachable fallback is a guess.

**The trap, found by a test:** the compositor's reference records WHICH
object it was taken on. A resize replaces the object while the reference
is still held on the old one, so releasing `bufs[b].shm` puts the NEW
object and frees it under a live mapping. `comp_ref_shm[]` is that
record.

### Stage 3 -- the general named channel

A ring of fixed-size messages in shared memory, a wakeup when it goes
non-empty, and a reply slot for the messages that need one. Async by
default.

**The wakeup is a FUTEX -- BUILT 2026-09-08.** `SYS_FUTEX_WAIT` /
`SYS_FUTEX_WAKE`, sitting straight on the scheduler's existing
address-keyed block (`api/scheduler.h`: "a channel is just an address").
It was chosen over posting to the receiver's event queue because that
queue is the WINDOW system's, and a channel woken through it could not
serve `/bin/service` -- which is the caller that makes this general
rather than window-shaped.

**The key is the PHYSICAL address**, which is the whole of the design:
two processes map a shm page at addresses nothing makes equal, so a key
built from the caller's pointer parks them on two unrelated channels and
a wake reaches nobody. Linux keys a shared futex on (inode, offset) for
the same reason. `scheduler_wake_n()` gained a count at the same time,
because releasing every waiter on a contended lock so all but one parks
again is the herd this channel mechanism exists to avoid.

It retires a roadmap item with two callers of its own: tolibc's mutex
spins and yields today, and a detached thread's stack cannot be
reclaimed without one.

**THE RING IS BUILT (2026-09-08), and it needed no kernel support at
all** -- `userland/lib/uchan.h` over shm, the futex and the wakeword,
each of which exists for its own reasons. That is Wayland's split: the
transport is general, the protocol on top is not, and the compositor is
a process like any other.

TWO OBJECTS per service, `/bin/soundd`'s shape because the same two
problems recur: a BEACON (`<service>`) holding the word a client wakes
the server through, and ONE RING PER CLIENT (`<service>.<pid>`), which
the server finds by walking `QUERY_SHM`. A ring per client is what makes
it lock-free -- one writer and one reader each, so `head` and `tail` are
each written by a single process and neither side needs a
compare-and-swap, which this system does not have.

Asynchronous by default; `uchan_call()` is the explicit round trip, for
the few messages that have an answer.

**IT HAS A PRODUCTION CALLER (2026-09-08): init.** `/bin/service`'s
`start` and `stop` travel over a channel and get a result back, instead
of appending to `/run/init.ctl` and ringing `SIGHUP`. That caller is
deliberately NOT the window system: it validates the channel, the
wakeword and the futex together, on something whose failure is a service
that will not start rather than a desktop that will not draw. The old
file-and-doorbell path is kept and stays producible.

It also gave the wakeword its second source. init could not serve
requests and reap children at once -- it parked in `waitpid(-1)`, which
a channel cannot wake -- so a child's death bumps the wakeword now, from
the one function both kinds of death funnel through.

### Stage 3b -- one wait over both -- THE PRIMITIVE IS BUILT, 2026-09-08

`SYS_WAIT_READY` parks on the process's event queue; a futex parks on a
word; a compositor needs to wake on either, and there is no `poll()`
here to build that from.

**`SYS_WAKEWORD` names ONE word a process waits on for everything.** The
kernel bumps it and wakes it whenever it queues a window or input event;
anything sharing the page -- a channel sender in another process -- does
the same. The waiter parks on that single word and, when it wakes, looks
at all of its sources. This is the self-pipe trick, or eventfd, in futex
form: what an event loop without a unified poll turns into. A third
source later (a signal, a timer) bumps the same word and needs nothing.

`poll()` over file descriptors is what Linux does and what every Wayland
compositor and the X server actually call. It was considered and is the
bigger job by a long way: neither the event queue nor a channel is a
file descriptor here, and both would have to become one first.

**IT HAS NO PRODUCTION CALLER YET, which makes it unvalidated by this
repo's own rule** -- the same thing `win_transport.h` says about itself.
A KTEST proves the event path bumps it (and reddens when that call is
removed), but the caller it exists for is the compositor, and the
compositor gains nothing from it until there is a channel to wait on as
well. That arrives with the ring below.

### Stage 4 -- presentation state moves -- TITLE DONE 2026-09-08

`WIN_REQ_TITLE` travels over the channel with its payload
(`userland/lib/uwmchan.h`), so the compositor is handed the string
instead of being told "it changed" and reading it back with
`WIN_REQ_WINDOW_INFO`. **The message keeps its `WIN_REQ_*` number**:
the protocol is still TWP and only the carriage differs, which is the
bet `abi/win_proto.h` describes.

**ORDERING IS THE TRAP, and it is handled by drain order.** A window is
created through the KERNEL and announced on the event queue; its title
arrives on the channel. Two carriages have no order between them, so the
compositor drains the EVENT QUEUE FIRST every frame -- enough, because a
client cannot send a title before its create returned.

Both paths are proven independently: with the kernel fallback removed
the title still arrives, and with the channel disabled it still arrives.

**THE KERNEL STORES NONE OF IT NOW (2026-09-08).** `title`,
`hint_flags`, `min_w`, `min_h` and `cursor` are gone from `struct
client_window`, the three request handlers are gone from
`win_server_request()`, the three `WIN_EV_CLIENT_*` notifications are
gone, and `WIN_REQ_WINDOW_INFO` answers GEOMETRY only -- the size, which
is the kernel's because it owns the buffer.

`app_id` stays, and deliberately: it rides `WIN_REQ_CREATE` so a window
can never exist without one, and `WIN_REQ_ACTIVATE` matches on it. A
window briefly nameless is the gap that makes a single-instance app miss
its own twin and exit without ever drawing.

The Toykit fallback is gone with the storage. A client whose channel
could not open now has no title, hints or cursor shape rather than a
slower path -- which in practice means the compositor published no
beacon, and that is a desktop that is not working anyway.

### Stage 4 -- the rest of the presentation state

`TITLE`, `HINTS`, `CURSOR`, `TIMER`, `PONG` and `ACTIVATE` travel over
the channel with their payloads. The kernel drops `title`, `app_id`,
`hint_flags` and `cursor` from `struct client_window`, and
`WIN_REQ_WINDOW_INFO` retires -- the compositor is the authority on what
a window is, rather than a reader of the kernel's copy.

`app_identity` is the exception and stays: it is taken from the
scheduler at create time precisely because a client must not be able to
declare it.

### Stage 5 -- the client allocates its own buffer

`win_buffer_vaddr(id)` derives the client's buffer address from the
window id, which `docs/decisions/kernel.md` records as deliberate --
"there is no address to re-negotiate when the transport changes". Once
the client creates the shm object itself and names it to the compositor,
the address is whatever `mmap` returned, and `win_compositor_vaddr()`'s
carved per-pid region goes with it. This is `wl_shm_pool`.

**Needs:** stage 3, since the name has to reach the compositor.

### Stage 6 -- delete the kernel's window table

What is left in ring 0 is what must be: the framebuffer grant
(`win_surface.c`, a device mapping) and raw input delivery to whoever
holds the compositor role (`win_input.c`, evdev's job). `win_server.c`'s
window table, `win_server_ops` and the dead ring-0 presentation layer go.

## Out of scope

- **Moving input.** A compositor reading raw devices itself is what
  Linux does *not* do either -- evdev is in the kernel and Wayland
  compositors read it through `/dev/input`. The kernel keeps this.
- **The network stack, the filesystem and USB.** Considered and
  declined, 2026-09-08: Linux and Windows keep all three in the kernel,
  and xHCI DMA from ring 3 without an IOMMU is a downgrade.

## Revision history

- 2026-09-08: written. Stages 0 and 1 built the same day. The carriage
  and the ordering were settled with the maintainer before any code
  changed; the argument that decided it was that only two of eight TWP
  requests need a reply, which rules out a synchronous carriage for the
  per-frame path.
