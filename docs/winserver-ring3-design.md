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
the top of this file -- and they are on the SUBHEADINGS where a stage
split (6a/6b/6c). Reading the stage headings alone says stage 6 is
outstanding; it is not, and on 2026-09-20 that cost a session a wrong
recommendation about what to work on.

## EVERY STAGE IS DONE (0 through 8, finished 2026-09-09)

`win_server.c` does not exist. Neither do `win_events.c` or
`win_transport.c`. What is left in `kernel/proc/`, tests excluded, is
what this plan always said must stay -- a device mapping and evdev's
job:

| file | lines | what it owns |
|---|---|---|
| `win_role.c` | 502 | the compositor ROLE: who holds it, the framebuffer map, the cursor plane, the glyph tables, the pointer warp |
| `win_surface.c` | 345 | the compositor's framebuffer grant |
| `win_syscalls.c` | 193 | `SYS_WIN_REQUEST` |
| `win_input.c` | 197 | raw input to whoever holds the role |

The whole `WIN_EV_CLIENT_*` family is retired in `abi/win_proto.h`:
every one of those events travels on the client's own channel now
(`userland/lib/uchan.h`).

## What it looked like before, measured

`kernel/proc/win_*.c`, tests excluded, measured after stage 5b
(2026-09-08) -- kept because the stages below are written against it:

| file | lines | what it owns |
|---|---|---|
| `win_server.c` | 1498 | window ids, buffer sizes and generations, ownership, teardown |
| `win_syscalls.c` | 282 | `SYS_WIN_REQUEST`, the event queue syscalls, and a legacy single-window path |
| `win_surface.c` | 195 | the compositor's framebuffer grant |
| `win_events.c` | 156 | the per-process event queue |
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

**ALL OF THIS RETIRED IN 5b**, and the entry is kept for the reasoning
rather than the mechanism: the compositor maps a client's object itself
now, so it holds a real reference by mapping rather than a recorded one,
nothing can be revoked underneath it, and neither the poison page nor a
retired slot has anything left to protect against.

**The trap, found by a test:** the compositor's reference recorded WHICH
object it was taken on. A resize replaces the object while the reference
is still held on the old one, so releasing `bufs[b].shm` puts the NEW
object and frees it under a live mapping. `comp_ref_shm[]` is that
record.

### Stage 3 -- the general named channel -- DONE 2026-09-08

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

**`WIN_REQ_WINDOW_INFO` IS RETIRED (2026-09-08).** Once the title and
the hints left, its only caller was the compositor asking for a size
`WIN_EV_CLIENT_CREATED` had already carried to it in the same event --
a round trip that existed only because the one message used to fetch
all three. The number is retired rather than reused, as the deleted
syscalls' are.

### Stage 4 -- the rest of the presentation state -- DONE, WITH 6b

**Described as its own stage and delivered inside another**, which is
why it carried no marker until 2026-09-20: `TITLE`, `HINTS`, `CURSOR`,
`TIMER`, `PONG` and `ACTIVATE` went to the channel, and the fields this
paragraph says to drop went with the whole of `struct client_window`
when 6b deleted the table. `WIN_REQ_WINDOW_INFO` retired on 2026-09-08.
Nothing below is outstanding; it is kept because it states WHY the
compositor is the authority on what a window is rather than a reader of
the kernel's copy.

What it said, as written:

`TITLE`, `HINTS`, `CURSOR`, `TIMER`, `PONG` and `ACTIVATE` travel over
the channel with their payloads. The kernel drops `title`, `app_id`,
`hint_flags` and `cursor` from `struct client_window`, and
`WIN_REQ_WINDOW_INFO` retires -- the compositor is the authority on what
a window is, rather than a reader of the kernel's copy.

`app_identity` is the exception and stays: it is taken from the
scheduler at create time precisely because a client must not be able to
declare it.

### Stage 5 -- the client allocates its own buffer -- DONE 2026-09-08

`win_buffer_vaddr(id)` derived the client's buffer address from the
window id, which `docs/decisions/kernel.md` recorded as deliberate --
"there is no address to re-negotiate when the transport changes". The
client creates the shm object itself and names it to the compositor
now, so the address is whatever `mmap` returned, and both carved
regions are gone: `WIN_CLIENT_BASE`'s per-window slots and
`WIN_COMPOSITOR_BASE`'s per-(pid, window) ones. This is `wl_shm_pool`.
**UNBLOCKED 2026-09-08.** It needed shm objects to have an owner, which
they now do -- without that, a client naming its pixel buffer would make
every window world-readable, which is the hole stage 1 avoided by making
those objects anonymous.

