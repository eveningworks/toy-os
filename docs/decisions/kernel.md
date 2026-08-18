# Decisions: Kernel, memory and processes

Scheduling, address spaces, syscalls, the process model, and the traps that live under them.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

Write the reasoning HERE, in full: an entry that cannot be understood
without opening something else is not finished.

---

## IRQ registration: one handler per line, framework-automatic EOI

`kernel/arch/x86_64/irq.c`'s table (`irq_register_handler()`/`irq_dispatch()`)
deliberately doesn't support multiple handlers chained on one IRQ line
-- every IRQ source this kernel has, or is about to add (a NIC), lives
on its own dedicated line in QEMU's default topology (confirmed by
build 390's `lspci`), so real IRQ-line sharing (which does happen on
busier real hardware) isn't a case that comes up here; registering a
second handler for an IRQ that already has one just replaces it.
`irq_dispatch()` also sends the PIC end-of-interrupt itself,
automatically, after calling whatever handler is registered -- not
left to each handler to remember. A forgotten EOI on a real IRQ line
silently stops all further interrupts on that line, a classic and
nasty-to-debug bug; removing the chance of it was judged worth the
small loss of flexibility (a handler can't EOI early, before doing
slower work). This replaced `isr_dispatch()`'s old hardcoded if/else
chain (timer/keyboard/mouse special-cased, everything else silently
EOI'd and ignored) -- including the timer, which now hands off to
`scheduler_tick()` from inside its own registered handler
(`idt.c`'s `timer_irq_handler()`) rather than a dispatch-level special
case, so every hardware IRQ (32-47) goes through one uniform path. See
`irq.h`'s top comment and the commit for build 400 for the full
writeup, including what got regression-tested (timer/scheduler,
keyboard, mouse) since this touched all three.

## Blocking I/O waits: hlt when safe, poll when inside a syscall

Any driver that wants to genuinely block (via `hlt`) until an IRQ
fires -- rather than busy-poll a status register -- has to know
whether it's currently running inside an interrupt handler, because
`int 0x80` is wired as an interrupt gate and clears IF for the whole
syscall, so `hlt` there would park forever with nothing able to wake
it, and naively `sti`-then-blocking would reintroduce a real, already-
documented reentrancy bug: `isr_dispatch()`'s epilogue unconditionally
overwrites a single global resume pointer (`g_next_kernel_rsp`) before
every `iretq`, so a nested interrupt firing mid-syscall corrupts the
outer handler's resume point (this is why `SYS_READ_KEY` abandoned
blocking-with-interrupts-on previously). `idt.h`'s
`isr_in_progress()`/`isr_reset_depth()` (a `g_isr_depth` counter,
incremented/decremented around every `isr_dispatch()` call, force-reset
to 0 at the one safe point -- `process_run_ring3()`'s longjmp-style
resume branch) answers "am I inside an interrupt right now?" so a
driver can genuinely `hlt`-block when it's safe (the common case: boot
init and `apps/` code calling `fs_write()`/`fs_read()` directly from
kernel space) and fall back to bounded polling of the *device's own*
status bit when it isn't (the ring-3 `*_test.c` syscall path) -- the
hardware still raises that bit regardless of the CPU's IF state, so
polling it is still real completion detection, just not CPU-interrupt-
driven. `ata.c`'s `wait_dma_irq()` is the first (and, as of this
writing, only) caller, but the mechanism itself is general-purpose --
any future driver wanting to block inside a syscall-reachable code
path (a NIC's TX/RX ring, say) needs this same check, not a
driver-specific reinvention. See `idt.h`'s doc comments and
the commit for build 470 for the full writeup.

## Contiguous memory: linear bitmap scan, not a buddy allocator

`pmm_alloc_contiguous()` (`kernel/mm/pmm.c`) finds a run of N
physically contiguous free frames by linearly scanning the same
one-bit-per-frame bitmap `pmm_alloc_frame()` already uses, rather than
reserving a dedicated always-contiguous region at boot, or replacing
the bitmap with a fundamentally different structure (a buddy/
segregated-free-list allocator, the standard answer to "finding
contiguous runs gets slow/fragmented"). A buddy allocator would win if
this got called often against a heavily fragmented pool -- it finds
and frees power-of-two-sized runs without a linear scan -- but nothing
in this kernel calls `pmm_alloc_contiguous()` on a hot path: today it
only exists for a future NIC driver to set up its descriptor ring once
at init, and no allocator changed size class, hot/cold split, or
scan strategy in a way this could regress. Replacing the whole
allocator to solve a fragmentation problem no code in this kernel has
actually hit yet was judged premature; it's flagged in
`docs/roadmap.md` as the fix if that ever changes, not built now. See
`pmm.h`'s top comment and the commit for build 410 for the
full writeup, including `pmm_selftest()`'s boot-time verification
(no consumer exists yet to exercise these functions any other way).

## `ring3test` still requires a reboot after its fault, on purpose

Once process exit/teardown existed (the commit for build 173) so a
crashed *scheduled* ring-3 process doesn't halt the kernel, `ring3test`
kept requiring a reboot anyway -- not because teardown didn't reach it,
but because it intentionally drops to ring 3 via its own raw `iretq`
instead of `process_run_ring3()`, so there's nowhere for the kernel to
recover it *to*. See `process.h` and `docs/roadmap.md`.

`elftest` used to be this file's other example (same raw-`iretq`
mechanism, via `hello.elf`) until the ELF64-to-`/bin` migration folded
it into the generic `run hello` path (see this file's entry on that
migration, and the git history), at the cost of losing
test coverage for the raw `iretq` entry path specifically. `ring3test`
is the one remaining place that path gets exercised. `hello.elf` no
longer faults at all -- it's a plain greet-and-exit program now; see
[hello.c stopped faulting on purpose and started faulting by
accident](#helloc-stopped-faulting-on-purpose-and-started-faulting-by-accident)
for what happened in between.

## `hello.c` stopped faulting on purpose and started faulting by accident

`run hello` page-faulted (`CR2=0x8000100000`, `RIP=0x800000000a`) for
some time before anyone chased it, and it read like a broken or stale
binary. It wasn't: `hello.c` predated syscalls, so with no way to
print, it proved it had run by writing a marker to a fixed address the
kernel would read back (`USERLAND_MARKER_ADDR`) and then executing
`hlt` to fault deliberately. The `elftest` command mapped a page at
that address specially. When the ELF64-to-`/bin` migration folded
`elftest` into the generic `run hello` path, the harness went away and
the assumption didn't -- so the binary faulted on the marker write, one
instruction *before* the `hlt` it existed to demonstrate. A deliberate
fault had quietly become an accidental one, at a different address, for
a different reason, while still looking like the expected outcome.

Fixed by making `hello.c` a real program (greet via `SYS_WRITE`, exit
0) rather than restoring the harness: `ring3test` still covers the
raw-`iretq` entry path, `crash_test`/`nx_test` still cover deliberate
faults and their recovery, and the binary named `hello` now does what
its name says. `USERLAND_MARKER_ADDR` was deleted along with it -- it
had no other user, and it aliased `ELF_RUN_HEAP_VADDR` (`elf_run.c`)
exactly, two constants picked independently at the same address, so a
future read-back test wanting the mechanism back needs its own address
clear of the heap and stack rather than that one.

Worth generalising: **a test binary whose harness is removed doesn't
report that it lost its harness -- it reports whatever failure the
missing harness causes.** The migration's own writeup listed exactly
what coverage was being traded away and still missed this, because the
lost piece wasn't the test, it was a page mapping the test depended on.
see the git history.

## Kernel heap: coalesces by real address adjacency, not list order

`kmalloc()`/`kfree()` (`kernel/lib/heap_core.c`, with the kernel's half
of the platform hooks in `kernel/mm/heap_os.c`) grow the heap by calling
`pmm_alloc_contiguous()` again whenever the free list can't satisfy a
request, appending the new region's block to the end of the list. That
means list order and physical-address order agree *within* a region,
but nothing guarantees the next `pmm_alloc_contiguous()` call returns
frames adjacent to the previous region -- the PMM's bitmap allocator
is free to hand back frames from anywhere. `kfree()`'s coalescing
(`try_merge_next()`) checks the real pointer arithmetic
(`(uint8_t *)(b + 1) + b->size == (uint8_t *)n`) before merging two
list-adjacent blocks, not just "these are next to each other in the
list" -- merging on list-order alone would corrupt the heap the first
time two separately-grown regions happened to sit list-adjacent but
not address-adjacent (freeing block A would silently absorb block B's
header into A's payload size, and a later allocation into that
"merged" space would write past the actual end of A's real memory).
See the commit that added it for the full design and what
`heap_selftest()` verifies.

## `kfree()`'s coalescing only checked the block being merged in, not the block being merged into

The heap allocator's backward-coalescing call was `if (b->prev)
try_merge_next(b->prev)` -- correct-looking, since `try_merge_next()`
already checks its *argument's* `next` pointer is free before merging.
The bug: it never checked whether `b->prev` *itself* was free. Freeing
a block whose list-previous neighbor was still allocated silently
folded the freed block's size into the still-in-use neighbor's `size`
field (since `try_merge_next(prev)` saw `prev->next` was now free and
merged it in), with no corresponding adjustment to `g_used_bytes`. A
later `kfree()` of that neighbor then subtracted more than was ever
added, underflowing the unsigned `g_used_bytes` counter. This existed
since the heap allocator was first written and went completely
undetected -- nothing ever displayed `heap_used_bytes()` until Task
Manager did, and it showed an impossible ~16 exabyte figure. Fixed
with a `b->prev->free` guard; `heap_selftest()` now explicitly asserts
`heap_used_bytes() == 0` after freeing everything, so this class of
regression is caught at every future boot instead of needing another
UI to happen to surface it. See the commit that added it
(the Task Manager bullet).

## A syscall's path-pointer validation checks a full `FS_PATH_MAX` range, not just up to the string's NUL

`elf_run_from_fs()`'s argv-on-stack layout (see `ls`'s migration to a
real `/bin` binary, the commit that added it) originally
packed argument strings as tightly as possible against the one stack
page's literal top address. That broke `SYS_LISTDIR` the moment an
argv string landed close enough to the page boundary: its handler
(`kernel/proc/syscall.c`) calls `vmm_validate_user_range(pml4, rdi,
FS_PATH_MAX)` on the incoming path pointer -- a fixed 64-byte range
from wherever the pointer starts, regardless of the real string's
length -- so a short string near the page's end still failed
validation because the *range* ran past the mapped page, even though
the string itself (with its NUL) fit fine. Every path-taking syscall
in this kernel follows the same fixed-range-not-string-length
convention (bounding the read once up front rather than trusting a
NUL inside untrusted ring-3 memory), so this isn't `SYS_LISTDIR`-
specific -- any future caller building a buffer a userland path
pointer will point into needs to leave `FS_PATH_MAX` bytes of margin
after it, not just after its longest real string. Fixed by reserving
`FS_PATH_MAX` bytes of never-written padding at the stack page's true
top before laying out any argv strings, guaranteeing every token's
start address has a full `FS_PATH_MAX` mapped bytes after it. Found
live via QMP testing (`ls /` failed with "cannot access '/'"), not by
review.

## The M16 scheduler is permanently armed now -- an empty process table makes that safe

`scheduler_armed` (`kernel/proc/scheduler.c`) used to be false by
default and only ever set true, briefly, inside `scheduler_demo_run()`
(`schedtest`), set back false before returning even on failure -- see
that function's own build-172-era comment for the original reasoning.
Milestone 1 phase 4b (Terminal async spawn, see the commit that added it) needed a scheduler available OUTSIDE that one demo
call, so `scheduler_init()` now sets `scheduler_armed = 1` once, at
boot, and nothing ever unsets it again.

Why this doesn't reopen the M8-M15 safety argument the disarmed default
existed for: `scheduler_tick()` being armed only matters once something
is actually in the process table. `find_next_ready()` scanning an
all-`SCHED_UNUSED` table always returns -1, so every tick that finds
nothing ready just re-confirms `g_next_kernel_rsp` at whatever
`isr_dispatch`'s default already set it to (`regs`, i.e. resume exactly
what was interrupted) -- byte-for-byte the same outcome the old disarmed
early-return produced. So every legacy `process_run_ring3()` caller
(every M8-M15 test command, the physical shell's own `run`/`ls`) is
still completely unaffected, for the same reason as before, just via a
different mechanism (an empty table instead of a flag check). See
`scheduler.c`'s own top comment for the full writeup, and
`scheduler_poll()`'s `SCHED_ZOMBIE` state for the other real change this
item made (a process holds its exit code until explicitly reaped,
instead of being freed back to `SCHED_UNUSED` the instant it exits).

## The kernel context is a rotation participant, not a kernel thread

Making `wm_run()` keep drawing while a ring-3 process runs sounds like
it needs a kernel thread -- a stack, a context, a slot in the process
table. It doesn't, and deliberately didn't get one.

The kernel context already had everything a scheduler entity needs
except a turn. It needs no address space of its own (kernel code is
correct under any process's CR3, since every PML4 shares kernel entry
0), no FP state (kernel and `apps/` are built `-mno-sse`), and no
kernel stack of its own (ring 0 interrupting ring 0 doesn't switch
stacks, so its trapframe lands on whatever stack it was already using).
`scheduler.c` was already saving its trapframe pointer in
`kernel_saved_rsp`. So it takes a POSITION in the round-robin cycle
(`ROT_KERNEL`, `rotation_pos`) rather than a `struct sched_process`,
and `current_index` keeps meaning exactly what it always did -- -1 when
the kernel is running -- because `syscall.c` depends on that through
`scheduler_current_pid()`.

The one deliberate exception is worth knowing before touching this: the
legacy blocking path (`process_run_ring3()`) runs a ring-3 process
WITHOUT a scheduler slot, so from `scheduler.c`'s point of view that
process's trapframe *is* the kernel context. Rotating away from it and
back would resume it under whatever CR3 and RSP0 the scheduler process
left behind. `kernel_slot_runnable()` therefore drops the kernel
position out of the rotation entirely while one is in flight, keyed on
the pre-existing `process_context_is_armed()` rather than a second flag
that could drift out of agreement with it.

See the commit that added it for the full writeup and how
both directions were verified, `scheduler.c`'s `ROT_KERNEL` comment for
the design, and `docs/roadmap.md`'s Milestone 41 for what this unblocks.

## Blocking syscalls deschedule; they never wait in place

The obvious way to write a blocking syscall here -- `sti`, then spin or
`hlt` inside the handler until the awaited thing arrives -- is not
merely slow in this kernel, it is broken, and it was tried before being
ruled out. It worked for exactly one keystroke and then hung.
`g_next_kernel_rsp` (`idt.c`) is a single global "where to resume"
pointer: correct for the scheduler's own use, never meant to be
reentrant. A nested IRQ handler overwrites it while the outer
`int 0x80` handler is still on the stack, so that outer handler's
epilogue resumes into a stale frame. `syscall.c`'s `SYS_READ_KEY`
comment is the original autopsy, and is why that syscall is
non-blocking by hard requirement rather than by preference.

`scheduler_block_current()` (`kernel/proc/scheduler.c`) sidesteps the
problem instead of trying to make the global reentrant. The handler
does not wait -- it RETURNS, through the ordinary `isr_common`
epilogue, into a different entity, which is exactly the switch
`scheduler_tick()` and `scheduler_on_exit()` already perform with
already-proven machinery. Nothing nests, and interrupts stay off for
the whole handler as they always were.

`scheduler_wake()` is deliberately limited so it is safe to call from
an interrupt handler: it only flips scheduler state and writes an
already-saved trapframe, and never touches `g_next_kernel_rsp`. A woken
process becomes eligible and runs at the next ordinary tick. An IRQ
handler that tried to switch directly to the woken process would be
re-creating exactly the reentrancy this design exists to avoid.

Full writeup in the commit that added it.

## `SYS_WAIT_EVENT` makes clients loop instead of restarting the syscall

`SYS_WAIT_EVENT` returns 0 meaning "you were woken, ask again", so every
client wraps it in `while (sys_wait_event(&ev) != 1) { }`. That looks
like a missing feature and isn't.

The wake happens inside an interrupt handler, running under whatever
address space happened to be current -- which is not necessarily the
waiting process's. So the kernel physically cannot copy the event into
that process's buffer at wake time; the copy has to happen back inside
the client's own syscall, which means the client has to re-enter it.
This is the same spurious-wakeup contract a condition variable has, and
the loop does not spin the CPU: each pass that finds nothing parks the
process again, using no timeslices.

The alternative, which Linux uses, is to rewind RIP over the trapping
instruction so the syscall restarts itself (`ERESTARTSYS`). Rejected
here: it buries a hard assumption about the syscall instruction's
length inside the scheduler, and would have to be revisited if the
syscall entry ever moved off `int 0x80`. The explicit loop costs a
client three lines and hides nothing.

## The windowing protocol is one syscall carrying typed messages, not a syscall per operation

Every windowing operation a ring-3 client can perform -- create,
present, destroy, retitle -- goes through the single `SYS_WIN_REQUEST`,
which dispatches on the `type` field of a `struct win_request_msg`
(`kernel/include/abi/win_proto.h`). The obvious alternative, a syscall
each, was rejected deliberately.

The reason is Milestone 41's whole architectural bet. toy-os is
building toward a GUI where apps are ring-3 processes, and the open
question was whether the window manager itself should move to ring 3
too (the Linux answer) or stay in the kernel (the Windows NT answer).
The chosen path is "kernel compositor now, movable later", and what
makes "later" cheap is that clients and the window server only ever
talk in messages -- never by calling into each other. With a message
protocol, moving the server out is a transport swap and the message
handling is untouched. With one syscall per operation, the syscall
signature IS the protocol, and moving the server means rewriting every
call site.

The same reasoning shapes two smaller choices. Neither
`struct win_request_msg` nor `struct win_event` contains a pointer, and
both have fixed layouts, so the identical bytes work whether copied by
a syscall today or read out of a shared-memory ring later. And a
client's buffer address is DERIVED from its window id
(`win_buffer_vaddr()`) rather than returned by the server, so there is
no address to re-negotiate when the transport changes.

The kernel/WM split follows the same line: `kernel/proc/win_server.c`
owns the memory (ids, buffers, mappings, teardown -- things `apps/`
cannot reach, since `kernel/include/kernel` is off its include path),
`userland/wm/wm_client.c` owns presentation (window list, chrome, z-order,
input routing), and they meet at a registered `struct win_server_ops`
-- the same registry pattern as `display.h`'s `display_driver`.

## The TWP transport seam has exactly one implementation, so it is UNVALIDATED

Milestone 41's stage 3 added `struct win_transport`
(`kernel/include/kernel/win_transport.h`) -- the same
one-struct-of-function-pointers registry as `display_driver` and the VFS
backend probe -- and there is exactly ONE implementation behind it, the
`direct` one that calls `win_server_request()`/`win_server_debug()` in
process. So nothing proves the interface is not simply syscall-shaped.

That matters because this repo's standing rule is the opposite: an
unreachable path is a guess, which is why `ata nodma`, `nopat` and TFS3
v1 exist to keep fallbacks producible. An abstraction with one
implementation is the same problem wearing a different hat, and saying
so is cheaper than pretending otherwise.

**What is most likely wrong with it, named in advance so stage 4 checks
these first:**

- **Batching.** `request()` is one message in, one answer out,
  synchronously. A ring transport wants to submit many and collect
  later, and the WM's ops are called inline from inside that call.
- **Who owns the copy.** The reply buffer lives in `win_server.c`
  between the WM and the transport, which suits a carriage that must
  chunk. A transport that could hand over the whole reply at once (a
  shared ring) would want to skip that copy entirely, and the current
  shape gives it no way to say so.

A cheap throwaway second implementation was considered and rejected: it
would demonstrate the seam is not *syscall*-shaped without demonstrating
it fits anything real, which is the only question worth answering. The
real second implementation is stage 4's, and the honest position until
then is that this seam is untested design, not proven design.

Deliberately NOT built in this stage: the shared-memory ring. It is a
performance item, not a prerequisite -- stage 4 needs the WM to talk
over *something*, and the syscall path already does. See
`docs/wm-ring3-design.md`'s "Scope DECIDED" note.

## `gui` output goes to a caller-supplied sink, not through a klog redirect

`userland/wm/wm_debug.c` wrote its answers with 143 `klog_write()` /
`klog_printf()` calls, straight to whatever the serial console was
connected to. Stage 3 needs that output to become a PAYLOAD, since the
console now reaches the WM over the transport and a message carries
bytes rather than side effects on a serial port.

The cheap way was a global capture: `klog_set_capture(buf, cap)` for the
duration of a command, leaving all 143 sites untouched. It was rejected,
and the reason is the interesting part -- a redirect is GLOBAL, so
kernel log lines emitted *during* a command get swallowed too. That is
not hypothetical: `gui spawn` provokes ELF-loader logging, `gui open`
provokes the WM's, and `gui damage verify on` provokes the damage
reports. Every one of those is something a test reads back through
`DebugConsole.logs()` or `damage_bugs()`, so the capture would have
quietly moved them out of the serial log and into a reply nobody parses
for them. A green suite would have stayed green while the diagnostics it
depends on went missing.

With an explicit `struct dbg_out` (`userland/wm/wm_debug.h`) only this
file's own output is captured and the kernel log is untouched. The cost
is a mechanical 143-site diff and a sink parameter threaded through
~14 functions, which is a one-time price for a property that holds by
construction afterwards.

Note the sink deliberately breaks `kernel/lib`'s formatter rule (a value
that does not fit writes NOTHING rather than a truncated one): it
truncates and sets `overflow`. The difference is what the value IS -- a
half-written number is wrong, whereas a transcript that stops early is
merely shorter, and the flag is what keeps that visible instead of
silent.

## The diagnostic channel carries its own payload struct, so presents stay cheap

`WIN_REQ_DEBUG_CMD`'s command and its reply ride `struct win_debug_msg`
rather than the two structs every other message uses. This looks like a
second mechanism and is worth explaining, because the staging note for
stage 3 explicitly rejected a side channel.

It is not one: these are ordinary `WIN_REQ_*`/`WIN_EV_*` types going
through the one transport and the one entry point, so a ring-3 window
server inherits the diagnostic path with nothing to re-plumb. What is
separate is only the PAYLOAD, and it has to be. A reply is text and runs
to kilobytes -- `gui help` measured 1699 bytes and `gui windows --json`
grows with the window count -- while `struct win_event` is a fixed 24
bytes and `struct win_request_msg` carries `text[32]`. Neither can hold
one.

The alternative was widening those, and that is the trap: `WIN_REQ_PRESENT`
is sent once per client frame, and the syscall path copies the whole
message in and back out on every request. Widening the shared struct to
hold a diagnostic reply would put a kilobyte-sized copy on the hot path
to serve a channel used only by test tooling. So the hot path keeps its
56-byte message and the diagnostic pays for its own size.

## A client window's close button is a handshake, not a seizure

Clicking the X on a ring-3 client's window does not close it. The
window manager sends `WIN_EV_CLOSE` and waits for the client to answer
with `WIN_REQ_DESTROY`.

Two reasons. The client may have unsaved state and is the only thing
that knows it. And the WM tearing the window down behind the process's
back would leave that process drawing into a buffer that is no longer
on screen -- with the frames behind it freed and possibly reissued to
something else.

The honest consequence is that a client which ignores the request keeps
its window. Force-closing an unresponsive one needs a "not responding"
timeout and a way to kill a process, neither of which exists yet -- see
`docs/roadmap.md`'s Milestone 41. Every real windowing system has the
same handshake and the same escape hatch; toy-os has the handshake so
far.

## Single-instance is the app's decision, and the launcher always launches

Opening Task Manager twice raises the window that already exists instead
of opening a second one. The mechanism is `WIN_REQ_ACTIVATE` plus
Toykit's `UAPP_SINGLE_INSTANCE`, and the decision it encodes is **which
side gets to say whether a second copy may run**.

The obvious implementation is the wrong one here: have the Start menu
notice that it already launched this entry and focus that window instead
of spawning. It needs no protocol change and it was rejected, because
the desktop does not know enough to make the call. Two Notepads editing
two files are useful; two Task Managers are not. That is a fact about
the program, so `apps/gui_apps.h` has always said a launcher **spawns**
and never focuses -- and a launcher-side rule would also only cover the
desktop, leaving `run taskmgr` in a Terminal and `gui spawn` to open
duplicates that the menu refused.

So the program asks. A client that declares an `app_id` and the
single-instance flag sends `WIN_REQ_ACTIVATE` **before it creates
anything**: TWS raises the matching window and answers 1, and the second
copy exits 0 without ever appearing on screen. A client that declares
nothing behaves exactly as every client did before. This is the pattern
real desktops converge on -- Windows' named mutex plus
`SetForegroundWindow`, GApplication's uniqueness, `QtSingleApplication`
-- and the **raise** is the half that makes it feel correct rather than
broken; an app that merely declines to start looks like a failed launch.

Three smaller calls inside it:

**The id rides `WIN_REQ_CREATE`'s `text` field**, which was unused by
that request, rather than becoming a message of its own. A separate
"register my id" message would leave a window briefly existing without
one -- and that gap is exactly long enough for a second copy to ask "is
anyone there?" and be told no.

**The id is an opaque token, not a path or a title.** `"taskmgr"`, not
`/bin/wm/system/taskmgr` (which breaks the moment a binary moves --
which has already happened once to every windowed binary in this repo)
and not `"Task Manager"` (which collides with the title the user sees
and that Notepad rewrites per file).

**It is not a lock, and the header says so.** Nothing serialises the ask
against the create, so two copies launched in the same instant can both
be told "nobody there" and both open. Claiming the id in the ask would
fix it and needs state to release on a crash; every launch path here is
a human clicking a menu, so the gap is recorded rather than closed. Do
not build mutual exclusion on this.

`tools/single_instance_test.py` covers it, and its own positive control
is worth knowing about: with the raise disabled, the check "the relaunch
brought Task Manager to the front" **stayed green**, because a brand-new
window is frontmost too. It compares the window's `client_pid` against
the original's now. "It is on top" and "it is the same window" are
different claims, and only the second one tests anything.

## Settings are a REGISTRY, not a pile of syscalls -- and the files stay plain text

Milestone 41 stage 4's last prerequisite was written down as "syscalls
for `etc_config_*`", i.e. let ring 3 read and write `/etc`. What shipped
instead is a registry (`kernel/include/api/setting.h`), because the
literal request would have left the actual problem in place.

**The problem is not access, it is DESCRIPTION.** Five settings existed
(`timezone`, `font_size`, `cursor_style`, `keyboard_layout`, plus
`PATH`), each with its own `*_init()`/`*_save()` pair, its own
validation, and its own in-memory copy in a kernel subsystem. Nothing
anywhere held the sentence *"a setting called `font_size` exists, it is
one of these values, and this is how to apply one."* So a Control Panel
had to carry that list itself -- a second source of truth that drifts
the moment a subsystem adds a key. Raw `etc_config` syscalls would have
handed ring 3 a text file and changed none of that.

A subsystem registers a `struct setting` the way it would register a
`display_driver` or a `block_device`: name, label, type, file, a choice
ENUMERATOR, a getter, and one `apply` that validates, applies and
persists. `SYS_SETTING` (`abi/setting_abi.h`) hands ring 3 the whole
list, so the ring-3 Control Panel is GENERATED from it and contains no
list at all -- a setting registered anywhere in the kernel gains a row
with no edit to Control Panel. That is the same move the Start menu made
when it started reading `.desktop` files instead of a C table.

**The files did not change, and that is deliberate.** Settings are still
plain `name=value` text under `/etc`, editable with `edit` and readable
with `cat`. The registry is an INDEX over those files, not a replacement
-- which is precisely the half Unix's `/etc` cannot provide about
itself: every descriptor carries its own `file`, so the system can
finally answer "which file is this setting in?". `config where
<name>` prints it.

Two consequences of keeping the files hand-editable, both paid for
rather than avoided:

- A hand edit and the subsystem's live copy disagree until something
  re-reads. `settings_reload()` re-applies every setting from its file
  and REPORTS how many values the owner refused -- silently ignoring a
  typo in a file someone just edited is how a setting appears not to
  work. `config reload` is the handle; `config diff` shows what an edit
  has not applied yet, using a `stored` field the kernel fills alongside
  the live value so no client needs a second `name=value` parser.
- A `generation` counter rides every reply, so a client learns "someone
  changed something" from a call it was making anyway. Same trick as
  `fs_generation()`; not a callback list, because the interested parties
  are in other address spaces.

`enum setting_result` moved from `api/etc_config.h` to
`abi/setting_abi.h` in the same change: once ring 3 could change a
setting, "applied but NOT saved" became part of the kernel<->userland
contract rather than an internal detail. A client reporting UNSAVED as
success is the exact lie that enum was introduced to stop.

## A config file declares itself with a file, over a built-in floor

`api/config_file.h` indexes the `/etc` DOCUMENTS -- including the ones
holding no registered setting at all (`/etc/timezones`, `/etc/kbs`,
`/etc/desktop.conf`). Those are the hardest to find precisely because
nothing describes them.

It has two sources, and the split is the decision. Kernel subsystems
register theirs in code; anything else registers by dropping a
descriptor (`Name`/`Path`/`Description`) in `/etc/config.d`, which is
the same shape and the same reasoning as the `.desktop` entries that
build the Start menu. That is what lets a RING-3 program declare its
config file with no kernel edit -- which matters because the window
manager becomes a ring-3 program in stage 4, and it owns
`/etc/desktop.conf`.

**Why not files only.** A registry living purely in `/etc/config.d`
cannot bootstrap: a blank disk has no such directory, and a deleted
descriptor would leave `/etc/toyos.conf` nameless -- the system unable
to describe its own primary config file. So the built-ins are a FLOOR,
and a descriptor with the same `Name` OVERRIDES one. That is the
vendor-default/`/etc`-override pattern real systems use, and it means
`/etc/config.d` is authoritative for everything except the ability to
come up at all. Two descriptors colliding, or a built-in colliding with
a built-in, are refused: there the winner would depend on directory or
boot order.

A descriptor is a CLAIM, not a guarantee -- it may name a file nothing
has written yet, and `config files` reports that as "(not created yet)"
rather than hiding the row. Filtering it would leave "where will my
settings go?" unanswerable until after the first save.

## A stale ISO passes every test, so the launchers refuse to boot one

Every headless test here boots `toy-os.iso`, and `make all` does not
rebuild it. So `make all` alone -- or a `make iso` that FAILED on a
compile error -- leaves the whole suite running against the previous
build and reporting a clean PASS. It does not fail loudly; it fails as a
success, which is the worst available direction, and it is what makes a
positive control come back green and send a session auditing the test
instead of the build. This has cost time in many sessions, twice in the
one that finally fixed it.

`tools/iso_guard.py` refuses to launch a stale image, called from
`vm.py` and `qmp_test.py`'s `launch_qemu_cmd()` -- the only two places
anything in this repo starts a guest, so one check covers all 23 GUI
tools plus `ktest_run.py` and `boot_smoke_test.py`.

Two design points, both learned by getting them wrong first:

- **Each source tree is paired with the artifact IT actually feeds**
  (`kernel/` and `apps/` -> `build/kernel.bin`, `userland/` ->
  `build/userland`). The first version compared everything against
  `kernel.bin` and cried wolf on the first userland-only edit, which
  correctly rebuilds `build/userland/` and correctly does not touch
  `kernel.bin`. A guard that false-alarms is a guard people switch off.
- **The userland side is witnessed by `build/.seeded`, a stamp the
  Makefile's `seed` target touches, not by `disk.img`'s mtime.** Seeding
  is content-hash based, so a rebuild producing byte-identical ELFs
  correctly rewrites nothing and leaves the image untouched -- using the
  image would report a no-op rebuild as staleness.

`TOYOS_ALLOW_STALE_ISO=1` bypasses it, for deliberately testing an older
image (bisecting, or building an earlier commit to prove a failure
predates your work -- which is what it was first used for). It prints
that it is bypassing, so a stale export in a shell cannot quietly become
the old behaviour.

## A yield is not a tick: SYS_YIELD reschedules without billing

Task Manager and Shapes both showed **100% CPU at the same time**,
which on a single CPU is impossible -- and that impossibility, visible
in a screenshot, was the whole diagnosis. It was an accounting bug, not
a scheduling one.

`SYS_YIELD` rescheduled by calling `scheduler_tick()`, the timer's own
entry point. The reasoning written at the time was that a yield is
"indistinguishable from the timer happening to fire right now", and for
the *rescheduling* that is exactly right -- the trapframe is laid out
identically and reusing one rotation beats maintaining two. It is wrong
for the *billing*, because `scheduler_tick()` also does
`cpu_ticks++`, and a tick is a unit of ELAPSED TIME while a yield
elapses microseconds.

The magnitude came from a detail worth knowing: a yield returns only
when the process is next scheduled, about a tick later. So a polling
app yields roughly 100 times a second and charged itself 100 ticks a
second against a 100Hz clock -- a clean, stable 100%. Every polling app
did, simultaneously. Anything that BLOCKED (`SYS_WAIT_EVENT`) never
yielded and read an honest 0%, which is what made it look like a
polling problem rather than an accounting one.

So the rotation is now `scheduler_rotate(regs, bill)`, with
`scheduler_tick()` passing 1 and `scheduler_yield()` passing 0. That
makes accounting **sampled**: whoever is current when the timer lands
pays for the whole tick. The known bias is that a process yielding
constantly is undercharged, and it is the honest direction to be wrong
in -- the sum can no longer exceed 100%, where before every polling
process independently claimed all of it. Measured after: a CPU-bound
`spin_test` reads ~50% (the WM's kernel context takes the rest) while
timer-paced clients read 0%, so the column discriminates. The upgrade,
if precision is ever wanted, is a TSC delta per switch; it is in
`docs/roadmap.md` rather than built.

**The test lesson is the reusable part.** The obvious assertion --
billed must not EXCEED elapsed -- does not catch this, and the positive
control is what proved it: the bug produces `billed == elapsed`
exactly, so a `>` comparison stayed green against a kernel that was
actively wrong. The real assertion is that a process doing nothing but
yielding must be billed SUBSTANTIALLY LESS than the whole window
(`userland/tests/cputime_test.c`). Ask what value the bug actually
produces, not merely which direction it errs in.

It also cannot be driven by `usertest_run.py`: `run` is the legacy
`process_run_ring3()` path with no `procs[]` slot, so the test can find
neither itself nor any billing. `kernel/proc/cputime_test.c` spawns it
properly, the same arrangement `pipe_test` already needed.

## Clocksources: timekeeping is an interface, and CPU time is measured not counted

`kernel/clocksource.h` registers sources of monotonic time the way
`display_driver` registers cards: several may exist, the best-rated one
wins, and no caller names the hardware. Two exist -- the 100Hz PIT
(rating 110) and the TSC (300) -- and the scheduler bills CPU time by
asking whichever is live how much time actually passed.

**Why an interface at all, given the repo's second-real-caller bar.**
It clears the bar on arrival: the TSC was already there, calibrated in
`cpuid.c` and used by `gfxbench`, the relocation path and `krandom`,
with three call conventions and no abstraction between them. And the
CPU-accounting bug is the argument in miniature -- billing incremented
a TICK COUNTER rather than asking a clock how much time had passed, so
a yield (microseconds) was charged a full 10ms tick, every polling app
read a fake 100%, and the fix for that produced the opposite error
(anything finishing inside a tick read 0%). Both errors are the same
mistake: a tick count is not a duration. An interface that answers "how
long was that" makes the mistake harder to write.

**The split it encodes** is Linux's: TIMEKEEPING (a counter you read)
is a different job from TIMER EVENTS (deciding when to interrupt, still
a fixed 100Hz here -- Linux's `clock_event_device`). Conflating them is
how one number ends up meaning both "how often we interrupt" and "how
precisely we can measure".

**Wall clock is deliberately NOT a clocksource.** `rtc_read_local()`
answers "what time is it", which jumps when the user sets it and has
one-second resolution. Linux keeps clocksource and RTC apart for the
same reason, and an early draft of this survey had merged them.

**Raw counter plus mult/shift, not a `read_ns()` per source.** Each
source exposes its counter and a fixed-point ratio, and one audited
function does the conversion -- so the overflow reasoning lives in one
place instead of being re-derived per source. `ns = (delta * mult) >>
shift`, with the shift picked as large as the worst-case product allows,
because more shift is more precision and the limit is overflow. The
conversion runs on a DELTA rather than the absolute counter, which is
what makes wraparound a subtraction that works by construction and
makes switching sources mid-boot a matter of not touching the
accumulated total.

**An invariant TSC is required, not merely preferred.** Without
CPUID 8000_0007H EDX bit 8 the counter's RATE changes as the CPU
throttles, so a boot-time calibration silently stops being true and
every duration is wrong by whatever the CPU felt like doing -- a failure
whose only symptom is numbers that do not add up. A machine without it
keeps the PIT, which is coarse and correct, and correct beats precise.

**Reachability, which cost the most time here.** Plain QEMU cannot run
the TSC path at all: TCG does not implement `invtsc` and says so
(`warning: TCG doesn't support requested feature`), and KVM withholds it
even under `-cpu host` because a guest that has seen it cannot be live
migrated. The only way to exercise it is
`python3 tools/vm.py --kvm --cpu host,+invtsc`, which `vm.py` now
supports and documents. The mirror problem is handled the way this repo
always handles it -- `notsc` on the GRUB command line keeps the PIT, so
the coarse path stays reachable on hardware where the TSC would win,
exactly as `nopat` and `ata nodma` do.

**What the accounting is worth now.** Measured under the TSC: a process
doing nothing but yielding for 300ms is billed 54 MICROSECONDS, which is
both plausible and invisible to the previous scheme in either of its
forms. Under the PIT the same run bills 0, which is honest for a clock
with 10ms resolution.

**The trap, found by the test.** Every path that stops running the
current process must bill BEFORE changing `current_index`, and the
first version missed one: when the KERNEL context was running there is
nobody to charge, but the start timestamp still has to move. Leaving it
stale handed the next process everything the kernel had just spent --
measured at 9.51 SECONDS billed across a 300ms window. `bill_current()`
is called unconditionally at the top of the rotation now.

## A client blocks between frames -- WIN_EV_TIMER, not a polling loop

A Toykit app with an `on_tick` used to run its loop flat out: tick,
pump without blocking, `sys_yield()`, repeat. That wakes a process 100
times a second whichever cadence it actually wanted -- Task Manager
counted 40 passes to refresh about twice a second, so 98% of its
wake-ups existed only to decide it had nothing to do.

`WIN_REQ_TIMER` arms a repeating timer on a window and `WIN_EV_TIMER`
delivers it, so the app blocks in `SYS_WAIT_EVENT` in between and
`on_tick` arrives as an ordinary event. An app names `tick_ms` and
nothing else changes: Task Manager asks for 500ms, Shapes for 10ms
(one frame per tick, the cadence its yield loop already happened to
run at, so its rotation speed is unchanged).

Four decisions inside it:

**Milliseconds, not ticks.** The tick rate is the kernel's business,
and a client asking to be woken every 500ms should not have to know it
is 100Hz today. The server rounds to whole ticks and floors at one --
an interval faster than the resolution becomes "every tick" rather than
an error, and crucially rather than zero, which would fire every frame
and turn a request to slow down into the busiest possible loop.

**A deadline, not a queue.** The next firing is computed from NOW, not
by adding the interval to the previous deadline. Those differ only when
a client is slower than its own timer, and the second form silently
accumulates overdue firings that all arrive at once when it catches up
-- the opposite of what a client asking for less frequent wake-ups
wanted. Same reasoning as the event queue dropping the oldest.

**One timer per window.** A client wanting several derives them from
one short interval, exactly as an app does on top of a frame clock. A
general timer service is a bigger feature than anything here needs.

**Polling stays as the fallback.** `tick_ms` of 0, or a server that
declines the request, leaves the old loop in place. That is what keeps
this additive: no existing app changed behaviour by not opting in, and
an older server does not produce an app that simply never ticks.

This also fixed `PIT_HZ` being a bare literal at the `pit_init()` call
and a "100 Hz" remark in two comments -- fine until something had to
convert milliseconds to ticks and would have hardcoded it a fourth
time, where being wrong makes every interval silently the wrong length.

## A hover test parks the REAL cursor, and must un-park it afterwards

`menubar_test.py` failed one check intermittently for three sessions --
roughly one full-suite run in three, always
`Recent files is greyed until a save, then opens a real submenu`, and
always passing on re-run. Diagnosed 2026-08-16. Two halves, and the
second is the part that was not previously written down.

**The cause was the documented `gui move` trap, in the last tool that
had not adopted the fix.** An injected move overrides the mouse for ONE
WM iteration; the submenu opened on that iteration and closed again when
the real pointer took over, so reading the layout afterwards was a race.
`dialog_test.py` and `uidemo_test.py` already used
`DebugConsole.warp_cursor()` for exactly this and said so in their
docstrings; `menubar_test.py` was the holdout.

Two steps made it a diagnosis rather than a guess, and they generalise:

- **Get a rate under both conditions.** Five runs of the tool ALONE
  passed; two of four full parallel runs failed. That alone ruled out a
  widget bug and pointed at timing.
- **Design a probe whose outcomes differ under each hypothesis.** The
  first two theories -- the disk write is slow, the recent-list update
  lags -- were both wrong, and a probe settled it: the saved file was on
  disk in 0.00s, and a SECOND identical hover opened the submenu. Item
  enabled, hover lost. That is the direct evidence the whole diagnosis
  rests on, and it came from instrumenting a real failure rather than
  from reasoning about the code.

**The evidence for the fix, stated as numbers.** Before: two of four
full-suite runs failed. After: **eight of eight passed**, plus three of
three with the tool alone. At the observed pre-fix rate those eight
consecutive passes are about a 0.4% coincidence, which is the point at
which this stops being "it seems better".

**What does NOT work, recorded because it would otherwise be
rediscovered.** Shortening the check's post-save wait to 0.05s looked
like an on-demand reproducer (it failed 1 of 2, then 1 of 4) and is not:
with the fix reverted AND that amplifier in place, four more runs
passed. Those early failures were luck, not a trigger. So the amplifier
is worthless as a control, and the mechanism evidence above -- an
instrumented real failure -- is what the diagnosis actually rests on.

**The new lesson is the un-park.** A parked real cursor is the point of
`warp_cursor()` and a hazard everywhere after it: it STAYS there, so a
menu opened later finds the pointer already inside it and can close or
expand on its own. Applying the fix to both submenu hovers turned a
different check red 5/5 -- and its partner, "a click outside dismisses
the menu", stayed GREEN, because the menu really was closed; it had
never opened. That is the vacuous-pass shape this repo keeps meeting: an
absence check satisfied for the wrong reason. So a tool that parks the
cursor moves it back to neutral ground when the measurement is done
(`unpark()`), and the pair reads as one idiom rather than two unrelated
calls.

## Rubber-band selection is shared source compiled twice, and it owns the behaviour

Drag a rectangle on the desktop and it selects the icons it touches,
highlighting them live as it sweeps and un-highlighting them when pulled
back -- Windows' and KDE's behaviour. Two decisions made it worth its
own entry.

**Where it lives: `kernel/lib/rubberband.c`, compiled TWICE.** The
desktop is kernel-side today and a file manager will be ring-3, so the
obvious options were "build it in `apps/ui/` and port it later" or
"build it in `userland/ui/` and the desktop waits". Both produce two
implementations that drift, which this project has already paid for
three times (the `kpath` copies that disagreed about `../x`, the three
hand-copied scrolling implementations whose third shipped a dead
scrollbar, the ring-3 Notepad's scrollbar grab). So it takes the path
`kernel/lib/geom.c` and `apps/calc_engine.c` already take: one source
file, built once into the kernel and once into `libuapp.a`. The Makefile
rule already existed; the only cost is that the file must stay
freestanding (`<stdint.h>` only, no allocator, no drawing).

**What it owns: the behaviour, not the items.** The caller answers "how
many?" and "where is item i?" through a `struct rb_ops` and does its own
drawing; the module owns the band, the selection set, the modifier
semantics and the click-versus-drag threshold. That is
`docs/gui-guidelines.md`'s "behaviour belongs to the component" rule,
and the payoff is concrete: every rule is a KTEST against a grid of
made-up rectangles, with no compositor, no cursor and no pixels.

Three rules in there that a from-scratch implementation tends to get
wrong, each with its own test:

- **A shrinking band deselects.** The selection is recomputed from the
  selection-at-drag-start on every motion, never accumulated. An
  accumulating version is indistinguishable while the band grows and
  wrong the moment it shrinks.
- **A band dragged up-and-left is an ordinary band.** The rect is
  normalised; a version that forgets selects nothing in that direction
  and passes every other test.
- **A click is not a zero-size band.** Below a small threshold a press is
  a click, which in replace mode clears the selection ("click empty
  space to deselect") and falls out of the same rule rather than being a
  special case. Without the threshold every click is a degenerate drag.

Two integration notes. Modifiers come from `keyboard_mods_now()`, added
for this: a click carries no modifier state of its own, and it is
deliberately a LIVE sample rather than a latched one -- which is exactly
why keyboard input must NOT use it (a key event carries the modifiers
held when the key was pressed, so a modifier released a moment later
cannot retroactively change an already-typed character). And the band
counts as a drag for the entry-reload guard, because its selection is a
set of registry indices that a reload would renumber.

Group DRAGGING -- moving every selected item together -- is deliberately
not built yet. This is the layer it would sit on.

## One desktop-entry directory with a `ShowIn` key, not a second directory per surface

`/usr/wm/desktop/` feeds BOTH the desktop icons and the Start menu. Asked
for a separate `/usr/wm/startmenu/` so an app could appear in one place
and not the other, the answer is a KEY on the existing entry instead:
`ShowIn=desktop startmenu`, defaulting to both.

The reason is duplication. Two directories means any app wanted in both
places has its file copied into both, and the copies drift -- rename the
app or change its `Exec=` and only one surface updates, silently. That is
the same failure this repo has already paid for with the `kpath` copies
that disagreed about `../x` and the three hand-copied scrolling
implementations whose third copy shipped a dead scrollbar.
freedesktop.org hit the identical question and answered it with
`OnlyShowIn`/`NotShowIn` rather than a second directory.

`NoDisplay=1` is kept and still means NEITHER -- a different statement
("this is not a launchable thing") from `ShowIn` ("it is, but only over
there").

**The parser deliberately breaks this project's usual rule.** Everywhere
else here a parser REJECTS rather than guesses; a `ShowIn` naming nothing
recognisable falls back to showing on both surfaces, with a log line. The
usual rule assumes the outcomes are "a value" or "an error", and here
they are not symmetric: hiding an app because its key was misspelled
makes it vanish with no visible cause, and an unreachable app reads as a
broken system (the desktop has already shipped that bug once, when icons
wrapped off the bottom of the screen). Showing it in one place too many,
loudly, is recoverable.

**The implementation trap, which is where the real bug would have been.**
The Start menu's rows are POSITIONAL: it draws row i from a list and
hit-tests by dividing the click's y by the row height. Filtering the draw
while leaving the hit-test on the unfiltered registry lands every click
on the wrong app and looks perfectly correct in a screenshot. So both go
through one accessor pair -- `gui_app_visible_count()` /
`gui_app_visible_at()` -- which makes the disagreement unrepresentable
rather than merely avoided. The desktop keeps registry indexing instead
(its icon positions are persisted by NAME in `/etc/desktop.conf`, so a
reload must not renumber them) and skips hidden entries in place.

## Desktop entries reload live off a filesystem generation counter, not a directory poll

Dropping a `.desktop` file in now updates the desktop and Start menu
without a restart, the way KDE and Explorer watch their desktop folders.
There is no inotify here, so the question was what "watch" means.

The obvious answer -- re-list the directory every few seconds -- was
rejected on cost: it means a real disk read every few seconds forever on
a completely idle machine. That is exactly the class of always-on
background cost this project keeps out.

Instead `fs_generation()` (`api/fs.h`): one counter the VFS bumps on
every successful mutation. The WM compares it each frame, which is an
integer compare and no I/O, and re-reads the directory only when it has
moved. Idle cost is nothing; latency when something does change is one
frame plus a ~500ms debounce.

Three things about it worth keeping:

- **It is global, not per-path, on purpose.** A watcher wakes for changes
  it does not care about and pays one small directory read for the false
  positive. Per-path watches would need a registry, a lifetime and an
  eviction policy to save a read that only happens when something already
  changed.
- **The streamed write path bumps once, at `FS_STEP_DONE`.** A save is
  one change however many slices it took, and this is the path Notepad
  saves through -- without it, editing a file in the editor would be
  invisible to anything watching.
- **The reload DEFERS while anything holds a reference into the entry
  list** -- an open Start menu (positional rows), an icon mid-drag (an
  index), or an open KERNEL-SPACE app window. The last is the one worth
  knowing about: `struct window::app` is a pointer straight into
  `gui_app_registry[]`, and `gui_apps_load()` rewrites and re-sorts that
  array in place, so reloading underneath an open window would silently
  rebind it to whatever entry landed in its slot -- its callbacks would
  belong to a different app. A ring-3 client's window holds no such
  pointer (`wm_client.c` sets it to 0), so it does not defer, and the
  case disappears with the last builtin in stage 4. Deferring costs
  nothing: the generation stays changed, so it fires the moment the
  condition clears, and the test asserts both halves -- deferred while
  open, delivered on close -- because "it did not appear" alone is
  equally satisfied by a reload that stopped working.

## `append` zeroed the block it appended into, and `write`/`append` now write LINES

Two bugs found by trying to create a `.desktop` file from the shell, one
of them serious.

**TFS3 destroyed data on every append.** `do_write_inner()`'s
partial-block path decides whether to read a block before modifying it,
and asked whether the WRITE OFFSET was at or past end-of-file. An append
starts exactly at `node->size` by definition, so that test was true every
single time and the whole block was zeroed -- wiping the bytes already in
it. `write f AAAA` then `append f BBBB` left four NULs followed by BBBB
on disk. The right question is whether the BLOCK begins past EOF, not
where this particular write starts; a partial block is a read-modify-
write, and the read is skippable only when the block is freshly allocated
or lies wholly beyond the file.

The regression tests have to use a fixture SMALLER than a block. An
append that happens to land on a block boundary takes the fresh-block
path and is correct either way, so a test written with block-aligned data
passes against the bug -- this repo's recurring "the data never crossed
the branch" trap, and the reason all three new KTESTs go red against the
old line while a size-only assertion would not (the file was the right
length; it was full of NULs).

**And neither `write` nor `append` terminated its line**, so `write f a`
followed by `append f b` produced `ab`. That made a multi-line file
impossible to author from the shell at all -- which meant every
line-based format this system has (`/etc/toyos.conf`, `.desktop` entries)
could be READ by the shell and never WRITTEN by it. Both commands write
one terminated line now, and refuse rather than truncate a line that does
not fit, matching kfmt's rule that a value which does not fit is written
not at all rather than wrongly.

## Claiming the compositor role is a message, and it is the one request that works with no window server

Milestone 41's stage 1 landed the compositor registration --
`win_server_set_compositor()`, the access-control idiom every mapping
call copies, and revocation of every mapping when the holder changes --
and nothing outside a KTEST could reach any of it. There was no syscall
and no `WIN_REQ_*` that got there. Stage 2's first job was filling that
gap, and the fork was whether to fill it with a message
(`WIN_REQ_SET_COMPOSITOR = 9`) or a syscall (`SYS_*  = 30`).

The message, for the reason the whole protocol exists: TWP's bet is that
an operation is a typed message on one transport, so moving the server
to ring 3 is a transport swap rather than a rewrite of every call site.
A syscall for the one operation a ring-3 window server needs most would
have been the exact shape the protocol was designed to avoid.

The argument against it was real, though, and worth recording because it
looked fatal at first: every other request is refused outright when no
presentation layer is registered, in TWO places -- `syscall.c`'s
`win_server_active()` gate and `win_server_request()`'s own `!g_ops`
guard. A compositor could therefore never register before the WM did.
That is harmless in stages 2 and 3, where the ring-0 WM is always
registered, and fatal in stage 4, where the ring-3 WM *is* the
compositor and there is no kernel-side presentation layer left to
register first. The registration would have become unreachable at
precisely the point of the milestone.

The fix is small and is the reason the objection did not decide
anything: handle `SET_COMPOSITOR` ABOVE the `!g_ops` guard, and make the
syscall's gate typed (copy the request in first, then apply the gate to
every type except this one). Six lines, and the exception is documented
at all three sites because it is the kind of thing a later edit
re-tightens without noticing.

Two rules came with it. Claiming REPLACES the previous holder -- last
claimant wins, the same non-arbitration `display_register()` already
uses -- and revokes every mapping the old one held. But RELEASING is
only the holder's to do: without that check any process could evict the
compositor and take the raw input stream and every buffer mapping down
with it, a denial of service needing no privilege at all. Both are
KTESTs in `kernel/proc/win_server_test.c`'s `winshare` suite, and the
third one there asserts the no-presentation-layer case directly, with
`WIN_REQ_PRESENT` returning -1 in the same breath as its control.

## Raw input to the compositor is level state, tapped inside the WM loop rather than at the driver

Stage 2 delivers the input stream a compositor needs -- the one
`wm_input.c` consumes, before focus and hit-testing. Two things about
how were decided rather than defaulted.

**It is LEVEL STATE, not synthesised edges.** There is no unified input
event anywhere in this kernel to reuse. `mouse_get_state()` returns an
absolute clamped position plus a button bitmask, and today's WM derives
presses and releases by diffing against its own previous sample; the
wheel is a read-and-reset accumulator; only the keyboard is a real
queue. The only event struct in the tree, `struct win_event`, is
*post*-routing and per-client. So the choice was to synthesise edges in
the kernel or hand the compositor the same level state and let it diff.
The second, because it is what the WM already does -- one differ instead
of two, and an event that stays honest about what the hardware actually
reports. `WIN_EV_RAW_MOUSE` carries screen coordinates and the button
bitmask; `WIN_EV_RAW_KEY` and `WIN_EV_RAW_WHEEL` are separate types
because the keyboard and the wheel are separate mechanisms, not fields
of the pointer's state.

**The tap goes inside `wm.c`'s loop, not at the driver.** This is the
finding that would have cost a session otherwise. `gui click` and
`gui key` inject through `wm_debug.c` and are applied AFTER the real
driver read -- the mouse is overridden for one iteration, and an
injected key is used only when the real keyboard returned -1 so a human
is never pre-empted. A tap on `mouse_get_state()` would therefore be
invisible to every synthetic event, i.e. invisible to every GUI test
tool, which are the only proof any of this works. Tapping after the
override is what makes stage 2 testable at all.

**And it is gated on CHANGE.** The WM loop runs on every timer tick and
the event queue is 32 deep dropping the oldest, so pushing level state
unconditionally floods it while the user sits still. The positive
control for this is worth repeating rather than re-deriving: removing
the gate reddens exactly one check in `tools/compositor_test.py` ("idle
produces no mouse events", 12 lines in one idle second against 0). The
neighbouring `dropped == 0` check stayed GREEN under that control,
because a client blocked in `sys_wait_event()` drains 12 events/second
without effort -- so that check catches a compositor falling BEHIND, not
a flood, and should not be read as covering this.

Both paths run at once, which is the stage's whole shape: stage 4
deletes the WM's own routing and keeps this, so the flip is a deletion
rather than a cutover. `compositor_test.py` asserts every injected input
TWICE -- once in the compositor's log and once in UI Demo's -- because
"the compositor received the click" is equally satisfied by an
implementation that stole the stream outright, which would be a
regression wearing a feature's clothes.

## Ring-3 clients draw for themselves, and the font is shared read-only

Two decisions that go together, both in `userland/ui/ugfx.c`.

**No drawing syscalls.** A client renders into its own window buffer
with plain arithmetic -- there is no "draw text" or "fill rect" syscall,
and the drawing path crosses into the kernel exactly zero times. The
client only calls `WIN_REQ_PRESENT` when it has finished a frame. The
alternative would have put every client's rendering back inside the
kernel, which is what Milestone 41 is moving away from; drawing is not
a privileged operation, only the framebuffer is.

**The font is mapped, not copied.** `WIN_REQ_FONT` maps the kernel's
baked glyph tables (`kernel/drivers/font_ttf.c`) read-only into the
client. Those tables are ~11,800 lines and are ordinary kernel
`.rodata`, which this kernel already identity-maps, so sharing them is
just pointing more PTEs at the same frames -- no copy, one instance in
memory however many clients ask.

The size saving is the lesser reason. The real one is DRIFT: link a
copy of the font into each binary and a client keeps rendering at the
old size after the desktop's `font_size` setting changes, so client
text and desktop text quietly disagree. Sharing the kernel's own data
makes them identical by construction.

Read-only is load-bearing rather than tidiness -- these are pages of
the kernel image, and a writable mapping would let any client scribble
on kernel `.rodata`. `vmm_map_user_page_flags(..., writable=0,
executable=0)` is what enforces it.

Two details in the ABI exist because getting them wrong renders
convincing-looking garbage rather than failing: the glyph data does not
start on a page boundary, so the request returns glyph 0's offset
within the mapping; and the tables are coverage maps, not bitmasks, so
`ugfx` alpha-blends per pixel (a `> 128` threshold would render the
same letters visibly jagged).

The boundary this stops at: `ugfx` is a drawing runtime, not a widget
toolkit. The widgets Calculator needs were ported separately into
`userland/ui/uui.c` -- see the next entry.

## Calculator's engine is shared source compiled twice, not copied

`userland/gui/calculator.c` is a port of `apps/calculator.c`, but
`apps/calc_engine.c` is NOT ported. The same file is compiled a second
time with `USERLAND_CFLAGS` (Makefile, `build/userland/shared/`) and
linked into the ring-3 binary. `kernel/lib/string.c` and `knum.c` ride
the same path.

Why a second compile rather than reusing the object: the kernel builds
with `-mcmodel=kernel` and a ring-3 ELF with `-mcmodel=large`, linking
at `VMM_USER_BASE`. The objects are not interchangeable, so rebuilding
is the only way to share the SOURCE — and sharing the source is the
whole point. A bug fixed in the engine fixes both copies of the app,
because there is only one engine. Two hand-synced copies of arithmetic
would have been the worst possible outcome of this migration.

The rule for putting a file on that path: it must be freestanding.
`calc_engine.c` needs only `string.h` and `knum.h`, which need only
`<stddef.h>`/`<stdint.h>`. A file that reaches for kernel state does
not qualify, and the `-Iapps` that lets `calculator.c` see
`calc_engine.h` is scoped to that one object with a target-specific
variable so no other userland program gains the ability to include
`apps/` headers.

What was deliberately NOT shared: the presentation layer.
`ui_button_group` became `uui_button_group` (`userland/ui/uui.c`), because
the kernel version draws through `gfx_*` straight to the framebuffer
and takes its events as WM callbacks — neither of which exists in ring
3. That is a genuine port, and its behaviour (commit-on-release,
luminance-derived hover direction) was carried over deliberately rather
than reinvented; see `uui.h`.

The kernel-space Calculator is still there on purpose. Keeping both is
what made the migration verifiable — the two were compared side by
side, and the shared engine means they cannot disagree about
arithmetic. Retiring the old one is a separate decision
(`docs/roadmap.md`, Milestone 41).

## Ring 3 gets the C names; the kernel keeps `k_`

`userland/lib/string.h` and `userland/lib/stdio.h` declare `strlen`,
`memcpy`, `snprintf` and friends — but there is no second
implementation. Every one of them is the toolkit's `k_*` function, and
`kernel/lib/string.c`/`knum.c`/`kfmt.c` are compiled a second time into
`build/userland/shared/` for `libuapp.a`, the same shared-source rule
[Calculator's engine](#calculators-engine-is-shared-source-compiled-twice-not-copied)
uses.

Why two vocabularies for one set of functions. The kernel's reason for
avoiding the C names is in `api/string.h`: GCC knows what `strlen` means
and recognising a hand-written one can produce surprising code in a
freestanding build. Ring 3 has the opposite need — GCC may EMIT calls to
`memcpy`/`memset`/`memmove`/`memcmp` on its own, for a large struct
assignment or an array initialiser, and those calls need real symbols
under exactly those names. Nothing in the tree provided them, which was
a latent link failure rather than a bug anyone had hit. So those four
are real functions (`userland/lib/cmem.c`) and everything else is a
`static inline` wrapper — that split is the rule, not a per-function
judgment.

Three things that bit while building it, each now recorded where it
bites rather than only here. `userland/lib/string.h` cannot include
`"string.h"`, because a quoted include searches the including file's own
directory first and that resolves to itself; the guard makes it a silent
no-op and every `k_*` is then undeclared. It uses `<string.h>`, which
skips the current directory. The implementation file cannot be called
`string.c`, because `ar` stores members by BASENAME and `libuapp.a`
already contains `shared/string.o` — two same-named members in one
archive, which linked silently only because they happened to define
disjoint symbols. And `USERLAND_CFLAGS` carries
`-fno-tree-loop-distribute-patterns`: without it GCC may rewrite
`k_memcpy`'s own copy loop into a `memcpy` call, making `memcpy()` call
`k_memcpy()` call `memcpy()` forever. That LINKS, and fails at runtime
as a stack overflow with no obvious cause. Before a `memcpy` symbol
existed the same rewrite was a loud undefined reference, which is why
the kernel needs no such flag — the asymmetry is deliberate.

Getting `snprintf` there also forced `kfmt.c` apart:
`vga_printf()`/`klog_printf()` needed `vga.h`/`klog.h` and so
disqualified the whole file from the shared path. They live in
`kernel/lib/kfmt_print.c` now. One header still declares all four; the
split is about what each half may INCLUDE. A new conversion goes in
`kfmt.c`, a new sink in `kfmt_print.c`, and a single kernel include in
the former takes `snprintf` away from userland with no other symptom.

What this deliberately is not: a libc. No `malloc`, no `FILE`, no
`printf`, no `errno`, no TLS — those are Milestone 24 and each has real
design in it. See the commit that added it, including the
honest size cost (`lscpu` +610 bytes of text, `lspci` +1042, because the
shared converters are more general than the hand-rolled loops they
replaced) and why a full libc turned out NOT to be a prerequisite for
moving the display server to ring 3.

## The shell is called `tosh`, and the name covers the language, not a binary

`tosh` = t + OS + h, contracting "toy-os shell" the way `bash` contracts
"Bourne-again shell". It was unnamed until 2026-08-15, which was fine
while there was one command line and no reason to refer to it.

**What the name covers.** The shell LANGUAGE and behaviour -- the
builtins, the command-line editing, the way a line is dispatched --
which today has two front ends: `apps/shell.c` in the kernel and
`userland/lib/tosh.c` in ring 3. `sh` names a language rather than one
binary, and this follows that. It is deliberately NOT the terminal:
`uterm` is the terminal emulator, and a terminal that is not a shell is
a distinction every real system keeps.

**Four names were rejected for collisions**, which is most of the
reasoning worth recording, because each looks obviously right until you
search for it:

- `toysh` -- toybox's actual shell.
- `hush` -- busybox's actual shell.
- `tsh` -- the CS:APP shell lab, assigned to enormous numbers of
  students, so the name is unsearchable.
- `tush` -- reads as crude Finnish slang, which the maintainer
  (Finnish) would have to explain forever.

`wish` (Tcl), `posh` and `ion` (Redox) were ruled out the same way.
`tosh` is British slang for "nonsense", which is the one live objection
and was accepted deliberately: for a hobby OS's shell it reads as
self-aware rather than rude, and no software owns the name.

**There is no `/bin/tosh` yet.** The ring-3 shell is a LIBRARY, because
its first caller is a GUI terminal that owns its own event loop and
cannot sit blocked in a `read()` (see `tosh.h`). A standalone binary
would be a thin `main()` over the same `tosh_init()`/`tosh_run_line()`
calls -- the header has said so since it was written -- but it needs an
interactive stdin story first: a process started by the physical
shell's `run` has nowhere useful to read a line from, which is the same
limitation that makes `echotest` hang under headless testing.

## The process entry ABI is SysV, and crt0 owns the stack alignment

A new process starts with the standard SysV layout on its stack --
`argc` at `(%rsp)`, then `argv[]`, a NULL, then `envp` (empty; there is
no environment yet, and no auxv, because nothing consumes one and
inventing entries nobody reads is how an ABI accumulates fiction).
`userland/rt/crt0.asm` reads it and calls `main()`.

It used to arrive in RDI/RSI instead. That was toy-os's own convention,
fine while every `_start` was a C function taking two parameters, and a
wall for Milestone 40's "run stock musl binaries". The register path was
deleted rather than kept alongside the stack one: two live conventions
for the same thing is exactly how an ABI rots.

**The alignment inverted with that change, and the direction is
counter-intuitive.** The kernel used to hand over `RSP % 16 == 8` on
purpose. That looked wrong and wasn't: GCC compiles a plain C `_start`
like any other function -- assuming a `call` has just pushed a return
address -- and sizes its prologue from there, so a "correctly"
16-aligned RSP put every aligned stack slot off by 8.

With a hand-written entry point, the standard applies. SysV states the
rule at the CALLEE's entry (`%rsp + 8` is a multiple of 16 there), which
means `%rsp` must be **16-aligned immediately before `call main`**. The
tempting `sub rsp, 8` in crt0 -- reasoning "the old convention was 8, so
restore it" -- produces the opposite and is a real bug: `main()` is then
entered 16-aligned, GCC emits `movaps` against stack slots it believes
are aligned, and those FAULT rather than mis-store.

The failure mode is worth remembering because it disguises itself: every
GUI client took a #GP a few instructions into `main()`, while every
plain non-SSE program worked perfectly. Nothing about that symptom
points at stack alignment until you notice which binaries are affected.

See the commit that added it, `userland/rt/crt0.asm`, and
`kernel/proc/elf_run.c`'s `elf_build_argv_on_stack()`.

## A legacy ring-3 process needs its own RSP0 and must not be descheduled

`process_run_ring3()` (the M8-M15 blocking path, still used by the
physical shell's `run`) runs a ring-3 process with NO `procs[]` entry.
From the scheduler's point of view that process simply IS "the kernel
context": its trapframe lands in `kernel_saved_rsp`, and there is
nowhere to record its CR3 or its RSP0.

Two consequences, both of which were live bugs the moment ring-3 GUI
clients could stay alive across a shell command, and both of which
presented as a bare `RING-3 CRASH: Page fault` **in the client** at a
syscall unrelated to the cause:

1. **It must not be switched away from.** `kernel_slot_runnable()`
   refuses to select the kernel position while one is armed -- but
   `find_next_runnable()`'s fallback returned `ROT_KERNEL` anyway when
   nothing else was runnable, which reintroduced exactly the case the
   guard existed to prevent. `scheduler_tick()` now returns early while
   `process_context_is_armed()`, so the rotation never starts.

2. **It needs its own ring-0 stack.** This path never set RSP0, and got
   away with it while a legacy process could never coexist with a
   scheduled one: RSP0 was still the boot stack. But `switch_to()`
   points RSP0 at the running process's kstack and leaves it there, so
   a later `run` took its traps onto a CLIENT's kernel stack and
   overwrote the trapframe that client was suspended on. It now uses a
   dedicated `g_legacy_kstack` (one is enough -- these calls cannot
   nest, which is what `process_context_is_armed()` guarantees).

The general lesson is worth more than either fix: an execution context
the scheduler does not own an entry for cannot be treated as
schedulable, and "the kernel context" was quietly serving as a
dumping ground for two very different things. Both are covered by a
KTEST in `kernel/proc/sched_test.c` that runs a legacy process
alongside a scheduled one; see the git history.

## The retry sentinel is -2 because 0 is a real answer

Every blocking syscall here shares one contract: the kernel cannot hand
over a result at wake time (the wake runs in an interrupt, under an
address space where the caller's buffer may not be mapped), so it wakes
the caller with "ask again" and the real work happens back inside the
caller's own syscall. See `SYS_WAIT_EVENT` in `abi/syscall_abi.h`.

That sentinel is `SYS_RETRY`, defined as **-2**. It started as 0, which
was fine while the only blocking call was `SYS_WAIT_EVENT` -- but 0 is
a LEGITIMATE result for `SYS_READ`: end of file. The moment pipes made
`read` blocking, a reader woken by a write treated the wake as EOF and
reported that the program had finished producing output at the exact
instant it produced some.

The general rule, which is worth more than the specific fix: a sentinel
must be a value the call can never otherwise return. "0 means nothing
happened" is only safe when nothing can legitimately be zero.

## The ring-3 terminal runs its own shell, not the kernel's

`userland/gui/terminal.c` links `userland/lib/tosh.c` -- a shell implemented over
syscalls -- rather than calling the kernel's `shell_dispatch()` through
some new "run this command line" syscall.

The syscall version would have been much less work and is a defensible
thing to build. It was rejected because it moves the WINDOW to ring 3
while leaving the shell in the kernel, which is the part that actually
matters: the kernel shell's builtins (`fsck`, `ktest`, `fsformat`,
`timezone`) reach directly into the filesystem, the test harness and
driver state. Exposing them through one syscall would re-export the
kernel's internals under a new name and call it a migration.

So `tosh` has the builtins a shell can honestly implement over the file
API, and spawns everything else through `SYS_SPAWN` with its output
piped back. The kernel shell is still reachable -- `Esc` leaves the
desktop for the physical one -- which is the honest division: the
ring-3 terminal does what a terminal does, and kernel-only operations
stay in a kernel-only shell.

The cost is real and worth stating: `tosh` is less capable than the
kernel Terminal today. That is a consequence of the boundary being
drawn honestly rather than a defect to paper over.

## The geometry module is shared source compiled twice, like the calculator engine

`build/userland/shared/geom.o` and `fixed.o` are `kernel/lib/geom.c` and
`fixed.c` built a second time with the userland flags. Same reasoning as
[Calculator's engine](#calculators-engine-is-shared-source-compiled-twice-not-copied),
and worth restating because this is now the established pattern rather
than a one-off: the objects genuinely cannot be shared (`-mcmodel=kernel`
vs `-mcmodel=large`), but the SOURCE can, and a second hand-written copy
of a rasteriser would drift from the first. When it drifted, the symptom
would be a ring-3 app drawing a slightly different circle from the
kernel -- a rendering difference with no obvious cause and no failing
test.

## Ring-3 GUI apps live in /bin, not /tests

Calculator, Notepad, Terminal (`uterm`) and Shapes are seeded to
`/bin`. They were in `/tests` for most of the ring-3 migration, purely
because that is where the first client landed and nobody moved them
once they stopped being experiments.

`docs/filesystem-layout.md` draws the line clearly -- `/tests` holds
"test/demo binaries, one kernel mechanism each... not things a user of
the OS wants offered to them" -- and a program offered in the Start
menu is user-facing by definition. The mechanism tests that remain in
`/tests` (`winclient`, `uiclient`, `pipe_test`, `spin_test`, ...) are
still exactly what that directory describes.

Moving them needed the cleanup that doc mandates: `sync` is additive
and never deletes, so the old `/tests` copies had to be removed from
the existing image explicitly (`tools/tfs3_writer.py delete`). Skipping
that leaves stale binaries frozen at their last-synced content forever.

## stderr goes to the kernel log, and is never redirected into a pipe

`SYS_WRITE` treats fd 1 and fd 2 differently: fd 1 honours
`SYS_SPAWN`'s stdout redirection (into a pipe, so a parent can read a
child's output), fd 2 always goes to `klog_putc()` -- the serial console
and `dmesg`.

They used to be identical, which was a bug with two faces. Redirecting
stdout is a request to capture a program's OUTPUT; folding its
diagnostics into the same stream corrupts whatever the parent was
parsing, which is the precise problem Unix has two descriptors to
avoid. And a GUI client has no terminal at all, so its `sys_print()`
went to whatever sink the console happened to have -- which is how this
was found: `tools/gfxdemo_test.py` could not see a single line the demo
logged, because there was nowhere for a windowed ring-3 process to say
anything.

The practical rule for app code: `sys_print()` for output, `sys_eprint()`
for anything diagnostic. The second is readable regardless of who
spawned the process or where its stdout went, which also makes it the
channel a test tool asserts on -- the same path `strace` output takes.

## Stack canaries: `-mstack-protector-guard=global` and a fixed constant, not GCC's defaults

Milestone 2's stack-canary item (the commit that added it)
turns `-fstack-protector-strong` on for both `CFLAGS` and
`USERLAND_CFLAGS` (previously explicit `-fno-stack-protector` in both,
just a "not built yet" placeholder -- see this file's `docs/roadmap.md`
excerpt for the original reasoning). Two things about the flags chosen
are worth knowing if this is ever revisited:

- **`-mstack-protector-guard=global`, not GCC's default `tls`.** The
  default reads the canary via `%fs:0x28` on x86-64 -- this kernel
  never sets up a per-CPU/per-thread FS/GS base at all (no `wrmsr` to
  `IA32_FS_BASE`/`GS_BASE`, no `swapgs`, confirmed by grep across
  `kernel/core/`), so the TLS-based default would dereference an
  unconfigured segment. `global` instead reads a plain
  `extern uintptr_t __stack_chk_guard` (`kernel/lib/stack_protector.c`
  for the kernel, `userland/rt/stack_chk.c` for userland -- two separate
  symbols, two separate address spaces, no reason to share one).
  Building real TLS infrastructure just to use GCC's default guard
  would have been wildly disproportionate to what this milestone item
  actually needed.
- **The guard starts as a fixed compile-time constant and is REPLACED
  with a random one at boot.** It was constant-only at first, because
  this kernel had no entropy source at all; `krandom.h` exists now
  (see the entry below), and `stack_guard_randomize()` installs a
  random guard from it early in `kernel_main()`. The constant still
  protects everything before that point, so it is a fallback rather
  than a placeholder -- and it is KEPT, with a log line, if krandom
  reports no entropy, since a guard "randomized" from nothing is no
  stronger and only looks handled.

  Two details that are load-bearing rather than incidental: the install
  must happen directly in `kernel_main()` (changing the guard while an
  instrumented frame is live panics that frame on return -- the
  defence firing on innocent code), and the guard's low byte is forced
  to ZERO deliberately, as glibc does, so that the guard terminates a
  string-copy overflow instead of surviving one.
- **`__stack_chk_fail`, not a synthesized trap.** Neither the kernel
  nor userland implementation routes through `idt.c`'s existing fault
  dispatcher -- each is just a small, direct function GCC's generated
  epilogue calls on a mismatch. The kernel's prints a panic banner and
  falls into the same unconditional `cli; hlt` loop `idt.c`'s
  non-recoverable path already ends on (there's no "recoverable"
  case for a kernel-side canary trip -- nowhere to hand control back
  to). Userland's is even simpler: `SYS_WRITE` a message then
  `SYS_EXIT` with a distinct code (2) -- from the kernel's point of
  view that's just an ordinary process exit, no different from any
  other `run <name>` finishing, so nothing new was needed to "catch"
  it.
- **Verifying it actually works needs `noinline` on the test's overflow
  function.** `userland/tests/stack_smash_test.c`'s first version called an
  un-annotated `static void smash(void)` from `_start` -- at `-O2` GCC
  inlined it straight into `_start`, which moved the canary check to
  `_start`'s OWN epilogue, after `_start`'s later code (a "survived"
  message + `SYS_EXIT`) had already run and exited the process. The
  test printed "UNEXPECTEDLY SURVIVED" and exited normally with the
  canary silently corrupted underneath -- not because canaries don't
  work, but because the check was unreachable code by the time control
  got there (confirmed by disassembling `build/userland/stack_smash_test.o`
  and finding the compare-and-branch instructions positioned after the
  exit syscall). `__attribute__((noinline))` on `smash()` fixed it --
  a real, non-inlined function has its own `ret` and therefore its own
  canary check firing immediately on return, before `_start` ever
  reaches the "survived" path. Worth remembering for any future
  deliberate-crash test: inlining can silently move a compiler-inserted
  check somewhere your test's control flow never reaches.

## NX landed in userspace first, and the default mapper is the non-executable one

Milestone 2's "NX bit enforcement" and "W^X on kernel + userspace
mappings" roadmap items were done for the *userspace* half first
(`kernel/proc/elf.c`/`vmm.c`, `userland/rt/link.ld`), deliberately not
touching `kernel/arch/x86_64/boot.asm`'s own flat 2MiB-huge-page
identity map, on the reasoning that process page tables get created
fresh per process anyway while the boot map is a boot-critical path.
The kernel half landed a milestone later and is its own entry below --
**the identity map is no longer RWX**, so don't take the ordering here
as a statement about today's state.

The default mapper (`vmm_map_user_page()`) was changed to be
non-executable by default rather than adding a parallel "safe" variant
-- every pre-existing call site (a process's stack, `SYS_SBRK` heap
growth, the GUI framebuffer, a window's pixel buffer) is data, never
code, so this is both the secure default and correct for all of them
with zero call-site changes; only `kernel/proc/elf.c` (needs real
per-segment control) and `kernel/proc/ring3_test.c` (its one
hand-assembled code page) call the explicit-flags variant instead. See
the commit that added it for the full mechanics and the QMP
verification (a purpose-built `userland/tests/nx_test.c` that jumps into a
non-executable data page and confirms the CPU actually faults --
`error_code=0x15` decodes to Present+User+Instruction-Fetch, not a
generic unmapped-page fault).

## Kernel W^X: NX on every huge PDE, one 4KiB split for `.text`, and CR0.WP

`paging_enforce_wx()` (`kernel/arch/x86_64/paging.c`, called from the
top of `kernel_main()`) rewrites the identity map boot.asm hands over.
It is deliberately NOT a general "make the map fine-grained" pass: all
2048 2MiB PDEs keep being huge pages and just get their NX bit set,
and only the slots holding something that must not be writable get
split down to 4KiB. That is one slot -- `.boot`/`.text`/`.rodata`/
`.eh_frame`/`.ktests` all fit inside the first 2MiB page -- so the
whole thing costs one 4KiB table out of `.bss` and needs no allocator,
which is why it can run before `pmm_init()` rather than after.

Two claims the previous entry made turned out not to hold, and both
are worth knowing before someone re-derives them:

- **`pmm.c` needed no changes.** The entry above predicted its
  frame reservation would have to become section-aware. It reserves
  `0.._kernel_end` as one blob and nothing here frees any of it, so
  section-awareness would only matter to a change that wants to hand
  parts of the image back, which this isn't.
- **The 2MiB granularity was never the obstacle.** The obstacle was
  believing the split had to happen in `boot.asm`'s 32-bit
  pre-long-mode code. Doing it in C afterwards is the same result with
  none of that risk, and it can read the linker symbols directly.

**Ring 3 is unaffected because user mappings never enter this map at
all**: `userland/rt/link.ld` links at `0x8000000000`, i.e. PML4 index
1, while the identity map is everything under index 0. Every process's
PML4 shares entry 0 (`vmm_create_address_space()`), so the blanket NX
reaches every address space -- and touches no user page.

**CR0.WP is the half that is easy to omit and impossible to notice.**
With WP clear -- the state the CPU resets into, and what GRUB hands
over -- a supervisor write ignores the read/write bit entirely, so ring
0 can scribble over a `.text` mapping that reads as read-only in every
page table. NX needs no equivalent switch (EFER.NXE covers it), so the
failure mode is half-working protection whose page tables look
completely correct in a dump. The `paging` KTEST asserts the bit
separately for exactly this reason, and its positive control is the
demonstration: clearing that one line turns the CR0 check red and
leaves every page-table check green.

Verified live, not only by reading bits back: a one-byte write to
`__ktext_start` from `kernel_main()` produces `PANIC: Page fault`. That
probe is not committed -- a ring-0 fault ends the boot, so it cannot
live in a suite -- see the commit that added it for how to
reproduce it in two lines.

## The framebuffer is write-combined via PAT, and `nopat` exists to make the MTRR fallback reachable

Reported as "drawing is really slow" on a real machine (an ASUS Zenbook
UX305FA) while being perfectly fast under QEMU. The cause was that
nothing in this kernel had ever set a memory type: `pat` and `mtrr`
existed only as CPUID feature-name strings in `cpu_features.h`. GRUB's
linear framebuffer is therefore whatever the firmware left it as, which
on real hardware is **uncached MMIO** — every store is a bus transaction
the CPU stalls on. `gfx_present()` compounded that by writing pixels a
BYTE at a time (three stores per pixel), so a full 1920x1080 frame was
6.2 million individually-stalled writes.

**QEMU cannot show any of this**, because its framebuffer is ordinary
cached host RAM. That is the important part for a future session: no
test in this repo can observe the bug, a clean `gui_regress` says
nothing about it, and the only instrument is `gfxbench` run on real
hardware.

**PAT is preferred over MTRRs** because it is per-page: it needs no
power-of-two size, no natural alignment and no free range register,
all three of which a variable-range MTRR demands and a framebuffer does
not reliably offer. Slot 4 of `IA32_PAT` is repointed at WC and slots
0–3 are left at their architectural defaults, so every mapping that does
not opt in keeps exactly the meaning it had; a page opts in by setting
the PAT bit and clearing PCD/PWT, which selects slot 4.

An MTRR marking a region UC does not defeat this — SDM Table 11-7 gives
UC(MTRR) + WC(PAT) = WC, which is why Linux write-combines framebuffers
through PAT without touching MTRRs either.

**The trap, and it fails silently in the dangerous direction:** bit 12
is PAT on a 2 MiB page, but on a 4 KiB page bit 12 is part of the
PHYSICAL ADDRESS and PAT is bit 7. Writing the huge-page bit into a 4 KiB
PTE does not fault; it silently repoints the mapping somewhere else. The
framebuffer is far above the kernel image so it is never in a range
`paging_enforce_wx()` split, but the code checks `PAGE_HUGE` rather than
relying on that.

**`nopat` exists because the fallback would otherwise be unreachable.**
PAT has been present since the Pentium III, so every machine this OS can
run on — QEMU's default model included — takes the PAT path, and an MTRR
path nobody can execute is a guess, not a fallback. This is the same
reasoning as `ata nodma` keeping the PIO disk path reachable. With the
flag, both were verified to boot and to report the mechanism they
actually used.

What is NOT proven by anything committed: that write-combining is
*faster*. It cannot be, in this environment. The speed claim can only be
settled by `gfxbench` on the real machine.

## The RAM meter is an uncomposited overlay, which is why it is debug-only

`rammeter` (a GRUB flag, see `docs/boot-flags.md`) draws a live
frame-allocator and heap readout in the top-right corner. It writes
STRAIGHT to the visible framebuffer through `gfx_overlay_*`, bypassing
the back buffer, the clip rect and the dirty-rect box alike.

That combination is normally a bug — it is precisely what leaves stale
pixels behind, and it is the family `gui damage verify on` exists to
catch. It is correct here only because the overlay is never composited:
the WM knows nothing about it, paints over it whenever it repaints that
corner, and the meter reappears on its next tick. Nothing it draws is
interactive, so nothing is lost when a repaint eats it.

**A control the user touches must not be built this way.** It belongs in
the back buffer with its damage declared, or the verifier will correctly
call it a violation.

Two consequences worth knowing. The damage verifier compares BACK BUFFER
contents, so the overlay is invisible to it and reports no violations —
which is a property of where it draws, not an exemption anyone coded.
And it ticks only from `wm_render_frame()`, so it appears on the desktop
and NOT at the physical console, which has no repaint loop to hang it
off.

**The heap row is deliberately not warn-coloured.** `heap_total_bytes()`
is what the allocator has claimed from pmm so far, and it claims more on
demand, so heap-used-against-claimed sits near full as a matter of
course — it read 89% on a freshly booted desktop. Colouring that yellow
would cry wolf every boot and teach the reader to ignore the one row
where the colour means something. Only the physical-frame row has a real
ceiling, so only it gets the green/amber/red bands.

## The console is double-buffered because write-combining made its scroll 357x slower

Write-combining the framebuffer (see the PAT entry above) made every
drawing path in the system faster except one, and made that one
dramatically worse. WC is a **write** optimisation: stores are gathered
into burst transfers instead of going out one at a time. It does nothing
for loads, and it removes the caching that used to hide them -- a read
from a WC page is an uncached bus round trip with no cache fill and no
prefetch.

The framebuffer console was the one surface that read the framebuffer
back. It scrolled by shifting the visible pixels up in place, which is a
whole screen of reads, and its cursor saved the cell underneath itself
before painting over it, which is another read per blink. So the GUI got
faster (double-buffered, writes only) while the CLI got slower, and the
symptom was a scanline you could watch travel down a real display.

Measured with `gfxbench 20` under `make run-kvm`, same build, the only
difference being whether the console had a back buffer:

| | ms per scrolled text line |
|---|---|
| direct (reads the framebuffer) | 178.5 |
| double-buffered | 0.5 |

Full-screen *fill* throughput was identical either way (17.3 GB/s), which
is what confirms the change touches only the read path.

The fix is the invariant, not the number: **the console never reads the
framebuffer.** It draws into gfx.c's back buffer -- which already
existed, is already a static array, and already had dirty-rect tracking
for the WM -- and publishes with `gfx_present()`. Scrolling becomes a RAM
memmove and the cursor's save becomes a RAM read.

Two things worth knowing before editing it. **Drawing and showing are
now separate steps**, so a path that prints and then halts without
reaching a flush point leaves its text in RAM only; the flush points are
`vga_present()` from `keyboard_getchar_mods()`'s idle loop, a throttled
present at the end of each `vga_putc()`, and an explicit call on the
panic path in `idt.c` (which is the one that would otherwise lose the
panic banner itself). And **there is deliberately no "dirty" flag in
vga.c** -- gfx.c already tracks the dirty box and `gfx_present()` no-ops
when it is empty, so a second copy of that fact could only ever disagree
with it, in the silent direction.

Why this was invisible for so long: plain QEMU's TCG ignores guest
memory types entirely, so both paths are equally fast under `make run`
and every test in this repo. `make run-kvm` honours them, which is what
made it reproducible at all -- and is the reason `gfxbench` reports
which mode is live rather than just a number. See `kernel/drivers/vga.c`'s
double-buffering comment and `kernel/drivers/gfx_test.c`'s scroll KTESTs,
which pin the shift but explicitly cannot pin the cost.

## The legacy text console cannot be selected from GRUB, and `gfxpayload=text` does not do it

`kernel/drivers/vga.c` has a complete legacy 80x25 `0xB8000` backend, and
it is dead code on every normal boot: `vga_init()` only reaches it when
`gfx_init()` finds no usable linear framebuffer. The obvious way to make
it reachable -- a second GRUB menu entry with `set gfxpayload=text` --
was tried and **does not work**, and the measurements are worth recording
so nobody spends the time again.

`boot.asm`'s multiboot2 header carries a framebuffer request tag (type 5)
asking for 1280x720x32. GRUB acts on that *before* the kernel runs, so by
the time any kernel command-line word could be read the adapter is
already in a graphics mode and writes to `0xB8000` land nowhere visible.
That rules out a cmdline flag outright.

`gfxpayload` does not rescue it either. Two experiments, both booted and
read back from `dmesg`:

- With the header requesting 1280x720x32, `set gfxpayload=800x600x32` in
  the menu entry changed nothing -- still 1280x720. So for multiboot2 the
  header's request wins and `gfxpayload` is ignored.
- With the header's width/height/depth set to 0/0/0 ("no preference"),
  the resolution *did* change (GRUB chose 1280x800), proving the header
  edit took effect -- and `set gfxpayload=text` **still** produced a
  linear framebuffer. GRUB's multiboot2 loader always sets a graphics
  mode when the kernel carries a framebuffer request tag.

So making text mode selectable needs one of: a second kernel image built
without the framebuffer tag (a `make text-iso` variant, the way
`live-iso` and `demo-iso` are already separate artifacts), or a runtime
VGA mode-3 switch by banging registers directly, since there is no BIOS
`int 10h` in long mode. Neither is built. This is recorded rather than
attempted because the first is a whole second build of the kernel for a
fallback nobody has needed yet, and the second is a few hundred lines of
fragile register tables.

Note the fallback is not *entirely* unreachable in the meantime: it is
what runs on a machine where GRUB provides no framebuffer at all, which
is the case it exists for.

## The user stack's guard is an unmapped hole plus two rules, and the sbrk rule is the one that mattered

`kernel/include/kernel/uaddr.h` states the ring-3 address-space map
once -- heap base, heap limit, guard region, stack bottom and top --
and `scheduler.c`'s spawn path, `elf_run.c`'s legacy loader,
`syscall.c`'s `SYS_SBRK` and `idt.c`'s fault report all read it. It used
to be two identical copies (`PROC_USTACK_*` and `ELF_RUN_STACK_*`) with
nothing keeping them equal, and the fault classifier would have been a
third.

**The guard region has no page-table representation, and does not need
one.** It is defined by being unmapped, which is what a page that was
never mapped already does. So a stack overflow ALREADY faulted before
any of this; nothing was added to make it fault. What the constants buy
is the two things a hole cannot do for itself:

- **`SYS_SBRK` is bounded against it.** This is the real defect the
  work found. The heap grows up from `0x8000100000` and the stack down
  from `0x8000200000`, about 1 MiB apart, and sbrk had no ceiling of
  any kind -- a large enough request mapped fresh pages straight over
  the live stack, one page at a time. Nothing faulted and nothing was
  logged; the process simply found its own locals changing underneath
  it. The check is written `inc > LIMIT - brk` rather than
  `brk + inc > LIMIT` because the sum overflows for a large enough
  increment and the comparison then passes.
- **The fault gets a NAME.** `uaddr_is_stack_guard(cr2)` in the ring-3
  branch of `isr_dispatch()` turns `Page fault / CR2=0x80001fc...` into
  `Stack overflow` plus the stack's range. Ring 0 is excluded
  deliberately: the kernel's own stacks are elsewhere, so a supervisor
  fault at that address is a wild pointer and mislabelling it would be
  worse than not labelling it.

One guard page does not catch a single frame LARGER than the guard
jumping clean over it -- the classic guard-page hole, which real
kernels close with a stack-probe ABI. `UADDR_GUARD_PAGES` is there to
be widened rather than have a second mechanism grow beside it.

**Both positive controls changed the design, and neither confirmed
what it was expected to.** Removing the sbrk bound left
`userland/tests/guard_test.c` entirely GREEN, because the test asked
for 1 GiB and the guest ran out of physical memory long before it ran
out of address space -- sbrk refused for the wrong reason and every
check passed. Asking for 2 MiB instead (just past the gap, trivially
allocatable) reddened the refusal checks, and writing through the
returned pointer was needed on top of that before the corruption became
visible at all: an alias costs nothing until somebody writes. And
`userland/tests/stackovf_test.c` first hung forever without faulting,
because GCC's accumulator form of tail-recursion elimination had turned
`return frame[0] + burn(depth + 1)` into a LOOP with one reused frame
at -O2. `volatile` on the frame does not prevent that; the call goes
through a `volatile` function pointer now, and the frame is read after
the call returns. `objdump -d` is what settled it, not reading the C.

## SMAP is absolute here because the kernel copies through its own identity map, not with STAC/CLAC

CR4.SMAP faults a supervisor access to a page whose mapping has U=1.
The standard answer is to bracket every deliberate kernel access to user
memory in `STAC`/`CLAC` -- which switches the protection OFF for exactly
the window a bug would use it in, and which requires getting every
window's extent right forever.

This kernel does not do that, and **sets EFLAGS.AC nowhere at all.**
`vmm_copy_from_user()`/`vmm_copy_to_user()`/`vmm_copy_string_from_user()`
walk the process's page tables to the physical frame and copy through
the kernel's OWN identity map -- a supervisor access to a supervisor
page, which SMAP does not police. That option exists because boot.asm
identity-maps the whole low 4 GiB; a kernel without a full physmap could
not choose it.

Three things follow, in descending order of how easy they are to
forget:

- **A raw `*(T *)user_ptr` in kernel code is now a page fault**, not a
  subtle bug. That is the point: the rule is enforced by the CPU rather
  than by review. All 23 sites that used to do it -- struct copy-outs,
  path strings, the `SYS_READ`/`SYS_WRITE` bulk buffers, `SYS_LISTDIR`'s
  per-entry writes, strace's argument strings -- go through the helpers.
- **The helpers subsume `vmm_validate_user_range()` where they replaced a
  validate-then-copy pair**, and close a TOCTOU gap in doing so: the walk
  and the copy are one operation per page, so there is no interval in
  which a checked mapping can change before it is used. The validator
  still stands alone where nothing is copied.
- **`paging_make_user_page()` is the live trap.** It adds U=1 to the
  KERNEL's own identity mapping of a page, which makes that page
  SMAP-protected against the kernel's ordinary access to it, at the
  address the kernel normally uses. Nothing calls it outside `paging.c`
  today; a future caller must go through the helpers or fault in code
  that looks innocent.

SMEP (ring 0 cannot execute a user page) needed no audit -- the kernel
never executes user pages -- and is the cheaper half by far.

**Both bits are absent on QEMU's default `qemu64` model**, so the
hardware path only runs under `--cpu max`. The KTESTs are written to
assert CR4 against CPUID rather than asserting the bits are on, so they
are meaningful under both models and can fail under either. What they
CANNOT show is enforcement: the helpers never touch a user mapping, so
they behave identically with SMAP on or off. That was proved separately
by putting one raw dereference back into `SYS_WIN_CREATE` -- ring-0
`#PF`, `CR2` pointing at the client's stack, `error_code=0x1`, under
`--cpu max`, while the same build ran clean on `qemu64`.

## Heap debug mode is a runtime toggle, and `kfree()` tells the two block shapes apart by a magic that cannot be a pointer

Red-zones and use-after-free poisoning (`heap debug on`) could have been
a build flag -- `-DHEAP_DEBUG`, zero cost when off, no per-block
bookkeeping. They are a RUNTIME switch instead, for two reasons: the
mechanism is then reachable in a booted OS without producing a second
image, and one build exercises both states, so `make test` and CI cannot
silently cover only the half the Makefile happened to pick. This repo's
standing rule that a fallback nothing can reach is a guess applies to a
debug facility as much as to a driver path.

The cost is that blocks allocated before and after a toggle coexist, and
`kfree()` gets only a pointer. Its layouts are:

    off: [header][............ payload ............]
    on:  [header][span][MAGIC][ payload ][MAGIC][MAGIC]
                              ^-- what the caller holds

so it decides by reading the eight bytes immediately before the payload:
`HEAP_RZ_MAGIC` in a red-zoned block, the header's `prev` pointer in a
plain one. **That is sound rather than a heuristic, and the reason is
worth keeping**: every heap pointer is an address in the identity-mapped
low 4 GiB and therefore fits in 32 bits, while the magic's top half is
nonzero, so no `prev` can ever collide with it. Break either fact -- a
heap above 4 GiB, or a magic that fits in 32 bits -- and the two cases
become indistinguishable on the freeing path, silently.

`prev` being the header's LAST field is load-bearing for the same
reason, which is why it carries a comment saying so.

Two smaller decisions inside it:

**A detected violation quarantines the block; it does not panic.** A
real kernel panics on a corrupt heap, and that is defensible -- but a
KTEST cannot then assert that detection works, so the mechanism would
only ever be proven by a manual crash. Reporting to the log and leaking
the block keeps it testable, and leaking is the right disposal anyway:
the block's metadata is exactly what proved untrustworthy, so returning
it to the free list hands the damage to the next allocation. Quarantined
bytes are counted in neither the used nor the free total, so those two
stop summing to `heap_total_bytes()` once any violation has happened --
stated in `heap.h` rather than left to be discovered.

**`kfree()`'s plain path checks the header, always, debug mode or not.**
An underflow of 1..8 bytes lands on the magic, which makes a red-zoned
block look plain -- and `kfree()` would then take its header from 16
bytes inside the real one and unlink whatever it found there. The guard
is a `HEAP_HDR_MAGIC` field sitting in four bytes of padding the
compiler was already inserting after `free`, so it costs nothing.

`heap_check()` exists because a use-after-free is otherwise only caught
by whatever allocation happens to reuse the block -- possibly a thousand
allocations later, in an unrelated subsystem, or never.

## The kernel's relocation table is placed after `.data`, and the image that is VERIFIED is not the image that ships

Kernel ASLR (`docs/roadmap.md` M2) needs the kernel to know every
ABSOLUTE reference in its own image, so it can adjust them if the image
moves. `tools/genrelocs.py` extracts those from a `ld --emit-relocs`
link and emits a table the kernel carries; `kernel/arch/x86_64/reloc.c`
applies it. That is Linux's `CONFIG_RELOCATABLE` shape -- a build-time
relocs tool, not a PIE link -- and it works here because the low 4 GiB
is identity-mapped, so VA == PA and only the ~7,300 absolute references
need help while the ~11,900 PC-relative ones survive a move untouched.

The obvious problem is circular: generating the table needs a linked
image, and linking the table in changes the image. **The fix is
placement, not a fixed point.** `linker.ld` puts `.krelocs` after
`.data` and before `.bss`, below every section that can contain a fixup
location -- so adding the table cannot move a single address the table
records, and pass 1's entries stay correct in the pass 2 image that
contains them. Moving it above `.data` instead makes exactly 8 entries
describe the image it displaced; `genrelocs.py --verify` reports that as
a same-size, different-content mismatch and names the cause.

It must also stay before `.bss`: `.bss` is NOBITS, so an allocated
section after it would force the file to materialise gfx's 13 MB back
buffer as real bytes on disk.

**The second decision cost a non-booting kernel to find.** The final
link also uses `--emit-relocs`, because `--verify` has to re-derive the
table from the FINAL image -- checking it against pass 1 would only
compare pass 1 to itself. But an image carrying its `.rela` sections is
2 MB larger and GRUB will not boot it: the symptom is a completely empty
serial log, with no kernel output at all, which reads like a code bug
and is a link one. So the build links `kernel.pass2.elf` with `-q`,
verifies THAT, and ships `objcopy --remove-section='.rela.*'` of it.
The verified artifact and the shipped artifact are deliberately
different files, differing only in sections that are never loaded.

A third, smaller trap in the same rule: `build/krelocs.c` is named as a
prerequisite of the kernel and marked `.PRECIOUS`, because make
otherwise classifies it as an intermediate file and DELETES it once the
`.o` is built -- after which the next build's `--verify` fails with a
FileNotFoundError on a path that looks obviously correct.

### What the table's tests can and cannot prove

`kernel_relocate(0)` runs on every boot, before `paging_enforce_wx()`
(the fixups write into `.text`, which that call makes read-only with
CR0.WP, after which every one of them is a ring-0 page fault). A delta
of zero patches nothing, so what it actually proves is that the table
describes ~7,300 real, mapped, writable words in this image.

`kernel_reloc_implausible()` checks that each entry points at a word
holding a reference into the image -- but only for the READ-ONLY part,
and that limit is the interesting half. A fixup in `.data` is a pointer
the kernel initialised and is then free to reassign, to a `kmalloc`'d
block or the framebuffer, so by the time a KTEST runs a healthy `.data`
entry routinely points outside the image. Checking it anyway reports a
working kernel as corrupt, which is how the function was first written.

The upper bound also carries a page of slack, because a relocation's
value is symbol + ADDEND: `pmm.c`'s `reserve_range(0, _kernel_end)`
constant-folds its page round-up into the relocation, so the image
genuinely contains one legitimate absolute reference to
`_kernel_end + 0xfff`. Exactly one entry needs it, which is why the
bound is a measured constant rather than a guess.

Since stage 3 landed, all of this runs against an image that HAS moved,
so the checks now offset every location by the delta -- reading the bare
link-time address reaches into the abandoned image, which is still
mapped and still holds pre-relocation values, so it does not fault. It
just answers questions about a kernel that is no longer running.

What no test inside a running kernel can do is prove the relocation
itself: it cannot move the image out from under itself. That claim is
carried by the whole suite passing on a randomized base instead --
every headless run picks a different one, so 175 KTESTs, the ring-3
diagnostics, the fault tests and the 13 GUI tools are collectively an
assertion that a relocated kernel works, across many bases rather than
one.

### CR3 *does* have to be repointed, for a reason that has nothing to do with mapping

Stage 2 concluded this step was unnecessary and was **wrong**, so the
reasoning is worth stating carefully rather than merely corrected.

The original argument was sound as far as it went: `boot.asm`'s
`p4_table`/`p3_table`/`p2_tables` identity-map the entire low 4 GiB, so
they already map wherever the image lands, and the old tables stay
reserved. Nothing about MAPPING requires a change.

What it missed is that `paging.c` reaches those tables by **linker
symbol** -- `extern uint64_t p2_tables[2048]`. After relocation that
symbol names the COPIED table, so `paging_enforce_wx()` and
`vmm_map_user_page()` write into a table the CPU is not walking. There
is no fault and no error: W^X simply never takes effect, and user
mappings land somewhere nobody reads.

So stage 3 repoints CR3 at the copied tables and rewrites the two
levels of internal pointers to match. The p2 entries need nothing --
they are pure identity mappings, whose values do not depend on where
the table itself lives. Every address in that code is computed as
`symbol + delta`, because it runs from the OLD image where the bare
symbols still name the old tables; writing through them would rewrite
the tables being abandoned and reload CR3 with the value it already
held, which looks exactly like a working call.

**The general lesson is about the test, not the code.** All six W^X
KTESTs stay GREEN with this step disabled, because they read
`p2_tables` through the same symbol `enforce_wx()` wrote -- test and
code agree with each other while the hardware walks something else
entirely. The check that catches it compares CR3 against the symbol,
i.e. asks the CPU rather than the program. When a subsystem is reached
through an indirection, at least one test has to bypass that
indirection, or the whole suite can be self-consistently wrong.

### Why a lower base is refused, and why delta zero cannot exercise the copy

The relocated base is always ABOVE the link base. `pmm.c` reserves the
old image and the new one as two SEPARATE ranges -- separate rather
than one span because on a randomized base the gap between them is most
of RAM -- and the abandoned image has to stay reserved because it is
still live: the CPU uses the GDT inside it until `gdt_init()` replaces
it. A base below the link address would put the old image above the new
one, outside anything the reservation covers, and hand those frames to
the allocator. Being above also makes the copy non-overlapping, so a
forward `k_memcpy` is correct.

`.bss` is COPIED rather than zeroed, which is what lets the caller
carry on: the relocated stack already holds the live frame byte for
byte, so adding the delta to `rsp` lands on the same position within it.

Delta zero is degenerate for the copy, which is why stage 2 could not
exercise it and did not pretend to: at zero the destination is the
source, so copying or zeroing `.bss` would wipe the live stack and the
page tables the CPU is currently walking.

### The base's entropy is bounded by RAM, and cannot come from the normal RNG

`krandom_init()` cannot be called this early. It harvests jitter by
spinning until `pit_ticks()` changes, and the PIT is not initialised
yet -- so on a machine without RDSEED/RDRAND, which is QEMU's default
`qemu64` and therefore most test runs, it would spin forever. The base
gets its own minimal source instead: RDSEED, then RDRAND, then the
timestamp counter, and it REPORTS which one it got rather than letting
a reader assume the base is unpredictable.

The TSC fallback was measured rather than assumed, per the same rule
the entropy source itself was held to: five consecutive boots under TCG
produced five different bases.

The honest entropy figure is the number of candidate bases, not the
width of the random draw: 114 on a 256 MB guest, about 6.8 bits, which
matches the ~6.5 bits predicted when this was scoped. It is bounded by
RAM and by the image being ~14.5 MB in memory (gfx's 13 MB back buffer
in `.bss`), not by the random source. Linux's x86 physical KASLR gets
about 9 bits.

The relocation also cannot log -- `klog_write()` goes straight out the
serial port and `serial_init()` has not run -- so every decision it
makes is recorded in a global and printed by `kernel_main()` once it
can. And every one of those globals must be assigned BEFORE the copy,
because the copy is what carries them into the image that will actually
run; assigning after it writes only to the abandoned image, and the
running kernel would report a delta of zero while sitting at a
relocated address.

`nokaslr` on the GRUB command line turns it off -- the same spelling
Linux uses, and the recovery path if a machine turns out not to survive
being relocated.

## A blank console cell gets the console's colour, and a coloured line pads to its own edge

Two rules in `kernel/drivers/vga.c`, from one visible bug: the panic
banner painted ragged red stripes across lines that had nothing to do
with it.

**A scroll fills the incoming row with the console's DEFAULT background,
not the live `cur_bg`.** The row scrolling in is blank -- nobody has
written to it -- so it belongs to the console rather than to whatever
colour a caller happens to have set. Filling it with `cur_bg` painted a
full-width band no text had asked for, and text drawn on that row later
only repainted its own cells, leaving the rest of the band behind. This
is the same reasoning `cursor_hide()` already spells out for the cursor
cell, applied to the other place that invents blank space.

**A newline with a non-default background pads to the end of the line.**
Otherwise a coloured run's right edge is wherever its text happened to
stop, which reads as a highlight rather than as a banner -- and made the
banner's appearance depend on whether a scroll had happened to fill the
row first. Padding makes it deliberate.

Two things about where that padding lives:

- It is in `vga_putc()`, the single funnel, **so the spaces go through
  `sb_record()` as well.** Done inside `fb_putc()` alone it would look
  right until PageUp redrew the line from scrollback without it.
- The padding **replaces** the newline on screen (writing the last column
  wraps, which is the same move) but **must still record one** --
  `sb_record('\n')` is what calls `sb_start_line()`. Skipping it
  accumulated all five banner lines into a single scrollback line, and a
  PageUp/PageDown round trip redrew the banner as one stripe with four
  lines missing. Found by testing the round trip, not by reading it.

And the loop bound is computed BEFORE the first space: looping on
`col < width` does not terminate, because writing the last column wraps
`col` back to 0. That fills the screen solid red, which is at least an
obvious failure.

## The serial debug console is poll-based from existing idle loops, not a new kernel thread

`debug_console_poll()` is called from `keyboard_getchar()`'s hlt-wait
loop and `wm_run()`'s main event loop -- both already wake on every
interrupt and already have a "cheap thing to do while otherwise idle"
convention (`vga_cursor_tick()`). Piggybacking there means a serial
debug session over COM1 works without any new scheduling or
kernel-thread machinery, matching this kernel's existing
single-threaded-cooperative-with-interrupts model. The honest
limitation: it does NOT get polled while a blocking command, a
ring-3 process, or anything else that isn't one of those two loops is
running -- accepted rather than solved, since fixing it properly would
mean either real kernel threads or polling from many more places for
a debug-only feature. See the commit that added it.

Getting serial RX working at all needed two things past "unmask the
PIC": `serial_irq_init()`'s IRQ registration/unmask has to run *after*
`idt_init()`, not from `serial_init()` itself, since `serial_init()`
deliberately runs first in `kernel_main()` (so `klog_write()` has
somewhere to send its very first byte) -- before `idt_init()`'s own
"mask everything, then unmask only what's wired up" pass, which would
otherwise immediately undo an earlier unmask. And unmasking the PIC
line isn't sufficient by itself: the UART's own Interrupt Enable
Register also has to be set (`serial_init()`'s original `outb(COM1+1,
0x00)` leaves it at 0, since nothing needed RX before this). Found
live -- a fully-correct-looking IRQ handler + PIC unmask produced zero
bytes, not even local echo, until the IER bit was added.


## The syscall table is hand-written, and one row carries the handler AND the trace description

`kernel/proc/syscall_table.c` holds one row per syscall number --
`{ name, handler, argument kinds, return kind }` -- and
`syscall_dispatch()` is a bounds-checked call through it. Before this,
dispatch was one `if/else` chain of 37 branches over 40 syscalls in a
1,492-line `syscall.c`, and `strace` carried a SECOND table keyed by the
same numbers.

**Why a table at all.** The chain grew with every capability, and the
handlers had nowhere to live but the one file. Linux and Windows NT
agree on the alternative: Linux defines each call with
`SYSCALL_DEFINEn()` in the subsystem that owns it (`read`/`write` in
`fs/read_write.c`, `fork` in `kernel/fork.c`) and dispatches through
`sys_call_table[]`; NT's SSDT is an array of service pointers with the
implementations in Io/Ob/Ps/Mm. Neither has a dispatch chain, and
neither keeps the implementations together -- the table is the only
central thing. The handlers here now live in `kernel/proc/syscall_fd.c`,
`kernel/fs/fs_syscalls.c`, `kernel/proc/proc_syscalls.c`,
`kernel/proc/win_syscalls.c` and `kernel/core/sys_syscalls.c`.

**Why the trace description shares the row.** This is where toy-os
deliberately DIFFERS from both, and the comparison is worth stating
accurately because it is easy to assume otherwise. Linux's
`sys_call_table[]` holds function pointers and nothing else; the names
live in `arch/x86/entry/syscalls/syscall_64.tbl`, consumed at build
time, and the kernel's own per-syscall metadata (`struct
syscall_metadata` -- name, argument count and types, emitted by the same
`SYSCALL_DEFINEn` macro) is a SECOND structure, used by ftrace's
`sys_enter`/`sys_exit` tracepoints. `strace(1)` reads neither: it is a
userspace `ptrace(2)` program with its own generated per-architecture
tables. NT is the closest precedent -- the SSDT is paired with
`KiArgumentTable`, indexed by the same number -- but that is still two
arrays.

One merged row is a small-system simplification, and it was chosen
because the drift it prevents had already happened: `strace`'s private
table stopped at `SYS_GETRANDOM`, so fourteen syscalls -- process
control, the whole window protocol, the settings registry -- traced as a
bare `syscall_<n>`, some for months. Nothing tied the two lists
together, so nothing could notice. At 40 syscalls on one architecture,
the cost of merging is that a row is wider; the cost of not merging was
paid already.

**Why it is hand-written and not generated.** The plan for this work
(`docs/roadmap.md`, before it was struck through) called for generating
the table from `abi/syscall_abi.h`, following `gen_syms.py` and
`genrelocs.py`, reasoning that generation preserves the compile-time
guarantee that a number is defined exactly once. It does not buy that:
designated initializers (`[SYS_WRITE] = { ... }`) already give it. An
undefined `SYS_*` is a compile error, and `-Wextra`'s `-Woverride-init`
turns a duplicated index into an error rather than a silent last-wins.
What generation would add is a parser over a header that is mostly prose
comments, and a build step whose correctness depends on those comments'
formatting. Linux generates because it has six architectures and 400
calls; that is the size, not the shape, and copying it here would be
copying the size.

**Why a handler reports "I parked" separately from its return value.**
The uniform signature is `int handler(struct syscall_ctx *c)`: the
handler writes its own return value into `c->regs[14]` and returns 1
only if it blocked the caller. The obvious alternative -- return the
value, with a sentinel for "blocked" -- has no safe sentinel here.
`SYS_SBRK` returns a pointer, so every 64-bit value is a legitimate
result, and a handler that parked must not write `regs[14]` at all
(the wake writes it; see `SYS_WAIT_EVENT`'s comment). Keeping the
return value where the branches already put it also made the conversion
a move rather than a rewrite of 37 bodies.

**The structural bonus, and it is measurable.** Each handler now has its
own stack frame, so no syscall can put its locals on every other
syscall's frame. `syscall_dispatch()` went from 864 bytes to **96**
(`-fstack-usage`); the largest handler frame, `sys_win_debug` at 576, is
now paid only by `SYS_WIN_DEBUG`. That is the same class of bug that
made the dispatcher 4832 bytes and cost a kernel-stack overflow to find
-- structural now rather than a rule someone has to remember.

Adding a syscall is three edits with no registry to forget: the number
in `abi/syscall_abi.h`, the handler in the subsystem that owns it with
its prototype in `kernel/include/kernel/syscalls.h`, and a row in
`syscall_table.c`. `kernel/proc/syscall_test.c` asserts no row is
half-filled in, and that `strace_syscall_name()` returns the table's own
string POINTER -- a private copy in `strace.c` holding identical text
would fail that.

## A user mapping records whether it OWNS its frame, in a spare PTE bit

`vmm_destroy_address_space()` walks a dying process's page tables and
frees every frame it finds. That is correct only while every mapping in
an address space owns its frame -- and that stopped being true the
moment anything was shared into a process.

**The bug this was found through.** Every ring-3 GUI client maps the
kernel's own glyph tables read-only at `WIN_FONT_VADDR` -- that is the
whole point of `WIN_REQ_FONT`, one instance of the font in memory
rather than a copy per client. Nothing unmapped them on exit. So an
exiting GUI client's teardown called `pmm_free_frame()` on pages of the
KERNEL IMAGE, putting them back in the physical allocator while the
kernel, the desktop and every other client were still reading them.

Measured on a fresh boot: opening and closing Calculator once returned
**four frames** of kernel `.rodata` to the allocator. A non-GUI process
returned zero, which is what identified the borrowed mapping rather
than process teardown generally as the cause.

**Why nobody had noticed, and this is the part worth carrying
elsewhere: an over-free shows up ONCE and then goes quiet.**
`pmm_free_frame()` only counts a frame that was marked used, so the
second client to exit finds the same font frames already free and
changes nothing at all. The first measurement of this looked like
`+4, +0, +0, +0` -- which reads as noise followed by a clean bill of
health, and was very nearly written off as exactly that. Any accounting
check for this class has to run on a fresh boot and believe only its
first cycle. Nothing broke visibly because a freed-but-still-mapped
frame is harmless right up until the allocator hands it out and
somebody writes to it.

**The fix: mappings state their ownership.** Bits 9-11 of a PTE are
ignored by the hardware and reserved for the OS; `PAGE_BORROWED` is one
of them. `vmm_map_user_borrowed()` sets it, and `destroy_pt()` unmaps
such a page without freeing the frame. The rule for a caller is a
single question -- *who calls `pmm_free_frame()` for this frame?* If the
answer is not "this address space's teardown", the mapping is borrowed.

Five call sites were, and each was a live over-free waiting for its
address space to die: the font (kernel image), the real framebuffer
(`WIN_FB_VADDR` and `SYS_GUI_INIT`'s `GUI_FB_VADDR`), a client window
buffer (the window server allocates it with `pmm_alloc_contiguous()`
and frees it in `destroy_window()`), another process's buffer shared
into the compositor, and the poison page -- which was the worst of
them, being ONE frame mapped at every page of a slot, so an owning
teardown would have freed the same permanent singleton dozens of times.

**Why a PTE bit and not a side table.** The teardown walk has the PTE
in hand and nothing else -- no VMA list, no `struct page`. A side table
would have to be kept in step with every map and unmap, which is the
same class of second-source-of-truth this repo keeps deleting. Linux
reaches the same answer from the other end: `vm_normal_page()` exists
precisely to ask whether a mapping has an owning `struct page` behind
it, and `VM_PFNMAP` marks the ones that do not.

**What this is NOT.** It is not refcounting, and it does not make a
frame shareable between two owners -- `PAGE_BORROWED` says "somebody
else frees this", not "count me". Copy-on-write and `MAP_SHARED` need a
real per-frame refcount in `pmm`, and that is `docs/roadmap.md`'s
demand-paging milestone. This is the narrower fact that had to be true
first, and was not: an address space must not free what it does not
own. `tools/frame_balance.py` is the standing check, and
`kernel/mm/uaccess_test.c` carries the pair of KTESTs -- an owned frame
comes back, a borrowed one does not -- that are each other's control.


## A killed process's address space is destroyed at KILL time, and needs its own entry point

`scheduler_kill()` marked the victim `SCHED_ZOMBIE` and
`scheduler_poll()` reaped it by marking the slot `SCHED_UNUSED`. Neither
freed any memory: `syscall_process_exit_cleanup()` ran only from
`sys_exit`, i.e. only when a process ended ITSELF. Every kill therefore
leaked the victim's whole address space -- ELF pages, stack, heap, any
window buffer -- plus its open fds, for the rest of the boot. Measured
at ~18 frames per kill, compounding, and reachable from the desktop
because Force Quit goes through `scheduler_kill()`.

**Why at kill and not at reap.** A zombie exists to hold an exit code
for whoever waits on it. Holding an entire address space as well buys
nothing -- nothing may run in it again -- so the memory should go at
death. That is also how Linux splits it: `exit_mm()` drops the `mm_struct`
when the task dies, while the `task_struct` lingers until it is reaped.
Waiting for the reap would also mean a process nobody ever waits on
holds its memory forever, which is the common case here: `gui spawn`
has no waiter at all.

**Why it could not reuse the exit path, which is the interesting part.**
`syscall_process_exit_cleanup()` switches CR3 to the kernel's address
space before destroying the tables, because freeing the frame CR3 still
points at is a use-after-free. That is free when the dying process is
the one running -- it is leaving anyway. It is WRONG when the caller is
somebody else: the window manager force-quitting a client is still
running in its own address space, and moving CR3 out from under it would
resume it somewhere else entirely. So `syscall_process_kill_cleanup()`
destroys the victim's tables and leaves CR3 alone, which is sound
precisely because they are not the live ones. It refuses loudly if
handed the caller's own address space -- leaking is survivable, pulling
CR3 out from under a running process is not.

**Ordering that matters.** The teardown runs AFTER
`win_server_client_gone()`, which unmaps this process's window buffers
from the compositor and clears the compositor role if this was the
desktop. Both of those reach into address spaces, so they have to happen
while this one still exists. `procs[slot].pml4_phys` is zeroed
immediately afterwards so nothing can follow it again.

Found by `tools/frame_balance.py`, which is also the standing check:
free frames must return to baseline across a spawn/kill cycle as well as
a spawn/exit one. Its positive control is the original measurement --
reverting the fix takes the kill cycles from flat to -18 each.

## The audit compares page tables against the allocator, and only in the direction that is cheap

`meminfo audit` (`api/mm_audit.h`, walking with `vmm_audit_space()`)
walks every live process's page tables and asserts one invariant: **a
frame a live mapping points at must be one `pmm` considers handed out.**
A present mapping of a FREE frame is memory the allocator may give to
somebody else while the process is still reading and writing it.

**Why it exists.** Two bugs on 2026-08-18 were both exactly this, and
both were silent: an exiting GUI client returned pages of the kernel
image to the allocator (the font mapping it had never unmapped), and
`scheduler_kill()` freed nothing at all. Neither cost anything at the
moment it happened -- a freed-but-still-mapped frame behaves perfectly
until the allocator hands it out and somebody writes to it -- so nothing
in a 300-check GUI suite could see either. The invariant above is
checkable in one walk and would have caught both.

It does catch them, and that was verified rather than assumed: with the
`PAGE_BORROWED` fix reverted, closing one Calculator leaves the
still-running desktop holding **5 dangling mappings at `0x8003000000`**
-- the font region -- named with the address and the frame.

**Why only that direction.** The reverse -- a frame marked used that
nothing references, an ordinary leak -- is not symmetric. Page tables,
the kernel heap, the kernel image and any DMA buffer all hold frames no
page table points at, so a naive sweep reports every one of them.
Answering it needs each kernel-side owner to declare what it holds,
which is roughly what a `struct page` array buys a real kernel, and is
its own project (`docs/roadmap.md`). Claiming a "leak detector" that
only worked in one direction would be worse than naming the one it
does.

**Two things about reading its output.** `unmanaged` is NORMAL and not a
finding: a framebuffer is MMIO, never RAM the firmware reported, so
`pmm` has nothing to compare against -- the desktop legitimately shows
~900 such pages, and telling "not mine" from "mine and free" is exactly
why `pmm_frame_is_managed()` is separate from `pmm_frame_is_used()`. And
BORROWED pages are audited too, deliberately: a borrowed mapping whose
real owner has already freed the frame is precisely the use-after-free
worth catching, so excusing them would audit away the interesting half.

**Where it lives, and why not in the shell.** The first version put the
walk in `apps/shell_sys.c` and did not compile -- `kernel/include/kernel/`
is off `apps/`'s include path, so `vmm.h` is unreachable from there.
That boundary was right: walking page tables is not something an app may
do. The walk is `kernel/mm/mm_audit.c` and `apps/` sees one function
that prints a report.

## init idles with SYS_SLEEP, and the alternatives were both worse

Stage 1 of `docs/init-design.md` gives toy-os an init at pid 1 whose job
is to reap orphans. The plan said it "reaps orphans in a `waitpid(-1)`
loop and otherwise sleeps", and building it found that *there was no
otherwise*: nothing in this ABI could sleep.

`waitpid(-1)` blocks perfectly well while init has children. The problem
is the other state, which at stage 1 is nearly all of the time -- the
desktop is still started by `gui` from the kernel context, so init is
nobody's parent until some process dies leaving children behind. And
"no children at all" is a permanent -1 by design (stage 0 wrote that ABI
comment deliberately: a caller conflating it with "not yet" either spins
forever or stops reaping).

Three ways out, and the cost of each is what decided it:

- **Yield-spin.** No new mechanism, and init then runs flat out forever.
  This repo has already paid for one process reading a fake 100% CPU and
  the confusion it caused; making pid 1 permanently busy would poison
  every CPU figure in Task Manager and every measurement taken from one.
- **Let `waitpid(-1)` park with no children**, woken by any exit --
  which is what `scheduler_wake(SCHED_WAIT_CHILD, ...)` already
  broadcasts, so it is SIGCHLD in all but name and costs zero idle
  wakeups. Rejected because it contradicts the documented answer for
  every OTHER caller: a program asking "do I have children?" would hang
  instead of being told no, and the trap is invisible at the call site.
- **A real sleep.** `SYS_SLEEP` parks on a deadline and the timer tick
  releases it.

The third is what real systems have, even though none of them idles
their init this way: Linux's init blocks in `epoll_wait` and is woken by
SIGCHLD, because it has file descriptors and signals to wait on. toy-os
has neither, so the honest primitive is a clock. init blocks in
`waitpid(-1)` when it has children and sleeps when it does not, which is
also the shape stage 2 needs -- a restart rate limit ("N restarts in T
seconds") is a clock, not an event.

**The design points inside it.** The deadline is computed once, from
`clocksource_now_ns()`, rather than counted down per tick: a sleeper is
not running and cannot decrement anything, and a duration counted in
ticks drifts with whatever else the machine is doing. The wake is
deadline-aware and therefore separate from `scheduler_wake()`, which
releases *every* process parked on a reason -- right for "a pipe has
data", wrong for "it is 09:00". The resolution is one timer tick and a
sleep never returns EARLY, because programming a one-shot timer per
sleeper is a real tickless design this kernel does not have; promising
better precision than the clock the wake loop runs on would be a lie.
And a caller with no scheduler slot -- the legacy loader, kernel code --
gets -1 rather than an instant return, because returning 0 would say
"you slept" and a polling loop would spin on it. That is why
`/tests/sleep_test` is excluded from `usertest_run.py`, which drives
everything through `run`.

## A fire-and-forget spawn is handed to init at the CALL SITE, not in the spawner

Adoption fixes orphans -- children whose parent died. It does nothing
for the other way a zombie becomes permanent: a parent that is alive and
simply never waits.

The kernel context is exactly that parent, permanently. It is not a
process, so it never dies (nothing to trigger adoption) and it can never
call `waitpid` (nothing to trigger a reap). The shell's `spawn` is
fire-and-forget by definition, so before this every `spawn` leaked a
process-table slot the moment its child exited -- silently, until the
table filled up. `tools/init_test.py` caught it on its first run, as
`{2: (0, 'zombie', 'orphan_test')}` left behind after the four children
it abandoned had all been reaped correctly.

The obvious fix is to have `scheduler_spawn()` give every
kernel-context spawn a ppid of init. That is wrong, and the reason is
worth keeping: **`gui` and the KTESTs spawn and then poll the pid
themselves.** `scheduler_poll_any()` reaps any zombie whose ppid names
init, so init would race those callers for the corpse -- `gui3_main()`
would report a bogus exit code for a desktop that exited cleanly, and
`proctree_test.c`'s assertions on `scheduler_poll(parent)` would fail
intermittently.

So the choice belongs to the call site, which is the only place that
knows whether it intends to wait: `cmd_spawn()` calls
`scheduler_reparent(pid, scheduler_init_pid())` and `gui` does not.
That is what `scheduler_reparent()` was built for -- adoption as a
deliberate act rather than a policy applied to everyone.

## Init is refused by ASKING which pid it holds, not by testing `pid == 1`

`scheduler_kill()` refuses init, as Linux discards SIGKILL to pid 1 and
for the same reason: killing it leaves every orphan unreapable and
nothing supervising anything.

It is written as `pid == g_init_pid`, with `g_init_pid` set once from
`kernel_main()` after the spawn succeeds. Testing `pid == 1` would have
been shorter and would have been wrong on the boot that matters most --
the one where `/bin/init` is missing or fails to spawn. That boot is
supported and deliberately quiet: `scheduler_init_pid()` stays 0, so
orphans are left parentless exactly as they were before init existed,
slot 0 is an ordinary process again, and nothing is mysteriously
unkillable. A hardcoded 1 would have made whatever landed in slot 0 on
such a boot -- the desktop, most likely -- refuse to die, with no
mechanism anywhere naming why.

The same reasoning runs through `reparent_children()` (the heir is
`g_init_pid`, which is 0 when there is no init, which is the old
behaviour exactly) and through `proctree_test.c`, whose adoption check
asks for the heir rather than hardcoding either value -- so one test
covers a normal boot and a boot with no init.

## sbrk RESERVES and the page arrives on touch -- and the copy helpers need the hook, not just the fault

The heap was ~14 MiB per process, bounded by where `WIN_CLIENT_BASE`
happened to sit, and `SYS_SBRK` mapped a frame for every page it moved
past. Both halves had to change together, and the second is why:
reserving ~2 GiB is only affordable because nothing is spent until it is
touched. A limit raised without demand paging would have meant handing
2 GiB of real frames to a process that asked for address space.

**What sbrk does now.** It moves a number and maps nothing. The break is
the process's claim; the frame arrives on first touch. The consequence
to know is that **sbrk can no longer report out of memory** -- it
refuses only a request past `UADDR_HEAP_LIMIT`, and the machine running
out is discovered at a page that cannot be given, which kills that
process. That is overcommit. Linux makes the same trade and backs it
with an OOM killer; here the fault is fatal to the process that took it,
which is a smaller blast radius than a kernel that cannot allocate.

**THE PART THAT IS EASY TO MISS: most of this kernel's user-memory
access never faults.** Ring 0 does not dereference user pointers -- it
walks the page tables and copies through its own identity map, which is
what makes SMAP absolute here with no STAC/CLAC window. So a buffer that
`sbrk` reserved and ring 3 has not touched, handed to `sys_read()`,
produces no #PF at all: it produces a walk that finds nothing, and the
syscall reports a perfectly legal buffer as a bad pointer. Demand paging
that only hooks the fault handler silently breaks every syscall taking a
caller-allocated buffer.

Hence a registered hook (`vmm_set_fault_handler`, the same shape as
`display_driver` and `block_device`) rather than a line in `idt.c`, with
three callers: the #PF handler, `user_phys_of()` inside the copy
helpers, and `vmm_validate_user_range()`. vmm cannot answer "is this
address inside somebody's heap" itself -- the break lives in the process
layer -- so that layer registers the answer.

**Its positive control is worth repeating before touching any of it.**
Disabling the retry in `user_phys_of()` ALONE reddens nothing, because
`sys_proc_info()` validates its pointer first and the validate path has
its own fault-in. Both had to be disabled for `guard_test`'s two new
checks to fail -- and they were the only two that failed, which is what
makes them load-bearing. A redundant path is exactly the shape that
makes a control look like it is measuring nothing.

**Two bounds, deliberately.** `sys_sbrk()` refuses a request past the
limit, and the fault handler independently refuses any address outside
`[UADDR_HEAP_BASE, UADDR_HEAP_LIMIT)` and past the caller's own break.
Either alone would do on a correct kernel; together, a bug in one still
cannot map a page over the stack, which is the failure this heap has
already had once.

**And `mapped_end` was DELETED rather than kept.** `struct sched_heap`
tracked how far pages had actually been allocated behind the break. With
demand paging the page tables already record which pages exist, and a
second record of the same fact can only ever disagree with them --
silently, and in the direction that matters (a page believed mapped that
is not). Same instinct as deleting the `fb_present_pending` flag that
duplicated `gfx.c`'s dirty box.

## The ring-3 map is sized for 4K, and the compositor region is why it had to be re-read as a whole

Raising the heap meant moving `WIN_CLIENT_BASE`, and the first attempt
moved it to `0x8080000000` -- straight into the middle of the
compositor's region. `WIN_COMPOSITOR_BASE` was `0x8010000000` and spans
`WIN_COMPOSITOR_MAX_PIDS * WIN_CLIENT_MAX * WIN_BUFFER_STRIDE`, which
was 2 GiB. The space below it looked spare and was not.

**The rule that falls out: in a map of derived regions, what the next
thing must clear is a region's END, never its base.** Three of the four
windowing addresses here are computed (`WIN_FONT_VADDR` from the client
base and stride; every window's address from the pair (pid, id)), so a
constant that looks isolated is the start of a range whose size lives
somewhere else entirely.

While the addresses were moving anyway, the map was sized for a 4K
display rather than for today's 1280x720, because virtual address space
is the one resource here that costs nothing:

- `WIN_BUFFER_STRIDE` 8 MiB -> 64 MiB. A 3840x2160x4 buffer is 31.6 MiB,
  so the old stride could not hold one however the other caps were set.
- `WIN_CLIENT_BASE` -> `0x8080000000`, `WIN_COMPOSITOR_BASE` ->
  `0x80A0000000` (16 GiB), `WIN_FB_VADDR` -> `0x8500000000`.
- The heap gets everything below the stack: ~2046 MiB.

Everything still sits inside one PML4 entry, with ~468 GiB spare above
the framebuffer.

**What this does NOT do is make 4K work**, and saying so is the point of
writing it down. Two things still bound it, neither of them addressing:
`WIN_CLIENT_MAX_W/H` is 1280x720, and a window buffer comes from
`pmm_alloc_contiguous()` -- 31.6 MiB is 8192 CONTIGUOUS frames from a
bitmap allocator with no buddy system, which fragmentation can refuse,
silently and by design (a refusal is a normal protocol outcome,
indistinguishable from a client declining). Raising the caps before
buffers are non-contiguous would convert a hard limit into an
intermittent silent failure, which is worse. See `docs/roadmap.md`.

## malloc is the KERNEL's allocator compiled twice, not a second one

Ring 3 had no allocator at all: `SYS_SBRK` was the only
allocator-adjacent syscall, grow-only, with nothing on top. Every
Toykit app therefore sized its state statically, and the compositor's
back buffer was a single lifetime allocation that is never freed.

The obvious implementation is a small `malloc` in `userland/lib/`. This
repo's standing rule says otherwise, and an allocator is the worst place
to break it: `kernel/mm/heap.c` (as it then was) was already a
first-fit, address-ordered,
coalescing free list with red-zones and use-after-free poisoning behind a
runtime toggle, all of it covered by KTESTs. A second implementation
would have started as a subset and drifted, and the drift would surface
as memory corruption rather than as a behaviour difference anyone could
see.

So it follows `geom.c`, `kfmt.c` and `etc_config.c`: the allocator moved
to `kernel/lib/heap_core.c` and is compiled twice, with everything
platform-specific behind three functions in `api/heap_os.h` --
`heap_os_alloc` (pmm frames / sbrk), `heap_os_report` (klog / stderr)
and `heap_os_should_fail_alloc` (fault injection / never). Ring 3 gets
the C names through `userland/lib/stdlib.h`.

**The state stays in the file's statics rather than becoming a context
struct**, which is deliberate and is what kept the diff small: each
compilation unit gets its own free list, and the kernel's heap and a
process's heap are separate address spaces that must never hand out each
other's memory. A `struct heap *` parameter would have expressed the
same thing while touching every line.

**Two things a caller inherits from sbrk.** Nothing is ever returned to
the kernel -- `free()` puts a block back on this process's list and the
break never moves down, so a process that allocates 500 MiB and frees it
still holds it. And a fresh region's pages do not exist yet: the
allocator writes a block header into it immediately, so the first page
faults in there, and the rest arrive as the program uses them. Both go
away with `mmap`/`munmap`, which is a later step of the demand-paging
milestone and is one function's worth of change here.

**The test's coalescing check was not load-bearing at first, and the
reason generalises.** It asked whether the process's footprint grew
after freeing three 4 KiB blocks and requesting 12 KiB -- and stayed
GREEN with `try_merge_next()` disabled outright, because the allocator
claims memory in 64 KiB regions, so all three blocks sat inside one and
the region's own leftover satisfied the request either way. The fixture
never reached the branch. It compares the returned ADDRESS now: the list
is address-ordered and first-fit, so a merged block starts where the
first of the three did, and an allocator that did not merge cannot
return that address. That version reddens exactly one check.