#### The naming decision, settled

**THE CLIENT PROPOSES ITS OWN SLOT, and the buffer is `win.<pid>.<slot>`.**
`struct win_request_msg`'s `window` field is an OUTPUT on
`WIN_REQ_CREATE` and free as an input, so the client picks a slot in
0..WIN_CLIENT_MAX-1 and the kernel accepts it or refuses it as taken.
That leaves `text` carrying `app_id`, which must keep riding CREATE so a
window is never briefly nameless (`WIN_REQ_ACTIVATE` matches on it), and
it means no field has to be added to the hot-path message.

The name is guessable, and that is now harmless: an object belongs to
its creator, and the compositor gets in because the client GRANTS it --
`sys_shm_grant()` with the pid from the `toywm` beacon, which Toykit
already opens for the channel.

#### Why it splits in two, and where the difficulty is

Only ONE place in a client computes its drawing surface
(`userland/ui/ugfx.c`'s `s.pixels`), which is less than it sounds like:
the hard part is that ALLOCATION, RESIZE and TEARDOWN have to move
together, and every window on screen goes through them.

- **5a -- the client OWNS the memory, the kernel still maps it.** The
  client creates and mmaps the object and passes the name; the kernel
  adopts it and drives `comp_map()` exactly as now, keyed on the shm
  index instead of frames it allocated. The kernel stops owning window
  memory, and every mapping path is untouched.

  **DONE 2026-09-08.** The client creates its buffers as named shm
  objects it owns; `create_window()` adopts them by name and checks the
  object's CREATOR is the requesting client, which is the access control
  that makes a guessable name safe. The kernel allocates nothing and
  maps nothing into a client.

  Three things the build established that the design had not:

  - **`WIN_REQ_BUFFER` was needed.** The kernel grew the stale half at
    PRESENT time, opportunistically; it cannot, because only the client
    can replace an object it created. The client says it replaced one.
  - **`WIN_REQ_RESIZE` carries WHICH buffer was prepared.** Both sides
    deriving it from `front` disagree the moment a present lands between
    the client's replace and the request.
  - **`win_server_create_raw()`/`resize_raw()` stand in for a client**,
    including its mmap, so the KTEST fixture needs no second creation
    path -- which was the prerequisite this plan named.

  **THE BUG THAT COST THE MOST, recorded because it was in none of the
  four places it was looked for:** `SYS_MUNMAP` requires a PAGE-ALIGNED
  length and a window is `w * h * 4`, which almost never is. Every unmap
  was silently refused, so each resize leaked a buffer's frames and the
  run's biggest allocation -- maximize -- was the one that failed. Found
  by `lswin` showing the shm indices climb where they should be reused.

- **5b -- the kernel stops mapping. DONE 2026-09-08.** The compositor
  opens the name itself. `comp_map`, `comp_unmap`, `comp_poison`,
  `comp_span`, `comp_poisoned`, `comp_ref`, `comp_retired`, the poison
  page, `win_compositor_vaddr()`, `WIN_REQ_MAP_WINDOW` and
  `WIN_REQ_UNMAP_WINDOW` are gone -- and so is `adopt_buf()`, which the
  build found had nothing left to protect. `struct win_buf` is
  `{ w, h, gen }`; the kernel holds no shm object, no reference and no
  mapping for a window, and `win_server.c` is ~1900 lines down to ~1500.

  **THE CLIENT GRANTS, which is the decision that shaped it.** A
  kernel-side grant at create time is safer TODAY -- it cannot be
  missed, and no beacon is needed -- but stage 6 deletes the hook it
  would hang on, because the kernel then never sees a window being
  created. So the grant is `sys_shm_grant()` in the client, with the pid
  from the `toywm` beacon it already opens, and Toykit REFUSES to create
  a window when that beacon is absent: without it the window would open,
  draw, present and never appear. Loud beats invisible, and it is what
  stage 6 does anyway -- no compositor, no window.

  What was checked before choosing it, and changed the answer: the
  argument FOR a kernel grant was that it survives a compositor
  handover. It buys nothing, because TWP has no window-enumeration
  request -- a compositor only ever learns about windows through
  `WIN_EV_CLIENT_CREATED`, so a handover with live windows was already
  unsupported.

  **A GRANT LIVES ON THE OBJECT, NOT ON THE NAME.** A resize unlinks and
  re-creates under the same name, and the new object's grant list is
  empty -- so the grant belongs beside every create in `buf_make()`, not
  once at startup. Missed, the first resize would be the last frame the
  compositor ever saw of that window.

  **THE GENERATION RIDES THE PRESENT, packed beside the front index in
  `WIN_EV_CLIENT_PRESENT`'s `b`.** It belongs to that message rather
  than to a carriage: today the present is a kernel event and after
  stage 6 it is a channel message, and the field travels with it either
  way. An invalidation EVENT of its own was rejected for a specific
  reason -- the event queue is 32 deep and sheds the oldest under a
  present flood, so a lost one is a compositor blitting a freed object
  forever, where a stale generation costs one frame.

  **THE SIZE CHECK MOVED TO THE READER, AND MAPPING IT IS THE CHECK.**
  The kernel used to refuse a window whose object was too small for the
  size claimed; it cannot now, holding no object. The compositor asks
  `mmap` for exactly the extent the present claims, and `SYS_MMAP`
  already refuses a length past an shm object's pages -- so the check
  sits in the one place that knows both numbers, and a lying client
  costs its own window a frame.

  **THE SIZE IS NOT A PROXY FOR THE OBJECT**, and the first version of
  this bumped the generation only when the dimensions changed. A client
  re-creating a buffer at the size it already had makes a NEW object
  under the same name, so the compositor would have gone on mapping
  memory nobody draws into -- a window frozen on its last frame. It is
  reachable: a drag proposing the size a window already has, or a
  restored geometry that matches, both send the request. Found in
  review rather than by a test, which is why there is now a test.

  Two things the build established:

  - **The poison page retired for free, along with `comp_retired`.**
    Nothing can revoke a mapping the compositor made itself, so the
    shrink hazard that `comp_span` existed for cannot arise, and a
    destroyed window's pixels stay readable by reference rather than by
    the window table holding its slot.
  - **A FAILING KTEST ASSERT SKIPPED EVERY LATER TEST IN THE FILE.** An
    assert returns from the test, so the teardown that gives the
    compositor role back never ran -- and one red test became one red
    and eight skipped. Found by running the file's own positive
    control, which is what a positive control is for. `FIX_ASSERT`
    tears down first.

#### Resize: this protocol ALREADY HAS buffer identity, implicitly

Designed 2026-09-08, before 5a starts, because it decides the shape.

The question looked like "does the protocol grow `wl_buffer`?" and the
answer is that it already has one and does not name it. `struct win_buf`
carries its OWN `w`/`h` -- "the dimensions belong to the BUFFER rather
than to the window" -- and a resize rebuilds only the BACK buffer,
leaving the front holding the last finished frame at its old size until
a present swaps them. Two slots, each with an identity and a size, one
of them named as current. That is `wl_buffer` with the object left
anonymous.

So client-owned memory does not introduce the problem; it makes the
existing thing explicit:

- **A buffer IS an object**, `win.<pid>.<slot>.<buf>`. The name is
  STABLE -- it identifies the slot, not the memory in it.
- **A GENERATION identifies the memory.** Replacing a buffer is
  `shm_unlink` plus a fresh create under the same name, and the
  generation for that slot goes up.
- **A present carries the front index, its size AND its generation.**
  A compositor holding a different generation re-opens the name and gets
  the new object; the one it was reading stays alive under its own
  mapping until it lets go. (Since 2026-10-04 it also carries what
  CHANGED -- `struct win_damage` in the `text` field, a zeroed one
  meaning the whole surface; `docs/decisions/gui.md`, "A window is drawn
  only where it can be seen".)

**That last property is not a lucky accident -- it is what `shm_unlink`
already promises**: "an object somebody is still using survives its own
unlink", with the frames going at the last holder. It is exactly
`wl_buffer.release`, reached from the other direction, and it is the
same mechanism stage 2 used to stop a destroyed window flashing black.

The alternative considered and rejected: allocate with headroom so an
ordinary resize keeps one object and only a growth past capacity swaps.
It is less code and it does not remove the swap -- it makes it rare,
which is worse, because a path taken once in a hundred resizes is a path
that breaks unnoticed. This repo's own rule about producible fallbacks
says the same thing from the other side.

### Stage 6 -- delete the kernel's window table -- DONE 2026-09-09

What is left in ring 0 is what must be: the framebuffer grant
(`win_surface.c`, a device mapping) and raw input delivery to whoever
holds the compositor role (`win_input.c`, evdev's job). `win_server.c`'s
window table, `win_server_ops` and the dead ring-0 presentation layer go.
(Stage 8, below, found one more thing that was not "what must be": the
per-client event queue, and took it too.)

#### What the table still backs, measured after 5b

`win_server.c` is 1504 lines and serves eleven requests. Five things
keep the table alive, and they do not move as one piece:

1. **Slot allocation.** `WIN_REQ_CREATE` accepts or refuses the slot a
   client proposes, and the client's buffer objects are named after it.
2. **A buffer's size, front index and generation**, which is what a
   present carries.
3. **Teardown.** `win_server_client_gone()` closes a dead client's
   windows.
4. **`app_identity`**, interned from the owning process's spawn path,
   taken from the SCHEDULER and never from anything the client said.
   The compositor groups taskbar buttons by it.
5. **`WIN_REQ_ACTIVATE`**, which the kernel answers ITSELF by scanning
   the table for a window whose identity matches the ASKING process's.
   This is the whole of single instance, and it is load-bearing in both
   directions: a wrong "no" opens a duplicate window, a wrong "yes"
   makes an app exit without ever drawing.

The first three move with the requests that carry them. **4 and 5 are
the decisions**, and they are open.

#### The three questions, ANSWERED 2026-09-09

- **How does the compositor learn what PROGRAM a client is?** A
  `QUERY_` provider, `QUERY_PROCPATH` -- one record per live process,
  pid and spawn path. The kernel stays the source of truth (it is the
  only party that knows) and holds no window state to do it. The
  alternatives declined: keeping `WIN_REQ_ACTIVATE` in ring 0, which
  leaves per-window state and does not finish the stage; and deriving
  identity from the `.desktop` entry the launcher used, which covers
  only apps started FROM the launcher and fails silently for anything
  else.
- **How does the compositor learn a client died?** `uchan`'s server
  scan, which already drops a ring whose name no longer resolves --
  needing only that it report WHICH client went instead of silently
  reclaiming the slot. Declined: a kernel event (prompt, but per-client
  state in the file this stage exists to empty, and sheddable under
  load) and polling `QUERY_PROCESSES` (no new mechanism, but a scan per
  frame duplicating what the channel carries). **BUILT WITH 6b** --
  it REPLACED `WIN_EV_CLIENT_DESTROYED` rather than running beside it,
  because a second path that closes a window is two chances to close it
  twice. (That event, and the whole `WIN_EV_CLIENT_*` family with it,
  is a retired opcode number in `abi/win_proto.h` now.)
- **How far in one go?** Two halves, 6a then 6b.

#### 6a -- DONE (2026-09-09). Identity, and the boundary that moved

**THE SPLIT THIS FILE ORIGINALLY PROPOSED DOES NOT HOLD, and the reason
is worth keeping.** 6a was written as "move CREATE / DESTROY / RESIZE /
BUFFER to the channel, leave PRESENT on the kernel event path". It
cannot be done in that order: `WIN_REQ_PRESENT` looks its window up in
the table, flips `front`, and reads `bufs[front]`'s size and generation.
Everything CREATE, RESIZE and BUFFER maintain is exactly what PRESENT
reads -- so those four cannot leave while PRESENT stays without the
kernel being handed the same numbers twice.

So the halves are drawn where the state actually divides:

- **6a takes out what PRESENT does not need.** `app_identity`, the
  interned path table, and `WIN_REQ_ACTIVATE` are gone from
  `win_server.c`. What is left in `struct client_window` is the slot,
  the front index, the two buffers' sizes and generations, and the
  client's declared `app_id` -- which rides `WIN_REQ_CREATE` and leaves
  with it in 6b.
- **6b takes the rest**, because it can only be done as one piece: the
  present carries its own w/h/gen, CREATE / DESTROY / RESIZE / BUFFER
  move to the channel, the death signal moves to the channel scan, and
  the table goes.

What 6a built:

- **`QUERY_PROCPATH`** (`kernel/proc/procpath_query.c`) -- pid, spawn
  path, one record per live process.
- **The compositor derives identity itself.** `wm_client.c` interns the
  owning process's path at window create, into the same shape the kernel
  used to keep -- a small never-reclaimed table, because the readers
  COMPARE identities rather than print them.
- **`WIN_REQ_ACTIVATE` is a channel message with a REPLY**, the only
  one. `uchan_call()` already existed for exactly this. The client sends
  no name, as before; the compositor asks who the ASKING pid is and
  matches against its own list, so the guarantee is unchanged and the
  string an app declares about itself is still not part of it.
- **No channel and no answer both mean "no twin".** The asymmetry is the
  design: a false "yes" makes a single-instance app exit without ever
  drawing, a false "no" opens a window the user can see and close.

**AND THE SIZE OF THE ROUND TRIP IS THE POINT.** This adds a scheduler
hop where there used to be a syscall -- but it happens ONCE, before a
single-instance app opens anything, and it is what removes an identity
per window from ring 0. The traffic that must not round-trip (a present,
every frame, per client) is untouched.

#### 6b -- DONE (2026-09-09). The table is gone

`win_server.c` is 916 lines, down from 1504. **It contains no window.**

What moved, and the one thing that made it possible:

- **A PRESENT CARRIES ITS OWN GEOMETRY.** `WIN_REQ_PRESENT` names the
  buffer, its generation and its dimensions. That is the change
  everything else follows from: the kernel kept a per-buffer size only
  so a present could answer from it, and `WIN_REQ_RESIZE` and
  `WIN_REQ_BUFFER` existed only to keep that copy in step. Both are
  RETIRED -- not moved. A record kept true by remembering to send a
  message is a record that goes stale, and this one did, as a window
  sheared one pixel per row (fixed in 8c07f3a2, designed out here).
- **The client owns its front index and its generations.** It knows
  which buffer it drew and which object it replaced; the kernel was
  answering both from a copy.
- **CREATE, DESTROY, TIMER, PONG and CLOSE_PID join TITLE, HINTS,
  CURSOR and ACTIVATE on the channel.** Create replies with the granted
  slot; everything else is fire-and-forget.
- **A dead client is found by the channel scan.** `uchan_server_scan()`
  reports which pids departed rather than reclaiming their slots
  silently -- the signal was already in the transport the compositor
  polls every frame, since an shm object's name goes when its creator
  dies.

**WHAT THE TABLE WAS DOING THAT IS NOT ABOUT WINDOWS.** Two callers
walked it to answer "who are the GUI processes": the font broadcast and
the ask-everyone-to-close when the compositor dies. Neither cares which
window anything has. `win_events_is_client()` answers it now -- a
process that has waited for a window event -- which is transport state
the kernel already owns, with no window behind it.

**WHAT WAS LOST, stated rather than glossed.** Six KTESTs went with the
table (a window at the claimed size, slot proposal and refusal, the
buffer flip, a present's generation, a resize touching only the back
buffer, a destroyed window freeing its slot). Their subject is in ring 3
now and is covered from outside by `winclient_test.py`,
`uapp_test.py`, `resize_stride_test.py`, `single_instance_test.py` and
`compositor_death_test.py` -- seconds each in a booted desktop, against
microseconds in the kernel. `QUERY_WINDOWS` and `/bin/lswin` went too:
they existed to show the kernel's view BESIDE the compositor's, and
there is no second view to disagree.

**Access control did not weaken, it moved.** `lookup(pid, id)` answered
"does this process own this window?"; the channel answers it by
construction, because `uchan_server_recv()` reports the ring's owner and
a client cannot write another's ring.

#### What does NOT move, and why it is not a compromise

**The event queue stays**, and so does `WIN_REQ_EVENT_PUSH`. Input is
the kernel's by design (see Out of scope below), and a client's
keystrokes and pointer events reach it through that queue -- so the
queue is a transport for something ring 0 legitimately owns, not a
remnant of the window system. The same is true of the compositor ROLE:
`win_input.c` and `win_surface.c` both key off it, and neither is
window state.

#### 6c -- DONE (2026-09-09). The presentation layer and the carriage

**STAGE 6 SAID `win_server_ops` WENT WITH THE TABLE, AND IT DID NOT.**
6a and 6b took the window state and left the registry that used to serve
it, so this file claimed a removal the code had not made -- found by
reading the two against each other rather than by any check, since
nothing fails when a doc is wrong.

What went, and why each was dead rather than merely unused:

- **`struct win_server_ops`, `win_server_register()`,
  `win_server_ops_current()` and `win_server_active()`.** The struct was
  down to one slot, `debug_command`, and its header argued that the slot
  survived because the `gui` channel is still the kernel's. The channel
  is; the CALLBACK was the ring-0 WM's way of answering it, and a ring-3
  compositor answers through `debug_via_compositor()` instead. Nothing
  outside a KTEST had registered ops since `apps/wm/` was deleted.
- **`win_server_active()`'s three live branches**, each always false: the
  early return in `win_input_poll()` that kept a ring-0 WM's keys from
  being eaten, the desktop-versus-second-consumer guard in
  `compositor_gone()` (whose own comment said it could go with the ring-0
  WM), and the `!g_ops` half of `win_server_request()`'s gate.
  `win_server_any()` keeps its name and is now `g_comp_pid != 0`.
- **`win_transport.c` and its header.** A registry whose one
  implementation was a pair of direct calls, kept as the seam a second
  carriage would plug into. That carriage arrived and is `uchan`, in ring
  3, reaching the compositor without passing through ring 0 at all -- so
  the seam was never used for the thing it was built for, and the two
  callers left (`SYS_WIN_REQUEST` and the serial console) call
  `win_server_request()` and `win_server_debug()` directly.
  `WIN_PID_KERNEL` moved to `api/win_server.h` with them.

**WHAT WAS LOST, stated rather than glossed.** Seven `wintransport`
KTESTs went. Six drove the debug chunker -- a short reply, a long one
reassembled byte for byte, one that exactly fills a chunk, an
unrecognised subcommand against an empty one, a second caller mid-drain,
and the same caller twice -- by registering a stub through the dead
ring-0 path, which is the only way a KTEST could reach it. The seventh
asserted that a transport with a missing slot is refused, which is a
property of a registry that no longer exists. The chunker itself is
unchanged and is exercised by every `gui` command the ring-3 tools send,
so it is not uncovered; what is gone is coverage AT the chunk
boundaries, and rewiring those six onto the compositor path (a test that
claims the role and plays compositor with `DEBUG_TAKE`/`DEBUG_REPLY`)
was considered and declined as more than this change was worth.

### Stage 7 -- popup surfaces -- DONE 2026-09-09

Not in the original staging: it became possible the moment stage 6b
left the compositor as the only party that knows what a window is.
`WIN_REQ_POPUP` is the third round trip -- a second surface of the same
client, anchored to a rect of its parent, placed by the compositor
against the work area, dismissed with `WIN_EV_POPUP_DONE` by a press
outside every surface of the client. The design calls (why the
compositor places it, why it is a row in `windows[]` and not an overlay,
the three-way delivery rule, and the two toolkit rules the first run
taught) are written in full in `docs/decisions.md`, "A popup is a
surface of its client". What it needed from the kernel: nothing. The
event queue carries `window` already, and the shm name carried the slot.

### Stage 8 -- a client's events on its own ring -- DONE 2026-09-09

Not in the original staging either. Stage 6 wrote "the event queue
stays, and so does `WIN_REQ_EVENT_PUSH`: input is the kernel's by
design" -- and that covered only half of what the queue carried. The
compositor's inbound half is evdev's shape and stays. The OUTBOUND
half -- the compositor asking the kernel to put a key, a resize or a
close on a client's queue, and the client parking in `SYS_WAIT_EVENT`
for it -- was Windows' shape (a per-thread message queue in win32k),
not Linux's, where a client's events cross a socket the kernel does
not read. Three things in ring 0 existed only for it: a queue per
process (49 KB of static rings, indexed by pid), the "is this process
a windowing client" bit the font broadcast and `compositor_gone()`
walked, and the one request that reached across into another
process's queue.

**What was built.** The channel ring a client already publishes
(`toywm.<pid>`) grew an INBOX: a second single-writer ring in the same
object, `in_head` the compositor's and `in_tail` the client's, plus a
word the client parks on (`lib/uchan_page.h`, `uchan_server_send()`,
`uchan_client_wait()`). Toykit's loop drains that ring and its own
worker posts; `uapp_post()` appends to a private queue and kicks the
word with an atomic add, so a worker thread wakes the loop without the
kernel. The compositor writes every event it used to push. The kernel
keeps ONE queue, the compositor's (`win_input.c`, which absorbed
`win_events.c`), resets it on a role change, and answers -EPERM to any
other process asking the event syscalls.

**The one thing a single writer cannot do, and what replaced it.** The
kernel queue evicted the oldest INPUT when full and kept a notification.
A ring writer cannot touch what its reader has not consumed, so the
compositor cannot evict. Input the inbox cannot take is dropped and
counted; a STATE -- close, resize, focus, font, screen, a popup's
dismissal -- is remembered as a bit on the window and re-sent next
frame with the state as it is then (`wm_client_flush_pending()`). That
is `xdg_surface.configure`: the latest configure is the only one that
matters, and a client that missed three of them missed nothing.

**A dead compositor is noticed rather than announced.** The kernel used
to ask every client to close when the role was dropped, which needed
the client list. A client that waits out `UAPP_WAIT_MS` with nothing
arriving now asks whether the beacon's live pid is still the one its
ring was granted to (`uchan_client_server_alive()`), and closes itself
-- a Wayland client seeing its socket close. `WIN_EV_FONT` and
`WIN_EV_SCREEN` reach the compositor on its queue and it forwards them
to its clients, the way a DRM hotplug uevent reaches the compositor and
not every client.

**Measured.** `tools/ping_rtt.py` reads the compositor's ping round trip
(event path out, request path back) from `gui compositor --json`, which
gained `ping_us_last/max/avg`. Before and after are in the commit that
landed this; the figure to compare is the mean over the same host and
the same apps, and the claim the measurement had to settle was only
that the ring is not WORSE than a kernel wake -- it crosses the same
number of syscalls (one futex wake per delivery in place of one
`EVENT_PUSH`).

What is in ring 0 after it, `kernel/proc/win_*.c` tests excluded:

| file | lines | what it owns |
|---|---|---|
| `win_role.c` | ~450 | the compositor ROLE: claim/release, the FB and cursor-plane gates, the font map, the `gui` channel |
| `win_syscalls.c` | ~170 | `SYS_WIN_REQUEST` and the compositor's three event syscalls |
| `win_surface.c` | 195 | the framebuffer grant |
| `win_input.c` | ~200 | the devices, and the one queue between them and the compositor |

Each is a Linux subsystem in shape -- DRM master, KMS, evdev -- and none
knows what a window or a client is.

## Out of scope

- **Moving input.** A compositor reading raw devices itself is what
  Linux does *not* do either -- evdev is in the kernel and Wayland
  compositors read it through `/dev/input`. The kernel keeps this.
- **The network stack, the filesystem and USB.** Considered and
  declined, 2026-09-08: Linux and Windows keep all three in the kernel,
  and xHCI DMA from ring 3 without an IOMMU is a downgrade.

## Revision history

- 2026-09-09 (latest): stage 8 -- a client's events on its own ring, the
  kernel queue the compositor's alone, `win_server.c` renamed to what
  it is (`win_role.c`) and `win_events.c` folded into `win_input.c`.
- 2026-09-09 (later still): stage 6c -- the ring-0 presentation layer
  and the transport registry deleted. Stage 6 had claimed the first of
  those already.
- 2026-09-09 (later): stage 7, popup surfaces -- the first thing built
  ON the emptied kernel rather than to empty it.
- 2026-09-09: stages 6a and 6b built -- the kernel's window table is
  gone. The 6a/6b boundary moved, and why is under 6a's own heading.
- 2026-09-08: written. Stages 0 through 5b built the same day. The carriage
  and the ordering were settled with the maintainer before any code
  changed; the argument that decided it was that only two of eight TWP
  requests need a reply, which rules out a synchronous carriage for the
  per-frame path.
