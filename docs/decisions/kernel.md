# Decisions: Kernel, memory and processes

Scheduling, address spaces, syscalls, the process model, and the traps that live under them.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

Write the reasoning HERE, in full: an entry that cannot be understood
without opening something else is not finished.

---

## A crash report is a text header plus the raw stack, not an ELF core

The maintainer asked for something to read after a process crashed
(2026-09-02). Linux writes an ELF core -- every mapped page with
NT_PRSTATUS notes, loaded by gdb; Windows writes a minidump of the
registers, the stack and the module list; macOS writes a text report
with a symbolised backtrace. toy-os writes the last two in one file:
the text macOS writes, then the stack bytes Windows keeps.

Not an ELF core, because the value of a core is a debugger that
understands it, and this OS has none: gdb would need a target
description for the layout, and the work is the writer AND the reader.
What is actually wanted after a crash here is the one thing the kernel
already declines to do -- walk a user stack for return addresses -- and
a few kilobytes of stack plus `addr2line` on the host answers it.
`panic_resolve.py --crash` scans every word of the saved stack for an
address inside the ELF's executable segments, which finds the frames a
frame-pointer walk would and some it would not, in the same shape the
kernel's own panic scan uses.

The text half is there so the file is readable with `cat` on the
machine, which the macOS report has right and a minidump has not: the
first question after a crash is "which program, which fault", and
answering it must not need the host.

**Kept apart from a kernel panic on purpose.** `/var/crash` holds
processes that faulted while the kernel carried on, and the roadmap's
crash-reporting milestone insists the two failure classes never share
a list. A panic's record is a RAM store recovered on the next boot,
pstore's shape, chosen over writing to disk from the panic path because
a panic inside the storage stack cannot use the thing it would write
through -- see "A panic keeps its log in RAM at a fixed address, and
logd files it" below.

## A panic keeps its log in RAM at a fixed address, and logd files it

Built 2026-09-25. When the kernel panics it copies the log ring into
32 KiB of RAM at physical 32 MiB, counts down (`panic=`, default 10 s)
and resets; the next boot checks the record's CRC and logd appends it to
the end of the DEAD boot's own log file, then clears it.

**Why RAM and not the disk.** Windows writes its dump through a separate,
minimal storage stack prepared at boot for exactly this; toy-os has one
storage stack and a panic may be inside it, so the panic path touches no
filesystem, lock or heap. Linux's pstore/ramoops is the shape copied: a
reserved range, a header with a checksum, recovered on the next boot. It
only survives a WARM reset, which is why a panic now restarts the machine
instead of halting -- a halted machine gets power-cycled and the record
goes with it.

**Why a constant address.** ramoops takes its range from a boot
parameter or the firmware's device tree; the range has to be known
before anything could look it up here, because KASLR copies the kernel
somewhere random before `kernel_main()` and pmm hands out frames right
after. 32 MiB is above the kernel image (about 10 MiB) and below where
GRUB's heap grows DOWN from the top of RAM. `panic_store_probe()`
refuses the range with a reason in the log -- not RAM, the image grew
into it, GRUB put something there -- rather than trusting it, and a
record that did not survive fails its CRC, so the failure is "no
record", never a wrong one.

**Why the dead boot's file and not a file of its own.** The record is the
tail that boot's log LOST: logd died with the machine, so its last lines
never reached the disk. Appending them where they belong -- minus the
lines logd did persist, found by the file's last kernel line -- means
`log -p 1` simply ends with the panic. systemd-pstore archives to a
directory of its own; that would be a second place to look for the end
of one boot.

**Why a keypress ends the countdown but a serial byte does not.** A test
harness's resync bytes arrive on COM1 unasked, and ended the countdown
after two seconds. Only a make code on the PS/2 keyboard counts, after
draining what was already buffered -- otherwise the release of the Enter
that ran the fatal command restarts the machine at once.

## The physical map is the identity map, extended -- not Linux's higher-half direct map

Planned 2026-09-02, before any of the ">4 GiB" work is built, because
the obvious answer is the wrong one here. Linux keeps a linear map of
all RAM at `0xffff888000000000` and converts with `__va`/`__pa`; the
offset exists to free the low addresses for user space, and every
kernel that copied the layout inherited the conversion at every
physical access. toy-os's user space is not in the low range -- the
ring-3 map (`uaddr.h`) starts hundreds of GiB up -- so the only reason
for the offset does not apply, and a linear map at offset ZERO is the
existing identity map made longer.

What the audit found is that physical == virtual is load-bearing in
about 180 places, none of them accidental: the kernel heap is a frame
cast to a pointer, every DMA ring is programmed with the address it is
reached by, the user-copy helpers dereference the frame they walked to
(which is what makes SMAP absolute here), and KASLR's relocation skips
the P2 entries because identity entries do not depend on where the
table lives. A higher-half map would convert all of it for no property
this kernel needs.

The cost accepted: the map is bounded by the first user region, so a
machine with more RAM than that cannot use it all -- a limit no
hardware this project will run on approaches. And frames above 4 GiB
are handed out by ZONE, with every existing caller on `DMA32`, because
the ATA PRD and AC97 BDL registers are 32-bit by specification and a
missed caller is a silent DMA into the wrong 4 GiB.

**Built 2026-09-02, with one exception the plan had not seen: MMIO.**
The extension itself went as planned -- 2 MiB slots only, because
QEMU's default CPU has no 1 GiB pages and a path the gate never runs
is not worth a second path; page directories from `DMA32`; the high
zone managed at 2 MiB granules so a managed frame is always a mapped
one; every walker in `paging.c` descending from the boot PDPT instead
of indexing a 2048-entry array. What the plan got wrong was that a
64-bit BAR could be identity-mapped: on an 8 GiB machine SeaBIOS puts
the PCI 64-bit window at 768 GiB, which is INSIDE the ring-3 half, and
no identity mapping of it can exist. So `paging_map_device()` is
`ioremap` after all -- a window above 4 GiB gets a virtual slot in the
top 32 GiB of PML4[0] (`UADDR_KDEV_BASE`), uncached, and the driver
keeps the pointer. Below 4 GiB it still returns the physical address,
write-back as before; retyping those is the separate change
`docs/decisions/drivers.md` describes.

**Every CPU-only consumer moved on the same day.** User stacks, the
stack and heap fault-ins, mmap's anonymous and file-backed pages, ELF
images, the compositor's window buffers, and page tables -- which the
plan had not listed, and which are reached through the identity map
like anything else, so there was never a reason for them to be low.
What stays `DMA32` is what a device reads, plus the page directories
that CREATE the high map: those cannot live in the memory they are
about to map.

## Swap picks its victims by walking FORWARD, and builds no reverse map

Every real system that swaps can answer "which page tables point at this
frame?". Linux builds `anon_vma` chains for it; Windows NT has the PFN
database and prototype PTEs. toy-os deliberately answers a different
question instead, and it is worth recording why, because the missing
structure looks like an omission.

**A reverse map exists to disambiguate.** It is needed when one frame
can be referenced by many PTEs, which is what `fork()`, copy-on-write
and `MAP_SHARED` create. toy-os has none of the three: there is no
`fork()`, no COW, and no shared anonymous memory. So a swappable page
has exactly ONE page table entry, and the reclaimer can pick a process,
walk its page tables forward, and take pages -- it never has to start
from a frame.

**The candidate rule was already in the tree, under another name.** A
page may be evicted iff its PTE is present, OWNED (not `PAGE_BORROWED`)
and pointing at a frame pmm manages. `PAGE_BORROWED` means "this address
space does not own this frame", and every mapping that must never be
evicted is borrowed for exactly the reason that disqualifies it: the
sound ring is a live DMA target, window buffers are contiguous runs a
per-page evictor cannot break up, the shared font is kernel image, the
compositor's poison page is many PTEs onto one frame, and shm and the
`/lib` image cache are shared between processes. That alignment is not
luck -- it falls out of `PAGE_BORROWED`'s own rule, "who calls
`pmm_free_frame()` for this frame?" -- but it is load-bearing, so
`vmm_set_swap_entry()` is the single place that applies it.

**One mapping escapes the bit**, and a reclaimer has to exclude it
separately: `win_syscalls.c` maps the raw framebuffer into a legacy GUI
client as OWNED, not borrowed. It is harmless today only because
framebuffer frames are unmanaged, so freeing one is a no-op. Hence the
"and managed" half of the rule.

**What this costs, stated plainly:** a future `fork()` or `MAP_SHARED`
invalidates the premise, and reclaim would have to stop until there is a
per-frame refcount -- which is the same prerequisite `fork()` already
carries on the roadmap, so the two arrive together or not at all. The
alternative was to build rmap first, for a machine with nothing to
disambiguate, and carry it unexercised until fork lands.

*Fork landed on 2026-09-11 with that refcount, and the premise was
kept rather than replaced: `vmm_set_swap_entry()` refuses a frame with
more than one owner. A shared frame is simply not a candidate, which
is what a machine with no reverse map can honestly say; the rmap is
still deferred, now until page-out is real and a forked shell's pages
are what it wants to evict (see "fork shares frames..." below).*

See `docs/swap-design.md` for the staged plan this belongs to.

## An ANY allocation stops at a DMA32 floor rather than draining it

Built 2026-09-02, with stage 3 of ">4 GiB", because moving user pages
to `PMM_ZONE_ANY` created a failure the zones existed to prevent.

`ANY` prefers the high zone and falls back into DMA32 when it is empty.
On an 8 GiB machine that fallback effectively never runs. On a 5 GiB
one it runs constantly: a gigabyte of high memory goes quickly, and
after that every user page, page table and window buffer comes out of
the zone a 32-bit DMA engine can reach. The first driver to want a
descriptor ring is then refused while `meminfo` reports gigabytes free
-- a failure that reads as a driver bug and is an allocator policy
mistake.

The obvious answer is to do nothing and let it happen, on the grounds
that the machines this runs on have plenty of high memory. That is true
of the 8 GiB laptop and of the QEMU guests, and it is exactly the
reasoning that makes the bug arrive on the machine nobody tested.

So the fallback keeps a floor: a sixteenth of DMA32, clamped to
[16 MiB, 128 MiB] and never more than half the zone. A fraction rather
than a constant because the devices needing low memory scale with the
machine. Zero when there is no high zone at all, since a floor there
would only lose memory with nothing to fall back FROM. A caller that
NAMES `DMA32` ignores it entirely -- the floor exists to keep memory
FOR those callers, so applying it to them would be backwards.

This is Linux's `lowmem_reserve_ratio`, simplified. Linux computes a
per-zone reserve from the size of the zones ABOVE it, because it has
several and the fallback order is long; toy-os has two, so the
arithmetic collapses to one number. Windows does not need an equivalent:
its PFN database is flat and a driver asks for below-4GB memory
explicitly through `MmAllocateContiguousMemorySpecifyCache`, which is
the same bargain reached from the other side.

The value is SETTABLE (`pmm_set_dma32_reserve_frames()`), which is what
makes the refusal testable: enforcement only fires when the fallback
runs, and a machine with a high zone almost never runs it. The `mm`
KTEST jams the floor to four frames below what is free and requires the
next `ANY` request to be refused while the same request naming `DMA32`
succeeds. That check runs on the ORDINARY 256 MiB boot and skips at
8 GiB -- the inverse of every other check in this milestone.

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

**THE PREMISE STOPPED BEING TRUE, AND THE MARGIN OUTLIVED IT.** No
path-taking syscall validates a fixed range any more: `resolve_user_path()`
(`kernel/fs/fs_syscalls.c`) copies with `vmm_copy_string_from_user()`,
which walks to the NUL and "fails only on an unreadable page", and
`SYS_LISTDIR` now range-validates its OUTPUT array rather than its path
pointer. The margin was still spelled `FS_PATH_MAX` -- so when a path
became 4096 the reservation ate the whole 4096-byte page, every spawn
failed the size test, and the machine could not start `/bin/init`. It is
`ARGV_TAIL_MARGIN` (64) now, a number of its own, because a margin has to
be small relative to the page it is carved from and had only ever been
64 by coincidence. The general lesson is the one this project keeps
relearning about pointers to numbers: a constant borrowed to mean
something it does not name will be wrong the first time the thing it
names moves.

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
already-saved trapframe, and never touches `g_next_kernel_rsp`. An IRQ
handler that tried to switch directly to the woken process would be
re-creating exactly the reentrancy this design exists to avoid. A woken
process that OUTRANKS the running one is switched to on the way out of
the trap, by the same rotation the tick uses; any other waits for the
next tick. See "A wake preempts only from a better level" and "Within
a level, whoever has run least runs next".

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

## A window's pixels are a NAMELESS shared-memory object

**Superseded (stages 5a/5b, 2026-09-08):** a window buffer is now a
NAMED object the CLIENT creates (`WIN_BUF_NAME_FMT`, abi/win_proto.h),
safe because an object belongs to its creator unless granted
(`SHM_PUBLIC`, abi/syscall_abi.h); `shm_create_anon()` has no callers
left. The fragmentation argument below still holds.

`create_window()` took its frames from `pmm_alloc_contiguous()`. Nothing
needed them adjacent -- the client's mapping and the compositor's are
both built a page at a time -- and the demand was itself the failure
mode: a fragmented allocator refused a large window, silently, in a way
indistinguishable from a client declining to open one.

The one thing that wanted a linear kernel pointer was
`win_server_ops`'s `window_created(..., uint32_t *buf, ...)`, and that
is the ring-0 presentation layer, which nothing has registered since the
WM became a process. The argument dropped with it.

**Nameless rather than named, and that is the load-bearing half.** The
shm namespace has no permissions -- `SYS_SHM_OPEN`'s own comment says
"this system has no users, so any process may open any name" -- so a
window buffer with a name would be a way for any process to map anybody
else's window. `shm_lookup()` refuses an anonymous object, and
`shm_create_anon()` is the only way to make one.

What it buys beyond the fragmentation fix is a REFERENCE COUNT on a
window's frames, which is what the poison page exists to stand in for:
the compositor may still blit a slot whose frames the kernel has already
freed, so the slot is remapped to a shared read-only page rather than
left as a hole. A compositor that holds its own reference does not need
that, which is the same answer `wl_buffer.release` gives. See
`docs/winserver-ring3-design.md`'s stage 2.

## A length that is not page-aligned makes munmap fail SILENTLY

`SYS_MUNMAP` takes a page-aligned length and refuses anything else. A
window's buffer is `w * h * 4`, which almost never is one -- 320x200x4
is 62.5 pages -- so when window memory moved to the client, every unmap
of a replaced buffer was refused, the old mapping outlived its
replacement, and each resize leaked a buffer's worth of frames. Twelve
objects after three drags.

**What made it expensive is where it surfaced.** Not at the unmap, which
returned an error nobody read, but at the run's LARGEST allocation --
maximizing a window, the last check in `tools/uapp_test.py` -- because
that is where the leaked frames finally ran the guest out. Four
hypotheses were written and disproved before the leak was even
suspected: the kernel refusing (it was not), the extra syscall (it was
not), the two sides disagreeing about which buffer is the back one (they
were not), and the WM's lag heuristic (identical on both sides).

What found it was a COLUMN: `lswin`'s shm index, climbing on the branch
where it was reused on main. The general lesson is the one this repo
already states about probes -- an unread return value is not a
diagnostic, and the cheapest instrument is often the one that prints
state you can compare against a known-good build rather than one that
explains itself.

The paired mistake is worth keeping too: the fix rounded the length in
the allocator and not in the comparison that decides whether to
reallocate, so the sizes never matched and every present replaced the
buffer -- handing the app a freshly zeroed one each frame. Round in one
place or compare in the same units.

## The window protocol already had wl_buffer; it just never named it

Moving a window's pixels to the client raised what looked like a new
question -- how do you hand over a resized buffer while the compositor
is still reading the old one? -- and the answer is that this protocol
solved it years ago without giving the thing a name.

`struct win_buf` carries its own `w`/`h`, deliberately: "the dimensions
belong to the BUFFER rather than to the window". A resize rebuilds only
the BACK buffer and leaves the front holding the last finished frame at
its old size, so the compositor keeps showing real pixels for the whole
round trip instead of a freshly zeroed window -- measured at 100-240 ms
of black under TCG before that. Two slots, each with an identity and a
size, one of them named as current: that is `wl_buffer`, anonymous.

Client-owned memory makes it explicit. A buffer becomes a named shm
object; the NAME identifies the slot and a GENERATION identifies the
memory in it, so replacing one is an unlink plus a create under the same
name, and a present carries the generation the compositor should be
holding.

**The release semantics come free, and from a promise `shm_unlink`
already made**: an object somebody is still using survives its own
unlink, and its frames go at the last holder. So the compositor goes on
reading the buffer it has until it re-opens the name -- which is
`wl_buffer.release`, and the same mechanism that stopped a destroyed
window flashing black.

Rejected: allocating with headroom so an ordinary resize keeps one
object and only a growth past capacity swaps. It does not remove the
swap, it makes it RARE -- and a path taken once in a hundred resizes is
one that breaks without anybody noticing, which is what this repo's rule
about keeping fallbacks producible says from the other side.

## A named shm object belongs to its creator, and PRIVATE is the default

`SYS_SHM_OPEN` had no access control at all -- its own comment said so:
"this system has no users, so any process may open any name". That
reasoning conflates two things. No USERS does not mean no isolation
between PROCESSES, and `lsshm` lists every name, so guessing was not
even required: any process could open `snd.<pid>` and write into another
program's audio ring.

An object now records its creator, who may grant others by pid
(`SYS_SHM_GRANT`). `SHM_PUBLIC` at creation opts out, for a BEACON --
the rendezvous every client has to be able to find.

**PRIVATE IS THE DEFAULT, and that is the load-bearing choice.** A
permission model added later cannot make existing callers private
retroactively; the sharing has to be the thing that is asked for. The
cost was paid immediately and is the evidence it was doing something:
every existing caller had to declare itself, and the one that was missed
-- the clipboard page, where the comment was added and the flag was not
-- broke every app's clipboard AND the terminal's copy, which is one
regression wearing two symptoms.

**WHEN THIS SYSTEM GAINS USERS none of it is wasted.** `pub` is the
degenerate case of a MODE (public 0666, private 0600) and the creator
pid gains a uid beside it. The GRANT is the part POSIX has no equivalent
for: it is a capability naming ONE process, and it coexists with a mode
the way Linux has both file permissions and fd passing -- the mode is
the coarse policy, the grant the fine one.

The grant is by PID and a pid can be REUSED. The window is small -- a
grant lives only as long as the object, and an object dies with its
creator -- but a capability outliving its holder is what to look for if
this ever misbehaves.

## A futex is keyed on the FRAME, not on the caller's pointer

`SYS_FUTEX_WAIT`/`WAKE` sit straight on the scheduler's existing
address-keyed block -- "a blocked process waits on a channel, and a
channel is just an address" (`api/scheduler.h`) -- so the only real
decision was what address to use.

The caller's own pointer is the obvious answer and is wrong for the case
the futex exists to serve. Two processes sharing an shm page map it
wherever their own arenas happen to land, so a key built from the
pointer parks them on two unrelated channels and a wake reaches nobody.
The physical address is what they agree on, and the page walk already
computes it. Linux keys a shared futex on (inode, offset) for exactly
this; here the physical address IS that pair. The private case falls out
right at no cost, since two processes' private pages are different
frames.

A user frame's physical address cannot collide with the kernel objects
already used as channels: those live in the kernel image and the kernel
heap, whose frames are never handed to a user mapping.

**`scheduler_wake_n()` gained a count in the same change**, because a
futex's wake takes one and the reason is this scheduler's own: releasing
every waiter on a contended lock so that all but one parks again is the
thundering herd the per-object channel mechanism was built to avoid.

The compare-then-park is safe here only because a syscall handler runs
with interrupts off, so nothing observes the word in between. That is a
property of this kernel being single-core rather than of the design, and
the comment above `sys_futex_wait()` says so -- an SMP port needs a real
lock around the pair.

**What the tests could and could not show.** The cross-process round trip
is only testable from ring 3, and the first version of that test passed
with the kernel keying on the VIRTUAL address -- because both halves are
one binary and mapped the page at the same place, so the property under
test never reached the code. It maps a spacer first now and asserts the
two addresses differ before believing the wake. The KTEST beside
`futex_key()` covers the property directly, by mapping one frame twice
in one address space.

## init serves a channel AND keeps the file and the doorbell

`/bin/service`'s request was a line appended to `/run/init.ctl` plus a
`SIGHUP`, because neither half could do the job alone: a signal carries
no payload, and a file could not be noticed because init BLOCKS waiting
for children. A channel is both halves at once and can answer, so
`start` and `stop` travel over one now.

**The old path is kept, not replaced**, and used whenever no beacon is
published. This repo's rule is that an unreachable path is a guess
(`ata nodma`, `nopat`, TFS3 v1), so it stays producible: stopping init's
channel and re-running the same commands exercises it, which is how it
was tested.

**What made this possible is the wakeword's SECOND source.** init could
not serve requests and reap children at once: it parked in
`waitpid(-1)`, and a channel wakes nobody parked there. A child's death
bumps the wakeword now, from `notify_parent()` -- the one function both
kinds of death funnel through -- so a single wait covers both. That is
the generality the wakeword was built for, and building it is what
turned the claim into something exercised rather than asserted.

**The SIGHUP handler bumps the wakeword too**, and the reason is not
obvious: a signal wakes a parked process by REWINDING RIP over the
syscall, so the wait is RESTARTED rather than failed. A restarted futex
wait finds its word unchanged and parks again with the doorbell
unanswered. Moving the word is what makes the restarted wait return.
`waitpid` needed no such thing because it has an interruptible variant;
a futex wait does not.

## A channel API that copies a whole SLOT is a stack smash waiting

`uchan_server_recv()` copied `UCHAN_SLOT_BYTES` into whatever pointer it
was handed. Every caller passes a message STRUCT, and a protocol's
message is usually smaller than a slot -- `struct wmchan_msg` is 40
bytes against a 64-byte slot -- so every receive wrote 24 bytes past the
caller's buffer.

It surfaced as the compositor exiting with code 2 the instant a client
connected, which is `userland/rt/stack_chk.c`'s deliberate "caught by
the canary" code rather than any fault, so there was no crash report to
read. The two callers that shipped before it (`chan_test`, init's
control loop) had been overwriting other locals and getting away with
it.

Every entry point takes a LENGTH now, and a send zero-fills the rest of
the slot so a reader asking for more sees zeros rather than the previous
message. The general rule: an API that moves a fixed-size unit into a
caller's buffer must be told how big that buffer is, and "the slot size"
is not an answer the caller can check.

## The message channel is a LIBRARY, not a kernel object

`userland/lib/uchan.h` is a channel between two ring-3 processes and the
kernel knows nothing about it. It is named shared memory (`SYS_SHM_OPEN`)
for the pages, a futex to park on and a wakeword to be woken through --
three primitives that each exist for their own reasons, none of which
was added for this.

That is deliberate and it is Wayland's split: the transport is general
and the protocol on top is not. A kernel channel object was the
alternative -- Mach ports, Binder -- and it buys ordering and atomicity
for free at the price of the kernel copying every message and having an
opinion about what a message is. With a per-frame path in prospect, the
copy is the thing to avoid.

**A RING PER CLIENT, not one ring per service**, which is what makes it
lock-free: each ring has exactly one writer and one reader, so `head`
and `tail` are each written by a single process and neither side needs a
compare-and-swap. There is no CAS in this codebase to build the shared
ring on, and adding one to serve a second writer would be a worse trade
than a few pages per client. `/bin/soundd` reached the same arrangement
first, for the same reason.

The counters are FREE-RUNNING rather than indices, so `head - tail` is
the depth and full is never mistaken for empty -- an index pair only
manages that by wasting a slot.

**The bump before the wake is the ordering that matters.** A sender
increments the beacon word and then wakes it; a server that sampled the
word before the message and parks after it finds the value already moved
and does not park. A wake alone lands in that window and is lost. The
same rule appears three times now -- here, in `futex_note_ready()` and
in `SYS_FUTEX_WAIT`'s own -EAGAIN -- because it is the one thing a
wait/wake pair always gets wrong.

## One wakeword per process, because there is no poll()

A futex waits on ONE word. A compositor has two sources -- its event
queue and, once the channel exists, its clients' messages -- and no way
to wait on both: there is no `poll()` here, and neither the event queue
nor a channel is a file descriptor to build one over.

So instead of one wait over many objects, there is one WORD that many
sources bump. `SYS_WAKEWORD` names it; the kernel bumps it and wakes it
whenever it queues an event, and anything sharing the page does the
same. The waiter parks on that word and, on waking, looks at all of its
sources. That is the self-pipe trick, or eventfd -- what an event loop
without a unified poll turns into, and the reason both of those exist.

**The bump is what closes the race, not the wake.** A waiter that
sampled the word before an event and parks after it finds the value
already moved and does not park at all. A wake alone would be lost in
that window.

`poll()` over file descriptors is the conventional answer -- Linux's,
and what every Wayland compositor and the X server call. It is
deliberately not what this is, and the reason is cost rather than taste:
the event queue and the channel would both have to become file
descriptors first, which is a larger change than the thing it enables.
Worth revisiting if a third and fourth source appear, because that is
the point where scanning every source on every wake stops being free.

**It has no production caller yet**, which by this repo's own rule
leaves it unvalidated -- the same admission the TWP transport seam made
about itself, above. A KTEST proves the event path bumps it, and
reddens when that call is removed; the caller it exists for is the
compositor, which gains nothing until there is a channel to wait on
beside its queue.

## A dead window's pixels are kept alive by whoever is reading them

Destroying a window freed its frames at once, while the compositor is a
process that learns about it from a QUEUED event and may blit the slot
before it drains one. So the slot was remapped to a shared read-only
poison page -- not a hole, because a hole is a page fault in the
desktop. The cost was a black frame on every close.

The compositor's mapping holds a reference to the window's shm object
now (`WIN_REQ_UNMAP_WINDOW` releases it), so the frames outlive the
window and the compositor goes on reading the last picture it drew.
That is `wl_buffer.release`, and it is here for the same reason: a
compositor cannot be stopped mid-frame to be told a buffer is gone.

**The poison page did not retire with it**, and did later. It survived
because a window's address in the compositor was DERIVED from its
(pid, slot) and slots are recycled, so a client that closed and reopened
faster than the compositor drained its queue could find every slot still
held; the reclaim took one back and poisoned it. **Both went with the
derived address** -- see "The compositor opens a client's buffer by
name" below. Nothing can revoke a mapping the compositor made itself,
so there is no hole to fill and no slot to retire.

The trap, found by an existing test rather than by reading: the
recorded reference named WHICH object it was taken on, because a resize
replaces the object while the reference is still held on the old one.
Releasing `bufs[b].shm` would have put the NEW object and freed it under
a live mapping. It is not a hazard any more -- the compositor's mapping
IS its reference -- but it is the shape to expect from any bookkeeping
that mirrors an owner's state instead of holding it.
## The TWP transport seam never got a second implementation, and was deleted

Milestone 41's stage 3 added `struct win_transport` -- the same
one-struct-of-function-pointers registry as `display_driver` and the VFS
backend probe -- and it had exactly ONE implementation behind it for its
whole life, the `direct` one calling
`win_server_request()`/`win_server_debug()` in process. So nothing ever
proved the interface was not simply syscall-shaped. What follows is the
argument as it stood; how it resolved is at the end.

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

That scoping is still right for the stage it was written about, and it
does NOT generalise: taking the window server's memory half out of ring 0
needs a carriage that can carry a payload the 24-byte `struct win_event`
has no room for, and one that does not make a per-frame present wait for
the compositor. There the ring is the mechanism, not an optimisation.
`docs/winserver-ring3-design.md` carries that argument.

**HOW IT ACTUALLY ENDED (2026-09-09), which is the part worth keeping.**
The real second implementation arrived, and it did not plug in here. It
is `uchan`, a shared-memory ring in RING 3: a client's window requests
reach the compositor without entering the kernel at all, so there was
never a second `struct win_transport` to register. The seam was built to
let a carriage be swapped underneath `SYS_WIN_REQUEST`, and what happened
instead was that the traffic left the syscall. It was deleted in stage
6c with its one implementation inlined back into its two callers.

**The lesson is about WHERE an abstraction is placed, not whether to
build one.** The prediction that the seam might be "simply syscall-
shaped" was right, and understated: it was syscall-*sited*. An
indirection inside the thing being replaced cannot survive the
replacement, however well shaped it is. The seam that did survive is the
protocol itself -- `abi/win_proto.h`, typed messages -- which is exactly
what let the traffic move rings without a rewrite, and which is the half
this entry should have been about.

## Ring 0 parses no font: the console is baked, the desktop is fontd's

A `.ttf` is attacker-shaped data with a lot of offsets in it, and until
2026-09-09 it was parsed in ring 0 -- the surface Windows spent a decade
of GDI CVEs on before Windows 10 moved it to `fontdrvhost`. It is
`/bin/fontd` now, and the kernel image contains no TrueType parser at
all: `nm build/kernel.bin` finds no `ttf_*`, `font_atlas_*` or
`font_face_*` symbol.

**THE KERNEL IS NOT A CONSUMER EITHER, and that is the part worth
recording.** The first design had fontd hand its atlas back so the
kernel could go on serving glyphs to clients. The maintainer proposed
the simpler split instead: ring 0 draws its console, its shell and its
panic reports from the bitmap tables compiled into the image, and ring 3
gets TrueType. That deletes more -- the atlas cache, the contiguous
frame allocation and the request that mapped kernel glyphs into a client
all go with the parser -- and it removes a dependency the other design
keeps, because the kernel's glyphs then live in memory no ring-3 process
can free.

It is also the split Windows actually makes. Its bugcheck screen uses a
built-in font, not the font host.

**WHY THE BAKED TABLES CANNOT SIMPLY GO.** Three windows have no process
to ask: every line of the boot log before the scheduler exists, the
INIT_CONFIG stage where the setting is read, and a panic -- which must
draw with a process possibly already faulted. A console that cannot draw
until a service starts is a console that cannot report why that service
did not start.

**WHAT THIS COSTS, stated rather than glossed.** The text console and
the desktop render in DIFFERENT TYPEFACES: JetBrains Mono baked in
versus whatever face is selected. `fontface` no longer changes the
console, and `fontsize` there snaps to the five baked sizes because an
arbitrary size needs a rasteriser ring 0 no longer has. The desktop
still gets any size, because fontd rasterizes it.

**The BIOS font was considered and does not work here.** GRUB puts this
kernel in a linear framebuffer rather than VGA text mode, so there is no
hardware glyph rendering to borrow, and a UEFI boot has no VGA BIOS ROM
at all. The baked tables are the same idea done at build time, and
better for it: five sizes, anti-aliased, and six Latin-1 letters ASCII
does not have.

**What ring 0 kept is the SETTING, not the font.** `system.font_face`
lives in /etc/toyos.conf like every other one, and the registry needs a
list of valid choices -- so `font_faces.c` lists a directory and
validates a name against it. It opens no file. Whether a `.ttf` actually
parses is fontd's question, which is the honest limit of what ring 0 can
answer about a font it never reads.

## Diagnostics are a NAMED REGISTRY in the kernel, not a window-server relay

The `gui` command channel was built into `win_server.c` when the
compositor was the only ring-3 thing anybody wanted to interrogate. By
2026-09-09 it was not: `/bin/soundd`, `/bin/netd`, `/bin/clipboardd` and
init are all services with state worth reading, and a font daemon was
about to be a fifth. Every one of them would have needed its own relay.

So the endpoint became a NAME. `kernel/core/diag.c` holds a table of
providers, a service claims one (`DIAG_CLAIM`), and `diag <name> <cmd>`
at the serial console or `sys_diag()` from ring 3 reaches any of them.
The compositor is the provider `gui`, and `/bin/guictl` is its front end
rather than a special case. `win_server.c` lost 315 lines and the window
protocol lost four request types, two event types and a message struct.

**Why the table is the KERNEL's, when the obvious answer is a channel.**
`uchan` already carries `/bin/service`'s conversation with init, and a
diagnostic is the same shape. It cannot serve this one, because the
caller that matters most is the kernel's own serial debug console: ring
0 has no channel client, and reaching a wedged service when no shell is
available is the entire reason a kernel-side path exists. A channel
would serve the ring-3 half and leave the kernel half needing a second
mechanism, which is two paths doing one job.

**Why the wakeword rather than an event.** The old relay posted
`WIN_EV_CLIENT_DEBUG` to the compositor's event queue -- a door only a
windowing client has. `futex_note_ready()` bumps any process's wakeword,
so a service with no window is reachable identically. That queue was the
wrong door for a second reason recorded at the time: it is 32 deep and
sheds the OLDEST, so a client presenting every frame could flood the
diagnostic out of it, and the console timed out forever while the
desktop drew perfectly. The compositor therefore POLLS once per frame
instead, which cannot be starved and costs one refused request.

**What was NOT done, and why.** The relay was not moved to ring 3, and
the ring-3 half was not split onto a channel while the kernel kept its
own. Both were considered. The first cannot serve the serial console;
the second leaves two implementations of one conversation, which is the
shape this repo keeps deleting. Generalising the endpoint gets the
window-specific code out of ring 0 -- which was the actual goal -- while
leaving in the kernel the one capability only the kernel has.

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

## A setting the kernel does not apply is DECLARED BY A FILE, not registered in C

The settings registry (the entry below) was built when every setting was
the kernel's: the timezone, the font size, the keyboard layout. Then the
desktop moved to ring 3 and kept registering its settings in ring 0
anyway, because that was where the registry was. By 2026-09-20 twelve
kernel source files -- ~1070 lines -- existed to describe the wallpaper,
the taskbar height, the tray items, the screensaver, the window effects,
the cursor theme and the four keyboard shortcuts. Every one of them was
`apply = 0`: the kernel validated a string and wrote it to `/etc`, and
a ring-3 process noticed on its generation poll and did the actual work.

Those files are gone. A setting the kernel does not apply is now
DECLARED by a file in `/etc/settings.d` -- `Type=`, `File=`, `Label=`,
`Category=`, its choices or its bounds -- and `userland/lib/usetting.h`
merges those declarations with the kernel's registry so a client sees
one list. The kernel keeps the settings it can actually turn.

**The test is "does ring 0 do anything with the value?"** -- not "is it
a system setting". `system.font_size` stays registered because the
kernel rasterises glyphs with it. `desktop.wallpaper` does not, and
never did.

**Why a schema file and not a registration syscall.** Letting the
compositor publish its settings over `SYS_SETTING` was the obvious
alternative and is what Wayland does with globals. It fails on
LIFETIME: a setting would exist only while its owner ran, so `config
list` at a `text` target would show half a machine, `config set
desktop.wallpaper` would answer "no such setting", and a supervised
restart would re-register 25 rows into a registry that might still hold
the dead ones. A Wayland global is meaningless without its server; a
setting is not -- it is a value on a disk, and the question "what can be
configured on this machine?" has an answer whether or not the desktop is
up.

**Why not a settings daemon.** dconf has one, macOS has `cfprefsd`,
Android has SettingsProvider -- and all three exist because their store
is a private database. This store is `/etc`, which every process can
already read and write (`lib/uconf.h` compiles the kernel's own parser
into ring 3), so a daemon would arbitrate nothing and add a process that
must be running for `config get` to work. GSettings is the closer
parallel and the one followed: a schema is an installed FILE, schema
lookup happens IN THE CLIENT, and no code is written per key.

**What the kernel kept, and why it had to.** The generation counter.
Everything watching for a settings change polls `setting_generation()`,
and a library writing `/etc` directly bumps nothing -- so a wallpaper
would reach the disk and never reach the screen. `SETTING_OP_TOUCH`
announces a change the registry did not make. One counter for the
machine was the point: a second, ring-3 one would have to be polled
beside the first, and a consumer reading only one would silently stop
noticing half the settings.

**What it cost, stated rather than hidden.** The kernel can no longer
answer for a desktop setting -- a KTEST cannot reach one, and neither
can kernel code. Nothing did either before; if something ever needs to,
the answer is that it has become a kernel setting and should be
registered.

**What it bought beyond the deletion:** a setting is a file now, so
adding one is dropping a file in `/etc/settings.d` rather than editing
two kernel files and rebuilding, and `SETTING_MAX` stopped being a
ceiling the desktop competed for -- it had been raised four times, and
the last raise found the old value exactly full.

**The one exception is `system.shell`**, registered in ring 0 though
ring 0 never applies it. libc's `system()` reads it, and the C library
reaches settings only through `SYS_SETTING` -- `lib/usetting.h`'s merge
belongs to the toolkit, not to libc's "any C program" audience -- and
its `apply` refuses a path that does not exist, which no declaration key
can express. Declaring it would mean libc parsing `/etc` itself or
`system()` ignoring the setting (glibc's choice: always `/bin/sh`). The
network-time and `netheal` settings were the other stragglers; they
became declarations on 2026-10-09.

## A declared setting can require another declared setting, and nothing else

`Requires=<name>=<value>` in an `/etc/settings.d` declaration makes the
setting UNAVAILABLE while that other setting has another value: System
Settings greys it with `RequiresReason=` in place of its description, a
write is refused, and `Otherwise=` is what it reads as meanwhile.
Asked for so that "Button position: Left" could not be chosen under a
centred Start -- a combination Windows 11 cannot express either.

**Why the rule "a declaration cannot make a setting unavailable" did not
simply go.** It exists so a text file under `/etc` cannot disable a
control the KERNEL is willing to change. A requirement is only resolved
against another DECLARED setting (`uschema_unmet()` asks the schema, not
the registry, so a kernel name is never found): it links two knobs
`/etc` already owns, and reaches nothing of the kernel's.

**Why `Otherwise` rather than leaving the stored value in force.** Real
systems draw a dependent control greyed AT the value in effect; a greyed
"Left" beside centred buttons is a control telling a lie. And because
GET answers the effective value, the window manager needed no special
case -- it reads `center` like any other.

**Why the stored value survives.** Moving Start back to the left brings
back the alignment the person chose, as a dependent control does on
every desktop.

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
holding no registered setting at all (`/etc/timezones`, `/usr/share/kbs`,
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

## The wall clock is a software clock, and NTP steps it rather than slewing

Three decisions taken together when network time landed, all about the
same thing: which component gets to know what about the time.

**The wall clock is an epoch plus a monotonic delta, not a CMOS read.**
`rtc_read_local()` used to poll the RTC on every call -- spinning on
register 0x0A's update-in-progress flag -- so the taskbar's once-a-second
redraw paid for a hardware transaction, the clock could only ever move in
whole seconds, and there was nothing for a time client to correct. It is
now `api/ktime.h`: read the RTC once at boot, record the clocksource
reading taken at that instant, and answer every query as the difference
added to the epoch. That is Linux's timekeeping in miniature.

The obvious alternative was to keep polling the CMOS and have
`SYS_SETTIME` write it. That is smaller, and it was rejected because it
leaves the two real costs in place -- second granularity and a port
transaction per read -- to save state that is two `uint64_t`s.

**It is deliberately not a `struct clocksource`.** `clocksource.h`
already refuses a wall clock in as many words, and the reason survives
contact with this: a clocksource must be monotonic, and this one jumps
whenever it is set. Wall time is built ON a clocksource. Putting it
behind the same interface would make "how much time has passed" and
"what time is it" answerable by the same call, which is the confusion
that interface exists to prevent.

**`SYS_SETTIME` takes UTC while `SYS_GETTIME` answers in local civil
time, and they are not inverses.** That looks like an oversight and is
not. The kernel holds UTC (the RTC is read as UTC and `tz.h` applies the
configured city's offset only when handing out broken-down time), NTP
hands out UTC, and the filesystem's stored epochs are local-derived for
reasons this file's own entry on file timestamps sets out. Three
reckonings already existed; what was new was a caller needing the UTC
one, so `QUERY_CLOCK` exposes it as a fact rather than bending
`SYS_GETTIME`'s long-standing contract. The hazard is stated in the ABI
comment, in the ring-3 wrapper, and here: a client that reads
`SYS_GETTIME`, adds a second and passes it back moves the clock by the
timezone offset.

**`/bin/ntpd` steps, and it is SNTP rather than NTP.** Full NTP
disciplines the clock's RATE against several servers and never lets time
run backwards. `ktime` has no rate to adjust -- it is an epoch and a
delta -- so slewing would mean a second kernel feature (a scaled
clocksource conversion, a servo, and a way to reason about a clock that
is deliberately running wrong) before a single packet could be useful.
Stepping is what SNTP specifies and what `systemd-timesyncd` does on a
first sync, and the blast radius here is small by construction: anything
measuring an interval uses `SYS_MONOTONIC_NS`, which a step cannot move.
The honest cost is that a wall-clock deadline can be jumped over, and
nothing in this tree holds one.

**And the client is a ring-3 program, as DHCP is.** Which server to
trust, how long to wait, how often to ask and what to do when nobody
answers are policy. The kernel exposes one verb, `SYS_SETTIME`, and
holds three settings it never acts on. The service is started always and
gated by `system.ntp`, which ships off -- one switch rather than two, so
turning network time on in System Settings needs no service enabled as
well. `telnetd` and `tftpd` ship disabled instead because they have no
setting to gate them and listen the moment they run.

## A string setting is editable in System Settings, and the widget grew two things to allow it

`SETTING_TYPE_STRING` rendered as an EMPTY `uui_radio_list` -- a row that
looks broken, with `config set` as the only way to change it. The app's
own comment said so and treated it as the design. It was not; it was the
absence of a control kind.

**The alternative considered was making the setting that prompted it an
ENUM** of a few known hosts, which needs no toolkit change at all. It was
rejected because it solves one setting and leaves the gap: the registry
has a free-text type, System Settings claims to show the whole registry,
and any future string setting would hit the same wall.

**The interesting part is `struct slot`'s `staged`/`baseline`, which are
ints** and carry an index for a choice control and the number itself for
a spinbox -- a deliberate overload, because everything the page does with
them is an equality test. A text field has no index, so it keeps
`baseline` at 0 and sets `staged` to 1 when the field differs from the
value the page opened with. Every existing `staged != baseline` test
reads unchanged, and only `staged_value()` knows there is a third case.
Two more fields would have meant a branch in each of those tests.

Two supporting changes were forced. `UUI_TEXTBOX_MAX` went from 48 to 64
so a field can hold a whole `SETTING_ABI_VALUE_MAX` value, with a
`_Static_assert` tying them -- a shorter field truncates silently, which
is a wrong answer rather than a full one. And `uui_textbox` gained a
`disabled` flag, which every other control here already had: a setting
the registry has made unavailable must read as unavailable, and the
sentence beside it is the whole value of disabling a control.

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
is 100Hz today. The server first rounded to whole ticks and floored at
one; since the kernel's timers became one-shot deadlines it keeps the
interval in nanoseconds, so 16 ms is 16 ms (`wm_client.c`'s
`on_window_timer()`). Zero is still never an interval: `uapp_set_tick()`
refuses it, because zero would fire every frame and turn a request to
slow down into the busiest possible loop.

**A deadline, not a queue.** The next firing is computed from NOW, not
by adding the interval to the previous deadline. Those differ only when
a client is slower than its own timer, and the second form silently
accumulates overdue firings that all arrive at once when it catches up
-- the opposite of what a client asking for less frequent wake-ups
wanted. Same reasoning as the event queue dropping the oldest.

**One timer per window.** A client wanting several derives them from
one short interval, exactly as an app does on top of a frame clock. A
general timer service is a bigger feature than anything here needs.

**Polling stayed as the fallback -- until 2026-10-10.** `tick_ms` of 0,
or a server that declined the request, left the old loop in place, which
kept this additive. That made the timer a REQUIREMENT: an app that
forgot `tick_ms` woke 100 times a second. Now 0 means a 33 ms timer
(`UAPP_TICK_DEFAULT_MS`, what most screensavers chose), a declined
timer is kept by `uapp` in its blocking wait, and the polling loop is
`UAPP_POLL`, asked for by name -- by DOOM, which keeps its own 35 Hz
clock inside `on_tick`. Win32 and SDL draw the same line: a timer by
default, a `PeekMessage` loop when a game asks for one.

This also fixed `COARSE_HZ` being a bare literal at the `pit_init()` call
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
freestanding (`<stdint.h>` only, no allocator, no drawing). **Both
callers ended up in ring 3** (the desktop moved there), so it lives in
`userland/lib/` now, with `icon_grid.c`: shared source is for code BOTH
rings call, and ring 0 kept no caller.

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

`/usr/wm/applications/` feeds BOTH the desktop icons and the Start menu. Asked
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
looked fatal at first: every other request was refused outright when no
presentation layer was registered, in TWO places -- the syscall's own
gate and `win_server_request()`'s. A compositor could therefore never
register before the WM did.
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
with `-mcmodel=kernel` and a ring-3 ELF with a user code model
(`-mcmodel=large` then, `-fpie -mcmodel=small` since dynlink Stage 1),
linking at `VMM_USER_BASE`. The objects are not interchangeable, so rebuilding
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

**SINCE 2026-10-07 THE ENGINE IS RING 3'S ALONE** (`userland/calc/`,
linked through `EXTRA_OBJS_calculator`). The kernel Calculator it was
shared with went on 2026-08-18, and the kernel had gone on compiling an
engine nothing in it called; `-Iapps` went with it. The rule above
still governs what stays on the shared path -- `geom.c`, `string.c`,
`knum.c`.

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

`userland/include/string.h` and `userland/include/stdio.h` declare `strlen`,
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
bites rather than only here. `userland/include/string.h` cannot include
`"string.h"`, because a quoted include searches the including file's own
directory first and that resolves to itself; the guard makes it a silent
no-op and every `k_*` is then undeclared. It used `<string.h>` to skip
the current directory — which stopped working the day
`userland/include/` went on the path AHEAD of `kernel/include/api/`, so
that BOTH spellings came back to itself. It says `<kstring.h>` now; see
the include-root entry below. The implementation file cannot be called
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

**`/bin/tosh` exists now** (`userland/bin/tosh.c`), and is the thin
`main()` `tosh.h` has promised since it was written: `tosh_init()` plus
a read loop. The shell itself is still a LIBRARY, because its other
caller is a GUI terminal that owns its own event loop and cannot sit
blocked in a `read()`.

What it was waiting for was an interactive stdin, which is why this
entry used to say the binary did not exist rather than that nobody had
written it. That is [fd 0](#fd-0-is-the-console-it-blocks-and-the-first-ring-3-reader-takes-the-keyboard).

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
vs the user model -- `-fpie -mcmodel=small` today), but the SOURCE can, and a second hand-written copy
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

## stderr is the terminal's, and the kernel log only without one

A process's fd 2 is wherever its terminal is -- the same place as fd 0
and 1 -- when it has one, and the kernel log when it does not. That is
Linux with systemd: a shell on a tty hands the tty to its children as
0/1/2, and a service's stderr goes to the journal because it has no
terminal. Windows splits the same way (a console process inherits the
console's handles; a service has none and logs to the event log).

Concretely:

- **A fresh descriptor table puts the console on all three**
  (`fd_space_open()`), and so does the kernel shell's FOREGROUND start
  (`scheduler_spawn_attached()`, what a bare command name at the
  physical or debug console runs). Errors reach whoever typed the
  command.
- **A DETACHED kernel-side spawn gets the kernel log on fd 2**
  (`scheduler_spawn()` and friends): init, the shell's `spawn`, gui3's
  compositor, the ktests' children. Nobody waits on the terminal for
  them, so an error there would land on whatever the console shows by
  then -- `systemd-run` sends a transient unit to the journal for the
  same reason.
- **init gives every service the kernel log on fd 2**, and the
  compositor passes it on to every desktop app it launches. A
  descriptor chooses with systemd's `StandardError=`: `kmsg` (the
  default, named explicitly as `SPAWN_FD_KMSG` rather than inherited
  from init) or `inherit`, meaning *the same as stdout* -- which is how
  the console shell's errors reach the console it runs on.
- **SYS_SPAWN can name the child's fd 2** (`SPAWN_STDERR` and
  `stderr_fd`), for the reason `stdout_fd` exists: a spawn ABI has no
  child-side window to dup2 in. Two sentinels: `SPAWN_FD_LOG` (the
  application log) and `SPAWN_FD_KMSG` (the kernel log).
- **`/bin/spawn` is the detached start from ring 3**, and names
  `SPAWN_FD_KMSG` -- `nohup` taking a background job's output off the
  terminal. The prompt is back before the child has said anything. It
  is also what every harness tool's `sh spawn` goes through, and a
  background test's verdict has to be readable afterwards.

**THE DIFFERENCE FROM systemd IS THE DEFAULT.** systemd's
`StandardError=` defaults to `inherit` (the journal, alongside stdout);
here it is `kmsg`. A windowed app has no terminal and inherits the
compositor's fd 2, and ~40 GUI test tools read those apps' lines
(`uidemo: press`) out of the kernel log -- the channel that exists
regardless of who spawned the process. Moving that is a change to the
harness's oracle, not to stderr.

**WHAT CAME BEFORE, AND WHY IT WAS WRONG.** fd 2 was the kernel log for
EVERY process, set once in `fd_space_open()`. It solved a real problem
-- a GUI client had nowhere to say anything a test could read, which is
how `tools/gfxdemo_test.py` found it -- but solved it for everyone: a
program run from a shell printed its errors into `dmesg`, invisible to
the person who ran it, and the `/bin` commands answered by writing
their errors to STDOUT instead (`userland/lib/cmd.h`). The debug console
then made it visible: after COM1 and COM2 were split, `kfmt_test`,
`memtest` and `malloc_test` reported their verdicts on the log port
while the harness read the console port.

**NOT DONE, deliberately: `cmd.h` still writes to stdout.** Its
precondition -- a terminal that can see fd 2 -- now holds for a
Terminal window, telnet, the console shell and the kernel shell's
foreground. But ~70 programs include it, services among them, and a
service's failure would move from `log -u <name>` (its stdout) to the
kernel log (its stderr). That wants its own change, with services
choosing `StandardError=inherit` where the application log is the right
home.

The rule for app code is unchanged: `sys_print()` for output,
`sys_eprint()` for anything diagnostic.

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

## The `rammeter` overlay was REMOVED, and what it was for still matters

It drew a live physical-frame and kernel-heap readout in the top-right
corner, once a second, behind a `rammeter` GRUB flag. Deleted 2026-08-20
at the maintainer's request.

**Why it was built:** those are the two pools that actually run out on
this machine, and they fail in unrelated ways -- the frame allocator is
what a leaking compositor or an unreaped process exhausts (window
buffers are contiguous frames), while the heap is what fragments under
kmalloc churn. A meter showing one would routinely point at the wrong
subsystem.

**Why it went:** it only ever ticked from `wm_render_frame()`, so it
appeared on the desktop and nowhere else -- never at the physical
console, which has no repaint loop to hang it off. The two obvious hooks
were both worse than the gap: drawing from the PIT IRQ can interleave
with a compositor mid-blit, and hooking `keyboard_getchar()`'s wait
would have a driver calling into gfx. Then the desktop became a ring-3
process, and an overlay painted straight at the framebuffer by the
KERNEL became a second writer to a surface the compositor believes it
owns. A debug instrument that works in one of the two places you want it
and fights the compositor in that one is not worth the seam.

**What went with it:** `gfx_overlay_fill/char/string` and their
`raw_put()` helper, which existed only for this. They bypassed the back
buffer, the clip rect AND the dirty-rect box, which is exactly what
`gui damage verify on` correctly calls a violation -- so leaving an
unused API whose whole purpose is to evade the damage system would have
been a footgun with no user.

**If it is ever wanted again:** it should be a normal widget the ring-3
compositor draws, with its damage declared like anything else, reading
the numbers through `SYS_QUERY` -- not a second hand reaching into the
framebuffer. Task Manager is the natural home. `git log` has the
original.

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

Measured with `gfxbench 20` under `make run KVM=1`, same build, the only
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
and every test in this repo. `make run KVM=1` honours them, which is what
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
`live-iso` is already a separate artifact), or a runtime
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
and `sched_fork.c`'s spawn path, `elf_run.c`'s legacy loader,
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
by putting one raw dereference back into `SYS_WIN_CREATE` (since
deleted) -- ring-0
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
worth keeping**: every heap pointer is a canonical address with bits
63:48 clear -- the kernel map ends at 512 GiB and the ring-3 map sits
just above it -- while the magic's top 16 bits are `0xC0DE`, so no
`prev` can ever collide with it. (It used to say "fits in 32 bits",
which was true and narrower than the property; the heap lives above
4 GiB on a big machine now.) Break either fact -- a pointer with bits
63:48 set, or a magic whose top 16 bits are zero -- and the two cases
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
spinning until `coarse_ticks()` changes, and the PIT is not initialised
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

**Where the rows live now (2026-10-09):** `kernel/include/abi/syscall_rows.h`,
an X-macro list `syscall_table.c` expands with each handler and
`/bin/strace`'s decoder (`userland/lib/utrace.c`) expands without it --
still ONE row per syscall, now readable by both rings, because decoding
moved to ring 3 (`docs/trace-design.md`). Everything below about one
merged row still holds.

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
`kernel/net/net_syscalls.c`, `kernel/fs/fs_syscalls.c`, `kernel/proc/proc_syscalls.c`,
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
the C names through `userland/include/stdlib.h`.

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

## The boot target is a setting, and the boot flag overrides the LIVE value rather than the file

`system.default_target` (`kernel/lib/target.c`) decides what init
starts. Two things about it were real forks.

**Why a registered setting rather than a word on the kernel command
line.** The command line is where a target override belongs, but not
where the target itself belongs: a machine's purpose survives reboots,
and a flag does not. systemd puts the persistent answer in
`default.target` on disk and lets `systemd.unit=` override it for one
boot; Windows keeps it in the BCD. The setting registry already gives a
persisted value a Control Panel row, a `config` entry and a namespace
for free, so declaring it there cost one `struct setting` and no edit to
anything that displays settings.

**Why the override is not written to the file.** The obvious
implementation of `target=text` is "set the setting" -- and that would
mean booting once with an override silently reconfigures the machine,
so the escape hatch you reached for because the desktop was faulting
also removes your desktop permanently. Instead `target_init()` reads the
file, then applies the command-line word over the top of the in-memory
value only. The registry already models exactly this state: a setting's
live value and its stored value are separate fields in
`struct setting_msg`, `config diff` reports the pair, so an overridden
boot is *visible* rather than mysterious.

The cost, which is real and is why this is written down: `config reload`
re-reads every setting from its file and therefore DISCARDS the
override. That is an explicit user action rather than a surprise, and
the alternative -- teaching reload which values are pinned -- means a
second notion of where a setting's value comes from, which is the thing
the registry exists to prevent.

**The matching rule is stricter than every other boot word here.** The
others are plain substring tests. `target=` cannot be, because
`default_target=` -- the key's own name, and therefore exactly what
somebody will eventually type on the GRUB line -- CONTAINS it. So the
match must begin the line or follow a space. A boot word that silently
matches a longer word is only ever found by the person it bites.

## A service is a FILE, and the give-up matters more than the restart

init reads `/etc/services.d/<name>` -- `Name`/`Exec`/`Target`/`Restart`,
the same `name=value` parser every other config file here uses -- and
starts the ones whose target matches. The alternative was a compiled-in
list of one, and this is the same call the Start menu already made when
it stopped being a C table and became `/usr/wm/applications`: adding a
service is dropping a file.

What is deliberately NOT copied from systemd is everything that needs a
dependency graph -- `After=`, `Wants=`, `Requires=`, socket activation.
There are two services' worth of ordering requirements here (none), and
a unit graph with no edges is a data structure pretending to be a
design. Ordering arrives when a second service actually needs it.

**The give-up is the part worth defending.** `Restart=always` with no
limit turns a binary that faults at its entry point into a machine that
spins forever starting it -- and on a `graphical` target there is no
console left to fix it from, because the desktop owns the screen. So a
service that keeps dying QUICKLY (five consecutive failures inside two
seconds each) is declared a crash loop and left down with a line saying
so, which is systemd's `StartLimitBurst`. The backoff between attempts
doubles from 0 ms to a 2 s cap, so the first restart of a healthy
service the user just killed is immediate and only actual failure pays
the wait.

The threshold is on how long the service RAN, not on how often it has
been restarted. That distinction is what lets a desktop be killed by
hand twenty times without ever being given up on, while a binary that
cannot start at all is abandoned in four seconds.

**A CLEAN EXIT IS A REQUEST TO STOP.** `Restart=on-failure` is the
default: a service that returned 0 stays down, one that crashed or was
killed comes back. `always` restarts either way, `no` never does. This
was not the first design -- `always` was, and it broke a real feature:
the Start menu's *Exit to shell* makes the desktop return 0, so init put
it straight back and the menu item silently did nothing.

Worth recording is WHY the wrong option was chosen. When the two policies
were offered, the note against on-failure said "a kill is exit code 0-ish
here, so `kill` would still restart it" -- which is simply false; a killed
process reports -1. A wrong factual aside made the right option look
useless, and it was only caught by opening the Start menu afterwards.
Check the claim you attach to an option, not just the options.

**Removing a descriptor DISABLES the service rather than stopping it**,
and init notices without a reboot -- it rescans when `fs_generation()`
moves, the same free poll the desktop uses for `.desktop` files. That is
systemd's `disable`/`stop` split, and it is the right way round: killing
a running process because somebody edited a file in `/etc` is a
surprise, while declining to restart it is what removing it means.

**The rescan happens when init WAKES, not immediately** -- it blocks in
`waitpid(-1)` between passes, so a change is seen the next time a child
exits (or within 250 ms with no children). A periodic rescan would mean
init polling forever on an idle machine, which is what `SYS_SLEEP` exists
to avoid. It lands the right way round for the case that matters:
removing a descriptor and then killing the service works, because the
kill IS the wake-up.

This was NOT designed in advance -- it was forced by two test tools, and
the way it was forced is the interesting part. `screen_surface_test.py`
and `compositor_death_test.py` both have to be the only compositor on
the machine, so both kill the desktop first. Under supervision that
stopped working: init restarted it with a zero backoff and the new
desktop claimed the role straight back, so one tool measured a role that
was never free and the other lost the role mid-run and never reached its
own release step. Both had been passing for months.

The lesson generalises past this repo: **making something automatic
removes an interlock somebody was relying on.** `gui` blocking the shell
for the desktop's whole lifetime was never a feature, but two tools and
the physical console's keyboard all depended on it. When you automate a
lifecycle, look for what was previously guaranteed by the manual step.

And the fix's shape is worth copying: the tools now ESTABLISH their
precondition (`rm /etc/services.d/toywm`, then kill) and keep their
original assertions, rather than weakening the assertions to tolerate the
new behaviour. The first attempt did the latter -- replacing "the role is
released" with a check on a log line -- and it was strictly worse
evidence for the same property.

## The keyboard's blocking readers are suspended while a compositor holds the role

Once init started the desktop at boot, the physical shell was left
sitting at a prompt BEHIND it -- and both were draining the same
keyboard ring. Whichever polled first won, so a key typed at the
desktop could be executed by an invisible shell. This was measured, not
predicted: `ps` typed over QMP into a running desktop ran the program.

It could not happen before, and the reason is worth keeping: `gui`
blocked the shell inside its own spawn-and-wait loop for as long as the
desktop lived, so the shell was never a competing reader. Making the
desktop init's child removed that accidental interlock.

The fix is one flag (`keyboard_suspend_blocking()`), set from the single
place the compositor role changes -- so registering, deregistering, a
kill and a fault are the same path, the argument `win_surface_revoke()`
already makes there. While it is set, ring 0's BLOCKING readers idle
instead of returning a key; the NON-blocking `keyboard_try_getchar*` are
untouched, which is what keeps `win_input.c` feeding the compositor.

**Why not the obvious alternative** -- don't start the shell at all on a
graphical boot. Because the desktop can die, and something has to be
there when it does. Suspending a live shell means the console comes back
usable the instant the compositor role is dropped, with no second
decision about when to start one.

This is NOT real console ownership and is labelled as such in
`keyboard.h`. A per-TTY input queue with a foreground process is the TTY
milestone's job. What it buys today is that exactly one thing reads the
keyboard at a time, which is the property that was actually broken.

The trap it introduces, recorded because nothing warns you: a compositor
that registers and then never draws leaves a console that is both blank
and deaf, which looks exactly like a hung machine. `target=text` on the
GRUB line is the way back, and the role being dropped on death is what
makes that rare.

**AND SUSPENDING THE READ WAS NOT ENOUGH, which is the half worth
remembering.** The first version put the check in the wait loop's `while`
condition only. That correctly stopped the reader consuming keys -- and
left the loop BODY running every iteration, where it calls
`vga_cursor_tick()` and `vga_present()`. Those are console upkeep, and
they publish into the framebuffer the compositor owns, so the shell went
on blinking a text cursor on top of the desktop at whatever cell its
prompt had left it. It was reported by the user from a screenshot: a
blinking block sitting on the Control Panel icon, with all 23 GUI tools
green.

`scheduler_idle()` had the right rule written down the whole time -- it
deliberately excludes the cursor tick and the present, because "console
upkeep belongs to whoever owns the screen, and the desktop owns it while
it is up". The bug was a second place that had to obey the same rule and
did not. `scheduler_idle()` itself stays unguarded in that loop, because
it is the one thing there that is not the console's: a suspended shell
must still drain the debug console and feed raw input to the compositor.

Two general lessons, and the second cost more than the first. **When you
suspend a loop, ask what its BODY does, not only what its exit condition
is.** And **a suite that only ever drives the system cannot see something
that happens when nobody touches it** -- every GUI tool here interacts and
then asserts on what changed, so a screen that paints itself while idle
was structurally invisible to all of them. `tools/idle_desktop_test.py`
asks that question now, with the taskbar clock as its own control.

## fd 0 is the console, it BLOCKS, and the first ring-3 reader takes the keyboard

`sys_read(0, ...)` reads the physical console keyboard and parks the
caller when nothing is typed (`kernel/tty/tty_fd.c`'s
`console_read()`). It exists because a ring-3 shell had nowhere
to read a line from: `SYS_READ_KEY` is non-blocking by hard requirement,
so `/bin/tosh` would have had to spin-poll the keyboard for its whole
idle life. That is why this file used to say there was no `/bin/tosh`
yet -- not that nobody had written the `main()`.

**Blocking is safe now, and the old autopsy is still correct about the
thing it describes.** The version that was tried and abandoned turned
interrupts on inside the syscall handler and sat in a `hlt` loop, so a
keyboard IRQ landed with the handler still on the stack and clobbered
`g_next_kernel_rsp` -- one keystroke, then a hang. Nothing here reopens
that: the handler does not WAIT, it PARKS and returns, through
`scheduler_block_current()`, exactly as the pipe, `waitpid` and `sleep`
paths already do. The wake site is the terminal's `tty_enqueue()` (then `keyboard.c`'s `ring_push()`), in the
IRQ, where all it may do is flip scheduler state and write an
already-saved trapframe -- which is all `scheduler_wake()` does.

The wake value is `SYS_RETRY`, not the key. Three consumers drain that
one ring (the ring-0 reader, `win_input.c`, and now this), so a woken
reader may find the key already gone; `SYS_RETRY` makes that "park
again" instead of a spurious answer, and libsys's `sys_read()` already
loops on it.

**Why fd 0 rather than a `SYS_READ_KEY_BLOCK`.** It is the Unix shape,
it costs no new syscall number, libsys needed no change at all, and it
is the same fd a later `dup2` and `SYS_SPAWN` stdin redirection have to
plumb anyway. A second key-reading syscall would have had to be
deprecated the moment either landed.

**It never returns 0.** A console has no end of file, and 0 is EOF for
every other reader of `SYS_READ` -- returning it would tell a shell its
input had closed, which is the same class of mistake as the `SYS_RETRY`
sentinel itself.

**One byte per key, exactly the driver's code.** Every value
`keyboard_try_getchar()` produces fits in a byte, `KEY_ARROW_*` and the
Nordic characters included, so no encoding decision had to be made and
none was. There is no echo, no editing and no escape-sequence
translation: all three are a line discipline, which belongs above a real
TTY where it can be turned off, and a program with its own editor wants
them off. The reader echoes what it reads.

**The first fd-0 read CLAIMS the console** (`keyboard_claim_console()`),
and the kernel shell stands down until that process dies. There is one
keyboard and no TTY layer, so the alternative is two readers splitting a
typed line between them -- which is not a thought experiment: removing
the claim as a positive control left `claimpobe.txt` AND
`claimprobe.txt` on disk from one typed `touch /claimprobe.txt`, the two
shells having taken alternate characters.

The claim is a SECOND flag beside the compositor's, not the same one.
Both can hold at once, and with one boolean whichever released second
would hand the keyboard back while the other still owned it -- which is
the exact bug `keyboard_suspend_blocking()` was added to fix. So:
two setters, one predicate (`keyboard_blocking_suspended()`), and the
ring-3 reader tests `keyboard_compositor_owns()` instead, because
testing the combined predicate would deadlock it against its own claim.

**A desktop owning the screen owns the keyboard with it.** A console
read while a compositor is registered parks without popping anything and
without claiming, because `win_input.c` drains the same ring to feed the
compositor -- taking a key there would make keystrokes vanish from the
desktop at random.

**The claim is released from `fd_release_all()`**, the per-address-space
teardown hook, which both exit and kill reach. A shell that crashes
therefore gives the console back on its own; tracking the release in the
reader would have made a crash a machine you cannot type at.

Whoever waits for a key also FLUSHES the screen: the console draws into
a back buffer and the ring-0 reader's idle loop is what normally
presents it. That loop is suspended on this reader's behalf, so
`console_read()` presents once before parking -- the same
"output is finished, we are waiting for a human" moment.

None of this is console ownership done properly. A per-TTY input queue
with a foreground process is the TTY milestone's job; what this buys is
that exactly one thing reads the keyboard at a time, which is the same
thing the compositor flag buys and for the same reason.

## File descriptors are two levels, and fds 0/1/2 are ordinary entries

A descriptor table used to be a single global array of eight entries,
each tagged with its owner's CR3, with fds numbered from 3. fds 0, 1 and
2 were not in it at all: they were numbers matched in an `if` inside
`sys_read`/`sys_write`, and "this process's stdout is redirected" was a
single `stdout_pipe` field on `struct sched_process`, set once at spawn.

That shape cannot express redirection. `dup2(pipe, 1)` had nowhere to
record itself, so `>` and `<` were impossible and a pipeline could only
ever be the one hardcoded case the spawn argument covered.

**The split, which is POSIX's.** A DESCRIPTION is what a stream is -- a
file, a pipe end, the console, the kernel log -- and is refcounted. A
DESCRIPTOR is a number one address space uses to name a description.
`dup2` copies the NAME; the description dies with the last descriptor
naming it. Routing then switches on the description's KIND rather than
on the fd number, which is what makes 0/1/2 unremarkable: they merely
start out pointing at the console -- or, for a detached process, fd 2
at the kernel log.

**Keyed by CR3, not by pid.** The legacy blocking loader (`run` at the
physical shell) has its own address space and NO scheduler slot, so a
pid-keyed table would leave it with no descriptors at all -- the trap
`SYS_SBRK` already documents. This was not theoretical: the first
version of the spawn path read the parent from
`procs[current_index].pml4_phys`, and children of a `run` silently
inherited nothing. `vmm_current_pml4()` is the identifier every path
has.

**CONSOLE and KLOG are separate kinds** because a process with no
terminal still needs somewhere for its diagnostics that a test can read
-- "stderr is the terminal's, and the kernel log only without one" has
who gets which. Being a description rather than a test on the number is
what lets a process redirect stdout without dragging its diagnostics
along.

**Inheritance is what removes the need for `fork()`.** A spawned child
copies its parent's whole descriptor table, sharing every description.
So a shell redirects ITSELF around the spawn --

    saved = dup(1); dup2(f, 1); spawn(...); dup2(saved, 1); close(saved);

-- and the child needs no cooperation and runs no code of its own. That
is precisely why `posix_spawn()` exists: fork's real purpose is to give
the child a moment to call `dup2` before `exec`, and a system without
cheap copy-on-write should not pay for a whole address-space
duplication to get it. Windows does the same thing with
`STARTUPINFO`'s handle inheritance. `SYS_SPAWN`'s existing stdout
argument survives as the one-call shortcut, applied after inheriting.

This also DELETED the `stdout_pipe` field and its two teardown sites: a
dying process's descriptors are unref'd by `fd_release_all()`, and the
last reference to a pipe write end is what turns a blocked reader's wait
into EOF -- one path instead of three.

**The counting trap it exposed.** `SYS_SPAWN` used to call
`pipe_add_writer()` for the child. With inheritance the child taking a
reference to the DESCRIPTION already makes it a second writer, so doing
both counted it twice and the pipe never reached EOF -- the reader hung
forever on a child that had exited. When a refcount moves down a layer,
delete the old one rather than keeping both.

## A full pipe blocks its writer, and a spawned child inherits only 0/1/2

Two decisions from building `|`, and both were forced by bugs the
pipeline made unavoidable rather than merely likely.

**`pipe_write()` is all-or-nothing and parks the writer.** It used to
take what fitted and report a short count -- a correct-looking answer
that nothing in ring 3 acts on, because no program here loops on a short
write. A producer faster than its reader therefore lost the remainder
SILENTLY. That was latent while the only reader was a shell draining
continuously; `|` makes the reader another process that may not have
been scheduled yet, so the pipe fills every time.

Atomicity is affordable rather than aspirational: `pipe_fd_write()`
CLAMPS a pipe write to `PIPE_BUF_SIZE`, so one write always fits once
the pipe drains and a parked writer can never be waiting on a request
too large to satisfy. POSIX guarantees the same for writes up to
`PIPE_BUF`, for the same reason. **The clamp was free when this was
written and is not any more** -- `SYS_WRITE_MAX` was then 1024 against a
4096-byte buffer, so the property held with nothing enforcing it;
raising the cap would have parked writers on requests the pipe could
never satisfy. `api/pipe.h` carries the note.
The retry lives in libsys, which already loops on `SYS_RETRY` for reads;
re-sending the whole buffer is only correct BECAUSE the write was
all-or-nothing, since a partial write would duplicate those bytes.

**Check-and-park had to become atomic.** The kernel is preemptible, so
"the pipe is empty -> park" can be split by the other end, whose wake
then fires with nobody parked and is lost. That was survivable while
only readers parked -- the next write or the EOF woke them. With both
ends able to sleep it is a deadlock, and it was observed as one. Both
pipe paths hold `scheduler_preempt_disable()` across the check and the
park now.

**A child inherits ONLY fds 0, 1 and 2.** The first version copied the
parent's whole descriptor table, which is what Unix does -- and Unix
gets away with it because the shell runs code IN the child, between
fork and exec, to close the pipe ends it must not keep, or marks them
close-on-exec. There is no fork here and no `CLOEXEC`, so inheriting
everything hands each child every pipe end the shell happens to hold.
A pipeline then never sees EOF, because the reading stage is itself a
writer of the pipe it is reading: two processes blocked forever, one
waiting for data and holding the write end that would have ended it.

0/1/2 is also exactly what `posix_spawn()`'s default and Windows'
`STARTUPINFO` pass, and for the same reason -- they are the streams a
child is meant to be given; everything else is the parent's private
business. If a future `fork()` arrives it will copy the whole table, as
it should, because there the child can close what it does not want.

**The shell-side ordering that follows**, recorded because each is a
deadlock rather than a preference. Builtins run LAST, after every
external stage is spawned and draining: a builtin runs inside the shell
synchronously, and one producing more than 4 KiB before its reader
existed would block the shell against a stage it had not spawned yet.
The last stage is drained AFTER the builtins (draining first blocks on
output from a pipeline whose first stage has not run) and BEFORE the
waits (waiting first blocks on a stage that is itself blocked writing
into a capture pipe nobody is emptying).

## A failed syscall returns -ERRNO, and the global lives in ring 3

Every syscall reported failure as `-1` and wrote the reason to the
kernel log -- 58 such sites, 23 distinct reasons already spelled out as
English sentences that only a person reading `dmesg` could act on. The
concrete cost: `/bin/tosh` probes each PATH candidate with `open()` and
read any `-1` as "not there", so a machine out of descriptors reported
"command not found" for a program that was sitting right where it
looked.

**Why a returned code and not a global `errno`.** A global needs
thread-local storage the moment a process can have two threads inside a
syscall at once, and toy-os has no TLS. A returned code needs nothing.
This is Linux's split exactly: the kernel returns `-errno` and the C
library -- not the kernel -- turns that into `-1` plus a per-thread
variable. NT does the same thing one layer differently, returning an
`NTSTATUS` whose severity is encoded in the value and letting Win32 put
`GetLastError()` on top. Both keep the reason IN the return value and
push the global up into userland, which is where it can be made
per-thread later without the kernel knowing.

So `libsys` holds the global (`userland/rt/sys.c`). It is a plain `int`
today and it is the one thing here with a known expiry -- when TLS lands
(`docs/roadmap.md`) that declaration moves and nothing else does.

**Why Linux's actual numbers.** `EPERM` 1, `ENOENT` 2, `EBADF` 9. A
private numbering would have to be learned by anyone reading this
kernel, and buys nothing. What is deliberately NOT copied is the size:
`abi/errno.h` defines fourteen codes, not Linux's ~130, and the bar for
a fifteenth is a handler that genuinely tells that case apart -- the
same bar `kernel/lib/` holds for a new helper. Copy the shape, not the
size.

**Why `SYS_RETRY` moved rather than becoming `-EAGAIN`.** It was -2,
which is now `-ENOENT`, so it had to move regardless; it went to -4095,
one past the top of the error range. Making it `-EAGAIN` looks tidier
and is wrong: EAGAIN is an error a caller reports, while SYS_RETRY means
the call did NOT fail -- the process was woken and must ask again, which
libsys absorbs in a loop no caller ever sees. Folding them together
would make every blocking call's spurious wakeup look like a failure one
layer up. That is the same mistake as the original "0 means try again",
which made a pipe read report EOF the instant its writer produced
something, and is why SYS_RETRY exists at all.

**Why the range is small, and what depends on it.** Errors are
`[-4094, -1]`; anything else is a result. That test has to stay
unambiguous against every syscall's legitimate return, which is why the
codes are dense at the bottom rather than spread out. The call that
makes it non-obvious is `SYS_SBRK`, which returns a POINTER -- a ring-3
heap address is nowhere near that window, so it cannot be mistaken for
an error.

**Why sbrk keeps a bare -1.** `(void *)-1` is its contract, and what
`heap_os.c`, `ugfx.c` and the WM already test against; a small negative
code there would be a plausible and wrong ADDRESS. It refuses with -1
and libsys records ENOMEM beside it, exactly as POSIX `sbrk()` does.
The consequence for `strace`: it does not decode a pointer-returning
syscall's -1, because -1 is EPERM's value and naming it would print a
reason sbrk never gave.

**Why the boolean syscalls were left alone -- and then flipped in one
commit.** `unlink`, `kill`, `gettime`, `proc_info`, `win_create` and
friends reported failure as **0**, not -1. A negative code is TRUTHY,
so converting them piecemeal would have made `if (!sys_unlink(p))`
callers read failures as SUCCESS -- silently, everywhere, at once. So
they were skipped by the original conversion and flipped later as one
atomic change: kernel handlers, the libsys wrappers, and every ring-3
caller together (docs/errno-design.md's leftover section records what
the flip found, including tolibc wrappers that had been quietly
POSIX-inverted). This is the same rule the plan set for itself: alter
only what a syscall REPORTS, never what it does.

## Why service ordering is `After=`/`Before=` and not a priority number

Services in `/etc/services.d` used to start in whatever order
`sys_listdir()` handed them back. Two candidates for fixing that.

**SysV's shape: a number.** `rc5.d/S20foo` -- a two-digit priority in
the name, a total order, no graph and no cycles possible. It is by far
the smaller change: one integer per descriptor, one sort.

**systemd's shape: names.** `After=`/`Before=` naming other services,
topologically sorted. Chosen, and the deciding argument is this repo's
own rule against facts somebody else has to keep true. A priority
number is exactly that shape: inserting a service between two existing
ones means renumbering them, and every number is only correct while
nobody has looked away. A name does not go stale -- it is the same
reason milestones here are titles rather than numbers, and the same
reason the changelog's build numbers were deleted.

The two keys are one relation written from either end, as in systemd.
That is not redundancy: the two ends are usually owned by different
people, so a service must be able to order itself against one whose
descriptor it has no business editing.

**It is ORDERING, not dependency.** `After=` does not start the named
service and does not stop this one from starting if the named one
failed. systemd separates ordering (`After=`) from requirement
(`Requires=`) for the same reason, and Windows' SCM conflates them in
`DependOnService`. Keeping them apart means each key says exactly one
thing; a `Requires=` can be added later without changing what `After=`
means.

**And ordering is LAUNCH ORDER, not availability -- which is the honest
limit.** A service is "started" the instant `SYS_SPAWN` returns a pid,
because nothing in this system can yet say "I am ready". So `After=`
promises the spawn happened first and nothing more. That is systemd's
`Type=simple`, which is also its default, so the weaker guarantee is
the common case there too. The stronger one (`Type=notify`/`sd_notify`)
needs an IPC path and a timeout policy and is a roadmap item; until it
exists, a service that needs another to be USABLE has to retry rather
than assume. Saying so in `data/etc/services.d/README.md` matters more
than the feature does -- an ordering key that quietly means less than a
reader assumes is worse than no key.

**A malformed graph never fails a boot.** A cycle drops one edge and
logs which service was started anyway; a name matching nothing loaded is
ignored with a line. systemd breaks cycles the same way. The reason here
is harsher than tidiness and is the same one behind the crash-loop
give-up: a machine that starts nothing has no console left to fix itself
from, so a typo in an ordering key must not be able to reach that state.
An unresolved name is not even necessarily a typo -- naming a service
that lives on the *other* boot target reaches that path identically, and
init cannot tell the two apart, so it reports what it saw rather than
guessing which.

**The sort is stable**, ties broken by the order descriptors were read.
Services with no constraints between them keep the behaviour they had
before any of this existed; an unstable sort would let adding a key to
one service reshuffle unrelated ones, which is what makes an ordering
feature untrustworthy.

`tools/init_test.py` covers it, and the shape of the test is the part
worth keeping: both groups of three demand the REVERSE of the order
their files are created in, expressed from opposite ends of the
relation. An init that ignored the keys would have to be handed a
perfectly reversed directory listing, twice, to pass -- and the control
run (`start_due()` walking `g_svc[]` instead of `g_order[]`) confirmed
listdir order really is creation order, so the assertion is not a
coincidence.

---

## The current directory belongs to the PROCESS, not to each shell

Until 2026-08-19 there was no cwd in the kernel at all: `apps/shell.c`
held a `char cwd[]`, `struct tosh` held another, and each resolved a
relative path against its own copy before calling `fs_*`. That works
exactly as long as the only things resolving paths are shells.

It stopped working the moment a filesystem command became a `/bin`
program. `SYS_SPAWN` passes its argument string VERBATIM, so `mkdir
docs` typed in `/tmp` reached `/bin/mkdir` as the bare word `docs`,
which the kernel then treated as `/docs` (`fs.h`: a bare name is
silently root-relative, for callers predating directories). The wrong
directory was created, silently, with an exit code of 0.

**The three ways out, and why this one.** A shell could rewrite its
children's arguments -- but it cannot tell a path argument from a flag
or a plain word, it leaves anything not started by a shell with no
notion of "here", and it is a SECOND path-resolution rule beside
`kpath.c`'s, which is the drift `kpath.c` exists to have fixed once
(`edit ../x` meaning different things in two windows). New programs
could demand absolute paths -- honest, and it makes the ring-3 shell
noticeably worse to use than the kernel one. Or the kernel holds it,
which is what Linux (`chdir`/`getcwd`) and NT (the PEB's current
directory) both do, and for this reason.

So `struct sched_cwd` sits in the process slot beside `struct
sched_heap`, with the same two-owners-one-representation shape: a
scheduler slot holds one, and the kernel context holds a single one of
the same type for the legacy `elf_run.c` loader, reached through the
same accessors so the two cannot drift.

**Every path-taking syscall resolves, including the three that predate
this.** `open`, `unlink` and `listdir` go through the same
`resolve_user_path()` as the six new ones -- otherwise `cat notes.txt`
would still mean a different file depending on who typed it. An
absolute path is unchanged by resolution and a process that never calls
`SYS_CHDIR` starts at `/`, so nothing that already worked behaves
differently.

**It is INHERITED across a spawn**, unlike the name and the CPU time in
the same slot, which are reset. That is the whole point: a child starts
where its parent was standing. The kernel shell's `cd` sets the kernel
context's copy for the same reason, so `cd /docs` followed by `spawn
/bin/mkdir notes` does not create `/notes`.

**The check that proves it is two-sided.** `userland/tests/cwd_test.c`
chdirs into a subdirectory, spawns `/bin/mkdir` with a bare name, and
asserts both that the directory appeared where the cwd pointed AND that
nothing of that name appeared at the root. Only the first would stay
green if the kernel resolved against `/` and the check happened to look
there too; only the second cannot tell a correct spawn from one that
failed outright. A positive control that pins a child's cwd to `/`
reddens exactly those two and nothing else.

It is a KTEST that spawns the ELF (`kernel/fs/cwd_test.c`) rather than
a `tools/usertest_run.py` entry, for the reason `fd_test` and
`pipe_test` are the same: under the shell's `run` the legacy loader has
no scheduler slot, so the test's own `waitpid` cannot park and it looks
for the child's work before the child has done any.

---

## Six filesystem syscalls, and what each refusal is allowed to say

`SYS_MKDIR`, `SYS_RENAME`, `SYS_TRUNCATE`, `SYS_STAT`, `SYS_LINK` and
`SYS_SYNC` (2026-08-19). Every one is a single `fs.h` call that already
existed -- the kernel could do all of this and ring 3 simply had no way
to ask, which is why the kernel shell's filesystem commands could not
become programs.

**The handlers check what they CAN distinguish before falling back.**
`fs_mkdir()` returns one 0 for "already exists", "no parent" and "the
disk refused", so the handler tests existence itself and reports
`-EEXIST` rather than collapsing three reasons into `-EIO`. That is the
whole point of `abi/errno.h`: `/bin/mkdir` printing "file exists" and
"no such file or directory" as different sentences is the difference
between a usable command and one that says `-1`.

Four codes were added to serve them, each genuinely distinguished by a
handler rather than because POSIX has a name for it: `ENOTDIR` (`chdir`
onto a file, which is not the same as `chdir` onto nothing), `EISDIR`
(truncate or hardlink a directory), `ERANGE` (`getcwd` into too small a
buffer) and `ENAMETOOLONG` (the one path failure that says nothing
about the filesystem -- the file may well exist, this kernel just
cannot name it).

**`getcwd` refuses rather than truncating.** A shortened path names a
different directory; it is not a shorter answer to the same question.
Same reasoning as `kernel/lib/`'s rule that a formatter which does not
fit its buffer writes nothing.

**`SYS_SYNC` returns a COUNT, not a status.** With a write-back cache in
front of the disk, a write that returned success can be refused later,
at the flush -- and "the flush failed and your data is still only in
RAM" is the one disk answer a caller must not read as success. Zero is a
real answer (nothing was pending), which is exactly why a bare ok/failed
would not do.

**`SYS_STAT` is deliberately not POSIX's `struct stat`.** There are no
modes, owners, devices or link counts to put in one, and a struct full
of zeroed fields invites a caller to believe them. Its timestamps are
`struct rtc_time` for the same reason `struct dirent`'s are: the epoch
shape is kernel-internal (`fs.h`'s `fs_stat_info`) and is converted back
to civil time at the boundary.

---

## Three words for system state: fact, setting, tunable

Fixed as project vocabulary on 2026-08-19, while designing the query
registry. Defined once in `docs/settings-and-queries.md`'s "The
vocabulary"; this entry is why those three and not others.

**The problem a name solves here is telling apart things that look
alike at the call site.** `mem_free` and `font_size` are both "a named
value the system knows", both addressed as `namespace.name`, and both
readable with one call -- and they behave completely differently.
One is computed on every read and has no stored form; the other has a
file behind it, survives a reboot, and can be reset to a default. A
single word for both invites exactly the API that has to explain, per
value, which one you are holding.

**Why "fact" rather than "property" or "metric".** *Property* implies
something the object HAS and could in principle set; the whole point is
that nothing can. *Metric* implies a measurement over time -- fine for
`mem_free`, wrong for the process list or a PCI device, which are half
of them. *Fact* carries neither implication and reads correctly for a
scalar and for a list.

**Why "tunable" is a kind of setting rather than a third thing.** It
differs from an ordinary setting only in what its `apply` touches: a
live kernel variable as well as a file. Giving it its own registry
would mean three mechanisms and three places to keep the `(namespace,
name)` addressing consistent, to express one extra behaviour that
`struct setting` already has a slot for. The word still earns its keep,
because "setting" alone does not tell a reader whether changing it does
anything before the next boot -- which is the first question anybody
asks about a kernel knob. Same split sysctl makes: `/etc/sysctl.conf`
persists, `sysctl -w` writes live, and both are the same names.

**Why a fact is not a setting with the write refused.** This was the
tempting simplification -- one registry, a read-only flag, `config
list` showing everything. Two things break. The setting registry's
value is answering "what can I change?", which is what GENERATES
Control Panel; two hundred read-only counters in that list bury the ten
real settings, and Control Panel would have to start filtering a list
it currently trusts wholesale. And a fact has no stored form at all, so
`config unset` and `config diff` -- both of which compare live against
file -- have nothing to operate on. A flag would have to disable half
the command's verbs per row, which is a registry wearing two shapes.

---

## Kernel state is a QUERY SYSCALL, not a `/proc`

Built 2026-08-19 as stage 0 of `docs/query-design.md`, which has the
full staging. This entry is the decision.

`/proc` is the obvious answer and is wrong here for three reasons that
get worse in order. It needs a MOUNT TABLE that does not exist --
`vfs.c` holds one global `struct fs_ops *`, chosen by probing the disk,
so a second filesystem mounted at a path is the "Real mount points"
milestone, taken on in order to print `mem_free`. It INVERTS A
DEPENDENCY: diagnostics would sit on top of storage, so a machine whose
disk failed to mount loses its introspection at exactly the moment
somebody needs it, and `dmesg` explaining why the mount failed must not
itself be a file. And its ABI would be TEXT, which every consumer
re-parses -- the rule `uui_table` already states (it cannot sort the
text it draws), and it costs real features: a Task Manager that graphs
memory needs a number.

Linux is the only mainstream system that made this a filesystem.
Windows uses `NtQuerySystemInformation(class, buf, len, &returned)`;
macOS and the BSDs use `sysctl(mib[], ...)`. Both are syscalls. The
filesystem is the outlier, and it is the one shape this kernel cannot
cheaply afford.

**What was already here** is most of the NT shape without the name:
`SYS_SYSINFO`, `SYS_PROC_INFO`, `SYS_PCI_INFO` and `SYS_CPU_INFO` are
four syscalls that are each ONE information class, each with its own
number, struct and handler. The number grows by one per fact -- the
growth `tools/check_dispatch.py` exists to notice -- and not one of them
can answer "what facts exist?", which is what `config` needs in order to
read a fact at all.

**Why a registry rather than a switch.** A provider registers from the
subsystem that owns the numbers, the way a `display_driver`, a
`block_device`, a `clocksource` and a `struct setting` already do.
Adding a fact is a struct in `abi/`, a provider, and a registration; no
central table, no init call to forget. A duplicate class is REFUSED
rather than resolved first- or last-wins, because either would make the
answer depend on link order.

**Class 0 is the registry describing itself**, and that is not
decoration. It means a program needs to know exactly one number to
discover every other class -- and it gives the LIST path (`count`,
`index`, the walk) a real caller from the first commit, rather than
leaving it an unvalidated half. This project's standing rule is that a
seam with one implementation has not been tested; a registry whose only
provider is a scalar would have shipped exactly that.

**`len` is the version tolerance, and it is why the message carries a
length at all.** The kernel writes `min(len, record)` and reports how
much in `returned`, so a record that GAINS a field does not break a
binary built against the shorter one. The rule that makes it work:
existing fields never move and never change meaning -- growth is
append-only. Same contract as `NtQuerySystemInformation`'s returned
length.

## `config get` reads facts, and named fields are how

A record has several fields, so `config get mem.frame_free` needs a way
to name one -- without it a fact has no flat name at all and nothing
could read a single value or list facts beside settings.

`struct query_field` (name, type, offset) declares them, and
`QUERY_FIELD()` derives the offset from the struct member so nobody
maintains a number. **The offsets never cross the syscall boundary**:
ring 3 sends a name and gets back a value and a type, which is what
keeps a record free to grow append-only and stops a bad offset arriving
from userland. Every field is 64 bits, so the reader copies eight bytes
and has no per-type switch that could disagree with a declared width.

**A LIST class deliberately has no field table.** `config get
providers.anything` answers `-ENOTSUP`, which is a DIFFERENT answer from
`-ENOENT`: the class exists and is a table, so the message sends the
reader to a tool that can show one instead of leaving them hunting for a
typo they did not make. The alternative -- `proc.3.name` -- is sysctl's
worst corner: an index baked into a name shifts as the list changes, so
the same string means a different process a second later. Linux keeps
processes out of sysctl entirely and reaches them through `/proc/<pid>/`.

**A setting wins over a fact of the same name.** None can collide today
(the registries are separate and nothing checks across them), but
resolving settings first means `config get` and `config set` can never
be talking about different objects.

**And the answer is LABELLED.** A fact prints as read-only kernel state
rather than in the shape a setting prints in, because the two have
different lifetimes: a setting survives a reboot and a fact does not
EXIST between them. A reader who cannot tell which they just read has
been told something misleading, however correct the number is.

## A wait channel is an ADDRESS, not a category

`scheduler_wake(chan, value)` releases the processes parked on one
address. A waiter names the object it is waiting for -- this pipe, this
client's event queue, this process's own slot -- and nobody else is
disturbed.

It used to be a category. `wait_reason` was a small enum
(`SCHED_WAIT_EVENT`, `SCHED_WAIT_PIPE`, ...) and a wake released
everything in it, with the header arguing the case in as many words:
"it names the EVENT that happened and every process parked on it
wakes." That is correct -- every woken process re-runs its syscall and
re-parks if it is still not ready, so a spurious wake costs a syscall
and never a wrong answer -- and it is O(waiters) per event on the two
busiest paths in the system. `win_events_push()` queues an event for ONE
pid and then woke every process blocked in `SYS_WAIT_EVENT`, so with N
windows open every keystroke and every mouse MOVE woke all N clients,
each to pop an empty queue and park again. Pipes had the same shape: a
write to one pipe woke the readers of every other.

**Why an address rather than a wait queue.** The three real designs:

- **Linux** embeds a `wait_queue_head_t` in the object (pipe, inode,
  socket) and `wake_up()` walks that object's list. It also carries
  `WQ_FLAG_EXCLUSIVE`, which exists precisely to stop thundering herds
  on `accept()`.
- **Windows NT** gives every waitable thing a dispatcher header with its
  own wait list, and `KeWaitForSingleObject` queues onto it.
- **FreeBSD** has no per-object structure at all: `tsleep(chan)` /
  `wakeup(chan)` take an arbitrary address and hash it into sleepqueues.

FreeBSD's is the shape taken, because this scheduler already walks a
fixed `procs[]` array on every wake. Per-object wakeups therefore cost
one POINTER compare where the enum cost one INT compare -- there is
nothing to allocate, nothing to initialise in each waitable object, and
nothing to tear down when one is freed. Linux's and NT's designs buy
ordering and exclusive wakeups, and neither is needed by a uniprocessor
kernel whose waker already holds every slot in one array. Copy the
shape, not the size.

**The two rules a channel must obey**, both consequences of it being a
bare address. It must OUTLIVE the wait, so a stack address is never a
channel -- the frame is gone by the time anyone wakes it. And a channel
whose object is freed must have its waiters woken first, or they are
parked on an address that no longer means anything; the per-object wake
sites sit next to the teardown that frees them for this reason
(`pipe_close_reader()`/`pipe_close_writer()` are the worked example).

**`wait_reason` still exists, and is never matched.** It is a label, so
the `kstack` debug surface can print "pipe" instead of a pointer. Keeping
it is not the rejected "encode the fact twice" option: nothing decides a
wake from it, and it is free to be wrong without affecting behaviour.

**What made the change testable** is `scheduler_test_park()`, which
fabricates a blocked slot. The property is selectivity, and it needs two
processes blocked on different channels at one instant -- a race to
arrange with real processes. Two rules make fabricating safe, and both
were learned by getting them wrong first: hold `scheduler_preempt_disable()`
across the window (a woken fabricated slot is READY, so the scheduler
would otherwise switch to it and `iretq` through a stack local), and
RELEASE BEFORE ASSERTING (a `KTEST_ASSERT` returns from the body, so an
assertion made while a slot is fabricated leaks exactly the READY slot
the first rule exists to prevent). The first version instead refused
unless the process table was empty, which was safe and useless: the
default boot is graphical, so the one test that matters skipped on every
ordinary run.

## `FS_OP()` is a preemption guard and NOT a sleeping lock, because syscalls run with interrupts OFF

The recurring proposal is to replace `vfs.c`'s
`scheduler_preempt_disable()`/`_enable()` pair with a real mutex, so the
machine keeps running during disk I/O. `docs/roadmap.md` carries it as
an item. It does not work, and the reason is worth writing down because
the argument for it is superficially very strong.

**`int 0x80` goes through an INTERRUPT gate** -- `idt_set_gate(128,
isr128, 0, 0xEE)`, type `0xE` -- so the CPU clears IF on entry and
interrupts are off for the whole syscall. `kernel/drivers/ata.c`'s
`wait_dma_irq()` is built around this: it polls the Bus-Master status
register rather than blocking, because `hlt` there would park forever
with not even the timer able to tick, and `sti`-then-block is the
`g_next_kernel_rsp` reentrancy bug this kernel has already been bitten
by.

Three consequences, in order of how badly they break the idea:

1. **A spin lock would HANG.** A contender inside a syscall cannot be
   preempted -- no tick -- so the holder never runs and the wait never
   ends. Not slow: deadlocked.
2. **A sleeping lock cannot live where the guard lives.** Parking a
   caller needs the syscall's saved trapframe, and no `fs_*` entry point
   takes one; this kernel blocks by descheduling and RE-RUNNING the
   syscall, not by switching kernel stacks the way Linux's
   `mutex_lock()` → `schedule()` does. Twenty-five files call the
   filesystem -- `elf.c`, `scheduler.c`, `tz.c`, `keyboard_layout.c`,
   `etc_config.c`, ATA code, KTESTs -- and most have neither a trapframe
   nor a scheduler slot.
3. **The benefit was misattributed.** "Every disk read freezes the
   machine" is true, and the guard is not what causes it: a ring-3
   syscall is already atomic because IF is 0. The guard only adds
   serialization for KERNEL-CONTEXT callers, which run with interrupts
   on and are genuinely preemptible -- and that is exactly the
   interleaving its own comment describes, the WM and an app both
   reading files. Swapping it for a lock would change nothing for the
   syscall path.

**So the real item is INTERRUPTIBLE SYSCALLS** -- a trap gate (`0xEF`)
plus retiring `g_next_kernel_rsp` as a single global -- and a sleeping
lock is a consequence of that, not an alternative to it. The guard stays
until then. Recorded because the lock is an obvious-looking change that
survives every argument except reading the gate type.

**MEASURED 2026-09-15, AND THE ORDERING NEEDS A THIRD LINK.** The trap
gate was flipped and `tools/latency_under_io.py` run under KVM, two runs
a gate: the compositor's loaded `wake` goes from ~14 ms average to
**0.3-0.4 s, with a 1.2-1.4 s worst case**, and it gets a FIFTH of the
frames. The syscall stall table barely moves, so it is not a handler
getting slower -- it is the compositor not being RUN, because `FS_OP()`
still holds preemption off for the whole backend call. **The gate alone
is a regression a user would feel, not an improvement awaiting polish.**

So "the gate first, the lock after" is right about the order and wrong
about it being enough, and the reason is point 2 above rather than
anything new: **blocking here abandons the kernel stack.**
`block_common()` takes the ring-3 trapframe, stores it as
`procs[idx].kernel_rsp`, and resumes with `iretq` back to ring 3 -- so a
parked caller RE-RUNS its syscall. That is fine at a syscall entry point
and impossible for a mutex deep inside `tfs3`'s block walk, whose
position lives on the kernel stack being thrown away.

**The machinery to suspend a kernel stack DOES exist now**, and that is
what changed since this entry was written: a timer tick preempting a
syscall under the trap gate saves and restores exactly that
(`isr_context_outer()`/`_defer()` beside `kernel_rsp`), which is why
ktest is clean at `0xEF`. What is missing is a VOLUNTARY door to it -- a
`schedule()` that suspends the current kernel context and resumes it
mid-call, which is what Linux's `mutex_lock()` reaches. Until that
exists a sleeping lock cannot be written, and until the lock exists the
gate costs more than it buys.

## Boot order is a hand-written list, and a violation of it PANICS

**The defect was never the list.** `kernel_main()` is a sequence of
init calls in a deliberate order, and reading it top to bottom is the
clearest description of this kernel's boot that exists. What made
ordering mistakes expensive is that they failed SOFTLY and pointed
somewhere else: a driver probing before `pmm_init()` got "queue 0 needs
3 contiguous frames and none were free", which reads as a broken device;
a PCI scan before `pci_init()` finds no devices, which reads as absent
hardware. Both were paid for -- the first is why virtio-blk's init call
carries a comment about its position, and virtio-gpu hit the same wall
from the display side.

So the fix is loudness, not structure. `kernel/include/kernel/bootstage.h`
records which subsystems are up, and `pmm_alloc_frame()`,
`pmm_alloc_contiguous()`, the pmm free calls and `pci_device_count()` /
`pci_device_at()` panic when used before theirs. The panic names the
calling function and the init it needed:

    PANIC: pmm_alloc_contiguous ran before pmm_init() -- see kernel_main()

**A BITMASK, not a stage number.** An ascending "boot stage" would
encode a total order that is not a fact -- PCI before PMM before the
heap is what the sequence happens to be, not something anything depends
on. The question a caller has is "is the thing I am about to use up?",
which is one bit per subsystem and claims no ordering.

**A subsystem marks ITSELF up**, at the end of its own init function,
never from `kernel_main()`. A flag set from the caller can drift from
the thing it describes; that is the failure mode of every
"remember to update the other file" convention this repo has deleted.

**Why not initcall levels** (Linux's shape: linker sections collecting
`DRIVER_INIT(core, foo)`, which this repo already has the machinery for
in `.ktests`): they would replace a readable list with an order derived
from link order, and -- the part that matters -- they do not make a
violation loud on their own. Linux needs them because it has hundreds of
drivers and a real dependency graph; it also needs `-EPROBE_DEFER` on
top, because levels alone do not express dependencies. This kernel has
had exactly one ordering edge that ever mattered. Levels stay on the
roadmap for when a third driver needs a slot rather than being built on
speculation.

**REVISITED 2026-09-02: the levels are built, and the bitmask stays.**
The bar above was met the way it said it would be -- not by a third
driver but by the fiftieth: `kernel_main()` had grown to ~50 `*_init()`
lines (a dozen drivers, twenty-two query providers, seven `/etc`
readers), each one an edit to a file that is not the subsystem's own,
and the maintainer asked for the list to go. `INITCALL(fn, LEVEL)`
(`kernel/include/kernel/initcall.h`) is Linux's `module_init()` shape
on the `.ktests`/`.drivers` mechanism this repo already had: six levels
in dependency order (core, bus, device, fs, config, query), link order
within a level, walked by `initcalls_run()` one level at a time from
`kernel_main()`, which keeps only the genuinely sequential bring-up by
hand. What the paragraph above got right is kept: the levels do NOT
make a violation loud, `BOOT_REQUIRE()` still does, and the two are
complementary rather than alternatives. What it got wrong was the
forecast that a readable list beats a derived order -- the list stopped
being readable at fifty lines, and the order it encoded was mostly "any
time after the heap". The three real edges (a class core before its
registrants, virtio after the heap, the /etc readers after the mount)
are now levels, which is the honest way to say "this is the order and
nothing else is". `tools/check_initcalls.py` fails the build on an init
that is both declared and hand-called (it would run twice) or declared
at a level nothing walks (it would never run); the `initcall` KTEST
checks at boot that every declared one ran.

**No BOOT_SUB_HEAP, and the reason is a trap worth restating.**
`kernel/lib/heap_core.c` is compiled twice -- kernel and `libuapp.a` --
so a kernel-only include there would silently take `malloc()` away from
ring 3, exactly as `kfmt.c` would. It would also buy nothing: the heap's
state is zero-initialised BSS, so an early `kmalloc()` does not
misbehave, it grows the heap through `pmm_alloc_frame()`, which is
already guarded. The useful assertion is on the path either way.

**The panic itself cannot be unit-tested** -- it halts the machine,
which is what it is for. The KTEST covers the half that rots silently
(an init that stopped marking itself up); the failing case is verified
by a positive control at the source, where the boot smoke test catches
the panic line.

## `lseek` made an fd's position mean something writes had been ignoring

`SYS_LSEEK`, `SYS_FSTAT` and `SYS_O_APPEND` landed together as Stage 0
of `docs/libc-design.md`, and adding the first one forced a change to
the second-oldest assumption in the fd table.

**A write to a file fd used to append unconditionally.** `struct
open_file` carried an `offset` maintained by the READ path only, and
`sys_do_write_file()` deliberately ignored it -- its comment said so,
and the reasoning was sound at the time: making writes honour it would
have changed what every existing caller did, which was a separate
change from the truncation fix being made that day.

`lseek` is that separate change arriving. A position that one half of
the interface ignores is not a position, and "seek, then write" is not a
thing that can be bolted onto an fd whose writes always go to the end.
So the position is now shared by reads and writes, as POSIX has it, and
`SYS_O_APPEND` is how a caller asks for the old behaviour.

**What made this safe to do rather than a flag day**: almost everything
that opens for writing here passes `SYS_O_TRUNC`, and position 0 of an
emptied file IS its end -- so those callers changed behaviour by exactly
nothing. Auditing the rest found ONE real caller relying on the old
silence: the shell's `>>`, which had been getting appending by not
asking for it. It asks now, and that is what finally makes `>` and `>>`
different operations rather than the same operation with a truncate in
front of one of them.

**`isatty` is a FLAG, not a syscall.** `SYS_FSTAT` fills the same
`struct sys_stat` the path-keyed `SYS_STAT` does -- one struct, as in
POSIX, rather than a second one meaning almost the same thing -- and
what an fd adds is `SYS_STAT_TTY` and `SYS_STAT_SEEKABLE` in the
existing `flags` word. Flags rather than new struct fields because a
bare `is_tty` member would mean nothing for a path stat, which is
exactly the "a struct full of zeroed fields invites a caller to believe
them" hazard that struct's own comment warns about; a flag that is
simply not set reads correctly for both callers. And a dedicated
`SYS_ISATTY` would have been a syscall returning a single bit that the
very next thing stdio wants (a size, to pick a buffer) makes redundant.

Linux and NT both have the fd-keyed and path-keyed stat share a
structure for the same reason, and both spell "this stream has no
position" as `ESPIPE` -- a name that mentions pipes only because a pipe
was the first unseekable thing, and which a libc's `fseek()` turns
straight into the errno a program expects.

**A bug fell out of the audit.** `SYS_OPEN` tested existence by
`fs_read()`-ing the whole file, which cost a full read on every open
and, worse, reported `-ENOENT` for any file too large for the staging
buffer -- `fs_read()` returns NULL for "cannot load this whole" exactly
as it does for "not there". Nothing had noticed because nothing opened a
large file; a libc does immediately. It asks `fs_exists()` now.

## The kernel does not store an environment, and inheritance is tolibc's job

`SYS_SPAWN` carries the child's environment EXPLICITLY, as a blob it
copies onto that child's initial stack, and keeps nothing afterwards.
There is no per-process environment in the kernel and no inheritance in
the syscall.

**This looked like a fork and is not one.** The question posed was
whether a child should inherit its parent's environment automatically
(what most code expects) or be handed one explicitly (what
`posix_spawn` does). Unix answers both at once by LAYERING: `execve()`
is the primitive and takes `envp` explicitly, while `execv()` -- no
`e` -- is the C library function that passes the global `environ` for
you. The kernel never inherits anything; the library does it on your
behalf. `posix_spawn` has the same shape. Windows is the outlier, where
`CreateProcess` takes an environment block and NULL means "inherit from
me".

toy-os copies the Unix split, and it fits a process model that was
already `posix_spawn`-shaped rather than `fork`-shaped. `sys_spawn()`
is `execv` and `sys_spawn_env()` is `execve`.

**Why not let the kernel inherit.** It would need to store an
environment per process and copy it at every spawn -- and the thing
every caller actually wants, "like my parent's but with one change",
would then need a second syscall to express, because the inherited copy
is the kernel's rather than the caller's. Passing it explicitly makes
that case free: the library edits `environ` and hands over the result.

**`environ` lives in libsys, not tolibc.** crt0 IS libsys, and argc,
argv and envp arrive together on the initial stack -- the startup vector
is one thing and one layer should own it. Putting it there is also what
made inheritance free for every existing caller: `sys_spawn()` passes
`environ` itself, so tosh, init and the WM inherit without a line
changing. Had it lived in tolibc, libsys could not have reached it and
every caller would have needed updating by hand.

**One blob, not a `char **`.** The environment crosses as
`"K=V\0K=V\0\0"`, so the kernel copies a single validated run of bytes.
A pointer array would mean walking user memory entry by entry,
validating each pointer and each string separately -- and `args` is
already one string for exactly this reason. Oversized is refused with
`E2BIG` rather than truncated, because a child silently missing half its
variables is a bug that surfaces somewhere else entirely.

**`SYS_SPAWN` became a message struct rather than gaining a second
syscall number.** Its three argument registers were full. A
`SYS_SPAWN_ENV` beside it would have left two syscalls doing one job
forever, and the next argument would have had to pick one -- which is
how a syscall table grows a shape per feature instead of a row per
capability. The struct is the shape `SYS_SETTING` and `SYS_WIN_REQUEST`
already use, and it has a `reserved` field that must be zero so a
caller built against a later struct is REJECTED rather than silently
having its extra request dropped.

**A consequence worth stating: a program started by the ring-0 shell's
`run` has no environment at all.** Inheritance is a library convention,
so a process gets one only if its parent had one to pass, and the kernel
shell is not a ring-3 process. That is correct rather than a gap, and it
disappears when the ring-0 shell does.

## Signals deliver on the way back to ring 3, and there are exactly two such places

**Why not act at send time.** A signal can be raised from another
process's syscall, from a fault handler, or from the keyboard IRQ.
Acting on one means tearing the target down: freeing its page tables and
the kernel-side bookkeeping keyed to its address space, which calls the
heap. From an interrupt that landed inside somebody else's `kmalloc`
that is corruption, not a race. And writing another process's register
state from inside a syscall the SENDER made is the shape of bug this
project has already paid for once, with a compositor's mapping revoked
underneath a live process.

So sending only sets a bit. Acting happens at a point where the kernel
provably holds nothing: **when the trap being handled came from ring 3.**
If the CPU was executing ring-3 code when the trap arrived, no kernel
work was in flight and the process about to be resumed is the one whose
state may be thrown away. Unix delivers at the same point, from the same
constraint.

**Two places, and BOTH are needed.** This was found by building one and
discovering the other by positive control:

- **At syscall entry**, before the handler runs. Without it, a process
  woken out of a blocking call by `-EINTR` resumes in ring 3 a few
  instructions from its next syscall -- and whether it died of the
  signal or reached `exit()` first was a race. Losing that race is
  permanent: the exit path zombies the slot, so the pending bit can
  never be acted on. It passed most of the time, which is the worst way
  for a race to behave. Linux reaches the same rule from the other side:
  a fatal signal pending means the syscall returns without doing
  anything.
- **At the end of the trap**, which is the only thing that reaches a
  process making no syscalls at all -- a compute loop, interrupted by
  the timer.

Delivery at the TOP of `isr_dispatch` was considered and is wrong for
hardware IRQs: an IRQ must reach its handler to be acknowledged to the
PIC, so returning early from one stops interrupts for the rest of the
boot.

**A pending bit means "must die", with no policy lookup.** An ignored
signal is dropped at arrival rather than queued -- POSIX's rule for
`SIG_IGN` -- and with no user-space handlers every other disposition
terminates. That is what lets a single `if (pending)` in the trap path
be the whole delivery test. Handlers break the invariant, and
`api/scheduler.h` says so beside the field.

**`SIGKILL` deliberately bypasses all of it.** It terminates from the
sender, ignoring dispositions, which is what makes Force Quit
trustworthy against a process wedged in its own loop -- the one case the
pending mechanism cannot reach, because such a process never returns to
ring 3. It is also why the INTR key sends `SIGINT` rather than
`SIGKILL`: the immediate teardown is not safe from an interrupt handler.

**A guard that was written, tested and removed.** `scheduler_block_
current()` briefly refused to park a process with a signal pending, to
stop a woken process re-entering the same blocking call forever. A
positive control showed it reddened nothing: the syscall-entry delivery
above means such a process never reaches that function. Keeping it would
have been an untestable guard with a comment claiming a mechanism that
was not the one doing the work. It comes back if syscalls ever become
preemptible -- every gate is an interrupt gate today, so nothing can
raise a signal against a process part-way through one.

## "Not the running process" and "not the loaded address space" are different questions

Terminating a signalled process that the scheduler had already switched
away from leaked its entire address space -- ELF pages, stack, heap,
window buffer -- once per signal, silently, with only a log line saying
the cleanup had been refused.

`switch_to_kernel()` hands the CPU back to the kernel context **without
changing CR3**, because every address space shares the kernel's mappings
and the kernel context does not care which one it is standing in. So a
victim that is no longer `current_index` can still be what CR3 points
at. `syscall_process_kill_cleanup()` guards on "is this the CURRENT
address space?", which is the only question it can ask, and refused.

The fix is for the caller to ask the first question and then make the
second one false: if CR3 is the victim's, move it to the kernel's before
killing. That is safe for exactly the reason the tick did not bother --
nothing is executing there. It is NOT safe when a process is running in
that address space, which is the separate branch.

The general lesson: a guard phrased in terms of what a function can
observe is not the same as the invariant it was meant to enforce, and
the gap shows up as a refusal that looks like a safety net working.

## `SYS_SPAWN` carries the process group, because there is no fork to close the window

POSIX has both the parent and the child call `setpgid()` after `fork()`,
because neither can be sure which runs first and a `Ctrl-C` arriving in
between would signal the wrong group. It is a documented race that
POSIX papers over with a redundant call from each side.

toy-os has no fork. `SYS_SPAWN` returns a process that is already
running, so there is no second side to make the redundant call from --
the window would be genuinely unfixable rather than merely awkward. So
the group is an argument to the spawn: `PGID_NEW` leads a new group, a
positive value joins one, 0 inherits the caller's. `posix_spawn` reached
the same answer with `POSIX_SPAWN_SETPGROUP`, for the same reason.

`SYS_SETPGID` still exists, for the one thing spawn cannot express: a
process naming its own group. `/bin/tosh` calls it so that a shell
started by init does not leave init's group in front of the console,
which would point every `Ctrl-C` at pid 1.

The field it occupies was `reserved`, which had to be zero -- so every
caller that predates process groups passes "inherit" without an edit,
and the "reserved must be zero" check became a range check on a real
value.

## `Ctrl-C` was recognised in the keyboard driver, and that WAS temporary

On Unix, `Ctrl-C` is not a kernel feature at all: a terminal's line
discipline recognises the INTR character and signals the terminal's
foreground process group. Every piece of that sentence is a separate
mechanism, and it is why `Ctrl-C` is the last thing a system gets rather
than the first.

toy-os had no line discipline -- fd 0 was raw, with no echo control and
no cooked mode. The recognition therefore lived in `keyboard.c`, which
is the one place a key arrives, and `kernel/tty.h` said plainly that it
belonged somewhere else. Putting it in the driver first and moving it
later is the right order; discovering it later as a layering mistake
would not be.

**IT MOVED ON 2026-08-22, AND THE PLAN HELD.** `kernel/tty/ldisc.c` is
the discipline, the driver just produces keystrokes, and the payoff was
the one this entry predicted: a Terminal window gets the SAME `Ctrl-C`
rather than a second answer to the same question -- disabling
`signal_char()` reddens the checks for both, and `Ctrl-Z` was later
added to that one function and worked in a window with no further work. `docs/tty-design.md` has the whole staging;
this entry is kept because the ORDER was the decision, and it was
right.

**The two outcomes of `signal_char()` are the two states a shell is
in**,
and keeping both is what makes the feature additive rather than a
replacement. With a job in front, the group is signalled and the byte is
DISCARDED, which is what a line discipline does with INTR -- delivering
it as well would leave a stray `0x03` for whoever reads next, and the
shell is exactly who that is. With no job, nothing is signalled and the
byte goes through, so `Ctrl-C` at a prompt still abandons the line
through the same `KLINE_CANCEL` both shells have always had.

The consequence worth stating: **the shell has to print the `^C`
itself**, because with a job running the editor never sees the key.
`bash` prints it from the same place and for the same reason.

## A stopped process is a FLAG beside its state, and stop/continue never reach the pending set

Job control needed two things the signal design did not have: a process
that exists but is not scheduled, and a way to say so that did not break
the invariant everything else rests on. Both had an obvious answer that
was wrong here.

**The obvious answer to the first is a fifth `enum sched_state`, which
is what Linux does** (`TASK_STOPPED`). It is wrong for this kernel
because of the BLOCKED case. A process suspended while parked on a pipe
has to come back to that pipe, so a real state has to remember which
state it displaced and what channel that state was waiting on — and
then `SIGCONT` has to put it back, correctly, into a state the test
suite cannot reach. It cannot be reached because this kernel has no
interruptible syscalls (`docs/roadmap.md`): there is no way to wake a
blocked process in order to stop it, which is exactly the mechanism
Linux's state transition exists to serve. Building the bookkeeping for a
transition nothing can exercise is how a subtle bug gets a permanent
home.

As a flag it composes with all four existing states for nothing. **One
line in `find_next_runnable()`** honours it; a wake still lands and
writes the parked trapframe, leaving the slot READY-but-stopped so the
syscall completes the moment somebody continues it; `SIGCONT` is one
clear. Userland is unaffected either way — `PROC_STATE_STOPPED` is
reported ahead of whatever the process was doing underneath, because
once it is suspended the block is no longer why it is not running.

When interruptible syscalls land, this is the decision to revisit: at
that point the Linux shape becomes buildable AND testable, and the flag
stops being the cheaper of the two.

**The second decision is that `SIGSTOP`/`SIGTSTP`/`SIGCONT` are applied
at SEND time**, by the sender, rather than queued in `pending` and acted
on at the return to ring 3 like every other signal. The invariant `a
pending bit means this process must die` is what lets `pending` be read
with no policy lookup at all, and suspending is not a kind of dying — so
routing a stop through it would cost every reader a lookup, for nothing.

It is also *safe* to do at send time in a way a termination is not. A
stop flips one byte of scheduler state: it allocates nothing, frees
nothing and unmaps nothing, which is the same restraint
`scheduler_wake()` keeps and the reason both are callable from the
keyboard IRQ. `Ctrl-Z` arrives there, so this is not a theoretical
property.

Two consequences worth stating, because each surprises somebody:

- **A stop reaches a process wedged inside a kernel path**, exactly as
  reliably as a running one — more than `SIGTERM` can say, and the same
  guarantee `SIGKILL` gets by the same route.
- **A `SIGTERM` to a STOPPED process does nothing until it is
  continued.** The bit is set and delivery waits for a return to ring 3
  that a suspended process does not make. POSIX behaves identically and
  `SIGKILL` is the exception here as there, but it looks like `kill`
  being broken, so it is written down in three places including
  `docs/commands/kill.md`.

**And the testing lesson, which generalises past signals.** Five of the
six KTESTs written for this assert on `scheduler_test_state()` — the
flag. Deleting the one line in the picker, so a "stopped" process
carries on running, left all five GREEN. The sixth spawns
`/tests/spin_test`, suspends it, and asserts its `cpu_ns` does not
advance across twenty ticks; that one failed on the right assertion.
**Anything the scheduler DECIDES has to be tested by measuring progress,
not by reading the bookkeeping the decision is made from.**

## The keyboard layout is keyed on evdev keycodes, so only the PS/2 driver sees a scancode

**The bug that forced the question.** `|` could not be typed at all on an
`INPUT=virtio` boot, and worked perfectly on PS/2. The key is `KEY_102ND`
-- the extra key an ISO keyboard has between Left Shift and Z, which on
every Nordic layout carries `<`, `>` and, with AltGr, `|`. So a pipeline
was untypeable on one keyboard and fine on the other, which is not a
"some keys are unsupported" situation: it is the same keyboard behaving
differently for a reason the user cannot see.

**The mechanism was a table pointing the wrong way.** `input.h` declared
evdev the canonical event -- correctly, since that is what virtio-input
and a USB keyboard report natively -- but `/usr/share/kbs/*` was keyed on AT
set-1 SCANCODES. So the input core had to translate evdev DOWN into a
legacy encoding for every non-PS/2 device, through a hand-kept list. The
direct range stopped at 83 and the extended table only held 0xE0-prefixed
keys, so keycode 86 fell between them and was dropped.

**Filling the hole was not the fix.** A hand-kept table in the wrong
direction grows another hole the next time a device reports a key nobody
tried; the parity check that would catch it is a guard on a design that
did not need guarding. So the layout was re-keyed on evdev and the table
DELETED.

**What Linux does, which is what this now is.** `atkbd` translates AT set
1 into evdev keycodes and `hid-input` translates HID usages into them;
above that, everything -- the console keymap, XKB -- is keyed on
keycodes. There is exactly one translation, at the very bottom, inside
the LEGACY driver. toy-os had it inverted. Now `keyboard_feed_byte()` is
the only place in the kernel an AT scancode exists, and
`keyboard_key_event(keycode, down)` is what every driver reaches --
`input_report_key()` calls it with nothing in between.

**Why the data did not change.** evdev's numbering was taken from AT set
1, so the two agree for the whole primary block (KEY_1 = 2 = 0x02, up to
KEY_F12 = 88 = 0x58). Every value in `/usr/share/kbs` stayed the same; only the
key names did (`sc_2a=` became `kc_42=`, hex to decimal). That
coincidence is exactly why the old naming looked right for years, and
why the hole was invisible until a device reported a key from the part of
the range where the coincidence stops mattering.

**Two consequences worth stating.** `tools/gen_kbs.py` got SIMPLER -- XKB
keycodes are evdev + 8, so it now emits what it already had instead of
converting down. And a layout file that parses to NOTHING is refused
rather than loaded, because a disk carrying the old `sc_` form would
otherwise produce an empty table and a keyboard that types nothing: a
dead machine, from a file that read perfectly.

**What is still hand-kept, and why that is the right place for it.** The
PS/2 driver's set-1 -> keycode table. A hole there breaks the LEGACY
path rather than every modern one, and `input_test.c` asserts that every
keycode the active layout maps is producible from some wire byte -- with
a second, named check for `KEY_102ND`, because the general one passes
vacuously on a `us` boot where no layout maps that key.

## The signal restorer is ring 3's code, not a kernel-mapped trampoline page

A handler has to get back into the kernel when it returns, and there are
exactly three places the two instructions that do it can live.

**The one that is dead everywhere:** write them into the signal frame
itself, on the user stack, and point the return address at the stack.
i386 Linux did this until NX made an executable stack unacceptable, and
it is not a candidate here for the same reason -- `nx_test` exists
precisely because this kernel enforces NX on ring-3 data pages.

**The one taken:** ring 3 supplies the address. `struct sigaction`
carries a `restorer`, the kernel pushes it as the handler's return
address, and `userland/rt/sigtramp.c` is the two instructions every
program in this tree links. This is x86-64 Linux exactly -- the kernel
*requires* `SA_RESTORER` there and glibc supplies `__restore_rt`.

**The one not taken:** a kernel-owned read-only executable page mapped
into every address space, holding the same two instructions. That is
i386 Linux's vDSO (`__kernel_sigreturn`), and it is the more defensible
design in the abstract: the kernel never trusts a userland pointer for
control flow, and a binary that linked no C runtime still gets a working
handler.

It was not taken because the second half of that argument is worth
nothing here and the first half is affordable. Every ring-3 program in
this tree starts through `crt0.o` and links `libsys` -- that is what
`docs/conventions/build.md`'s "every ring-3 program is just a `main()`"
means, and there is no dynamic linker, no foreign toolchain and no
third-party binary for the "linked no C runtime" case to describe. So
the vDSO would buy a guarantee against a hazard that cannot arise, and
charge a page of address space, a slot in `kernel/uaddr.h`'s map, and a
kernel-owned executable mapping in every process for it.

**What is actually given up, stated rather than glossed:** the kernel
takes an address from ring 3 and makes it a return address. Three things
bound it. The restorer is only ever *pushed as data onto the user
stack*, never jumped to by the kernel -- the ring-3 `ret` is what
transfers to it, in ring 3, with ring-3 privileges. `SYS_SIGACTION`
refuses a handler with a null restorer outright rather than substituting
one, so the failure mode is an error at install time and not a fault on
the way out of something that otherwise worked. And a program that
supplies a *bad* restorer faults in ring 3, which is where a program's
own mistakes belong. There is no privilege to gain: the process could
already jump anywhere in its own address space.

## A signal wakes a blocked syscall by REWINDING it, not by failing it

Stage 1 woke a process parked in a blocking syscall by writing `-EINTR`
into its saved RAX and letting the call return. That is correct as far
as it goes -- the process reaches a delivery point, which is all the
wake was for -- and it makes user-space handlers impossible to get
right.

The problem is ORDER. With `-EINTR` the sequence is: the call fails,
ring 3 sees the failure, and the handler runs at whatever trap comes
next. POSIX's sequence is the other way round: the handler runs, and
*then* the call reports what happened. Every program that has ever been
written against signals depends on the second one, because the first
leaves nothing to restart -- by the time the handler runs, control has
already left the syscall and gone somewhere the kernel cannot see.

So the wake rewinds RIP over the two bytes of `int $0x80` instead
(`SYSCALL_INSN_LEN`), leaving RAX and the argument registers exactly as
the caller passed them -- nothing has written a return value into that
frame. The process resumes, immediately re-enters the kernel at the same
syscall, and `idt.c`'s syscall-entry check delivers there. That is the
one instant at which both answers are still available: rewind the SAVED
frame once more and the call is made again after the handler returns
(`SA_RESTART`), or write `-EINTR` into it and the call fails after the
handler returns. Either way the handler goes first.

Linux arrives at the same place from the other end: its blocking
primitives return `-ERESTARTSYS` and the signal-delivery code turns that
into a restart or an `EINTR` depending on the action's flags. The
difference is only where the state lives -- Linux keeps it in the
in-flight kernel frame, which this kernel does not have, because
blocking here saves a trapframe and unwinds rather than parking a kernel
stack.

**The cost, stated: a syscall can now run twice.** Once as the attempt
that was interrupted before it did anything, and once after the handler.
That is safe *because* the rewind only happens where the call has not
run -- the vector must say 0x80 and the frame must be one the wake
rewound. A signal that becomes pending while a syscall is mid-flight is
delivered on the way out with the call's real result intact, and nothing
is rewound. Getting that distinction wrong would silently double every
write a signalled process made, which is why the two call sites pass an
explicit `at_syscall_entry` rather than working it out locally.

## Delivering a signal at syscall entry means the syscall must be able to run anyway

`idt.c` checks for a pending signal at `int 0x80` and delivers it
INSTEAD of dispatching the syscall. That is deliberate and stage 1
explains why: it makes the process's own next syscall the delivery
point, which it always reaches, rather than depending on a timer tick.

Handlers made the "instead" load-bearing in a way it had not been. The
check asked `scheduler_signal_pending()`, and `pending` counts signals
that a running handler has BLOCKED. So a handler's own `SYS_SIGRETURN`
matched the check, delivered nothing (the only pending signal was
blocked), and was never dispatched -- the restorer returned from an
`int $0x80` that had done nothing at all and ran into its own `ud2`.
It presented as an Invalid Opcode crash in ring 3 with `RAX=43`, which
is 0x43, which is 67, which is `SYS_SIGRETURN`.

Two changes, and the second is the one worth keeping in mind. Asking
`scheduler_signal_deliverable()` -- which accounts for the mask -- fixes
this bug. Making `signal_deliver_pending()` RETURN whether it acted, and
falling through to the syscall when it did not, makes the whole class
unreachable: any future reason for delivery to decline (an action that
turns out to be SIG_IGN, a process that is no longer current, a frame
that will not fit) now runs the syscall instead of swallowing it.

The general shape is worth naming because it is not specific to signals:
**when a check decides to do A INSTEAD OF B, the check and the doing
must agree about when A is possible.** Two predicates that are nearly
the same -- "something is pending" and "something can be delivered
now" -- were close enough to look interchangeable and differed in
exactly the case that mattered.

## A caught fault is not a crash, and the report says so

A ring-3 exception used to have one outcome: `idt.c` printed a
panic-grade report -- registers, faulting address, symbol name, stack
scan -- and tore the process down. That report is genuinely good and
several bugs in this tree were diagnosed from a pasted copy of it, so
the question when faults became catchable was what to do with it.

The answer is that it depends on whether anybody is listening. A process
with no handler for the signal a fault maps to gets exactly the report
it always did, on exactly the same path -- which is every process in
this tree, since catching `SIGSEGV` is a deliberate act. A process WITH
a handler gets one line naming the signal, the faulting RIP and the
handler, and then runs its handler.

The alternative -- print the full report either way -- was rejected
because a program that catches faults on purpose is not crashing, and a
page of registers per occurrence would make the report useless for the
case it exists to serve. Being able to find the one real crash in a log
is worth more than being able to see every deliberate one.

**Three properties that make this safe rather than merely tidy.** A
fault is delivered SYNCHRONOUSLY, not through the pending set -- the
process is standing on the instruction that caused it and there is
nowhere to defer it to. It is NOT restartable, so a handler that returns
without fixing the cause re-executes the faulting instruction and faults
again, which is correct and is what every Unix does. And a fault INSIDE
its own handler falls through to the teardown, detected by the blocked
mask that entering the handler set -- Linux's `force_sig`, arrived at
for free rather than as a separate counter.

**What a `SIGSEGV` handler still cannot catch here is a stack
overflow**, because there is no `sigaltstack`: the frame is built on the
faulting stack, so a stack that has run out has no room for one and
`frame_fits()` refuses. The process gets the default action, which is
the honest outcome and the same one it had before.

## SIGCHLD is sent on exit only, from one helper both deaths call

`SIGCHLD` had a number, a name and a documented default of "ignore" for
a day before anything sent one. Wiring it up is three lines; the two
decisions worth recording are where the send lives and what it is sent
FOR.

**Where: `notify_parent()` in `kernel/proc/sched_exit.c`, called by both
`scheduler_on_exit()` and `scheduler_kill()`.** The tempting spellings
were the two obvious ones and both are worse. Putting it in the CALLERS
-- `sys_exit`, the signal-termination path, Task Manager's force quit --
spreads one fact over three files and makes a fourth kind of death
silently miss it. Putting it in `scheduler_on_exit()` alone is the
mistake this file has already made once: killing a process freed none of
its memory for months, because the teardown lived only in the exit path
and nobody noticed the kill path was a second one. So the rule is the
one that survives a future edit rather than the one that is shortest
today: **the notification is one function and every death calls it.**
Linux funnels identically, through `exit_notify()` ->
`do_notify_parent()`; the difference is that Linux has one `do_exit()`
and toy-os has two entry points, which is exactly why the helper is
worth its name.

The helper does two things that look like one and are not. The **wake**
releases a parent parked in `SYS_WAITPID` on that child's channel --
the synchronous half, and what every waiter in this tree has used since
blocking landed. The **signal** reaches a parent that is not in
`waitpid` at all, which is the case a shell sitting at an idle prompt is
in. Neither substitutes for the other.

**What for: exactly one consumer, and it is `/bin/tosh`.** A shell
reports `[1]+ Done` at a prompt and nowhere else -- bash does the same,
because a report landing mid-command or halfway through a typed line is
worse than a late one. What that leaves is the idle case: the only thing
that produced a prompt was a KEYSTROKE, so a background job that
finished while nobody was typing sat unreported and unreaped until the
next Enter. `SIGCHLD` without `SA_RESTART` makes the shell's blocking
read fail with `EINTR`, and that is the whole feature. It is also why
the flag is tested BEFORE the read as well as after it -- and why that
test-then-block is not the race `pselect()` exists to close on Unix:
this kernel delivers at SYSCALL ENTRY, so a signal arriving in the
window comes straight back as `EINTR` instead of being swallowed by a
read that already parked.

**NOT sent for a stop or a continue, which is a deliberate difference
from POSIX** (which sends `SIGCHLD` for those too, absent
`SA_NOCLDSTOP`). A stop is already reported to a waiter that asked, as
`SIGNAL_STOP_BASE + sig` through `SYS_WUNTRACED`, and that waiter is the
only consumer there is. The asynchronous route would have to raise a
pending bit from the keyboard IRQ that delivers `Ctrl-Z` -- safe, since
raising only flips state, but real machinery for a fact nothing reads.
The bar this project sets is a second REAL caller, not a plausible one,
and there is not one. Revisit if something ever needs to hear about a
suspension without asking for it.

**AND THE BUG THIS EXPOSED IS THE GENERAL LESSON.**
`signal_send()`'s branch for "the default action is ignore" tested
`!scheduler_signal_ignored(pid, sig)` -- the exact inverse of what it
meant. It dropped the signal for a process that had installed a HANDLER
and let one through for a process that had explicitly set `SIG_IGN`
(where `scheduler_signal_raise()` dropped it anyway, so that half was
merely wasted work). `SIGCHLD` is the ONLY signal that can reach that
branch: stop and continue return above it and everything else
terminates. With nothing sending a `SIGCHLD`, the branch had no callers
at all -- so it read as tested code, sat inside a file with KTESTs
either side of it, and was wrong. **A branch only one caller can reach,
with no caller, is untested code that looks tested.** The first check
that asked a handler to run found it in one run; the fix is to ask
whether there is a HANDLER (`SIG_IS_HANDLER` on the action) rather than
whether the signal is ignored.

## The heap starts where the image ends, so a ring-3 program has no size limit

`UADDR_HEAP_BASE` used to be a fixed `0x8000100000` -- one MiB above the
image base -- and `userland/rt/link.ld` carried an `ASSERT` refusing any
binary whose sections reached it. That made a memory-map constant into
**a hard ceiling on how big a ring-3 program was allowed to be**, and it
was not a soft one: the linker was the only thing that knew how big the
image had got, so the failure was a link error with a message telling
you to raise a kernel constant.

One MiB is small. The measurement that made this urgent: the 83
translation units of a `doomgeneric` port compile to ~419 KB of text,
~80 KB of data and ~271 KB of bss -- 770 KB before this project's own
libc and toolkit are linked in. It would have fitted, with perhaps
150 KB to spare, which is not a margin anybody should be spending
thought on.

**The obvious fix is to raise the constant, and it is worth saying why
that was not done.** It would have worked -- address space below the
heap is free, nothing else lives there, and moving the base to 256 MiB
costs a heap span that is reserved rather than mapped, so the cost is
literally zero bytes of memory. What it does not do is stop anybody
having to think about it again. A constant that has already been raised
twice (this one moved with the stack top in M41 stage 4b, and the stack
pages went 1 -> 4 separately) is a constant that will be raised a third
time.

So the base is DERIVED instead. `elf_load()` reports the page-aligned
end of the highest `PT_LOAD` segment (`out_image_end`), and both loaders
arm the process's heap there -- `struct sched_mm.heap_base`, per
process, because it is a property of the binary and not of the map.
**This is what Linux does**: `fs/binfmt_elf.c`'s `set_brk()` sets
`mm->start_brk` and `mm->brk` from the end of the data segment, with
ASLR adding a randomised gap on top. There is no equivalent constant in
Linux to raise, which is the property worth copying.

Three consequences worth knowing:

- **`elf.c`'s bound moved rather than disappeared.** `ELF_IMAGE_END` is
  `UADDR_GUARD_BASE` now, not the heap base -- the loader still refuses
  a segment claiming an address the stack or its guard will occupy,
  because segments are mapped BEFORE the stack and a greedy one would
  be silently replaced by it. What is gone is the collision with the
  heap, because there is no longer a fixed heap address to collide
  with. An image that ran all the way to the guard would leave its
  process no heap at all and every `sbrk` would refuse -- a useless
  binary rather than an unsafe one, and the file's own doing.
- **The image end must be the MAXIMUM over segments, not the last
  one's.** Program headers are not required to be in address order, and
  a loader taking the last header's end would start the heap underneath
  a segment it had just mapped: no fault, corruption on the first
  `malloc`. `kernel/proc/elf_test.c` asserts this with a fixture whose
  low segment is declared last, and a positive control (making the last
  segment win) reddens exactly that check.
- **`UADDR_HEAP_MIN_BASE` survives as a FLOOR**, not a base. It is
  where the legacy `elf_run.c` loader starts a heap when it has no
  image end to derive one from, and the value a corrupt ELF cannot push
  a heap below.

`userland/tests/bigimage_test.c` is the proof: 4 MiB of `.bss`, four
times the old ceiling, which under the old `link.ld` **would not have
linked at all**. It fills every page with an address-derived pattern,
mallocs a megabyte, writes it, and re-reads the array -- the aliasing
the ASSERT existed to prevent, now prevented by arithmetic instead of
by refusal.

## The user stack is reserved and grown on fault, not allocated bigger

The user stack was one 4 KiB page, then four, and the comment above the
constant said plainly that four "is not a considered maximum" and that
it "should be REPLACED rather than raised again when a client outgrows
it". This is that replacement.

**Why not simply raise the page count, which is one line.** Because the
loader maps stack pages EAGERLY -- a `pmm_alloc_frame()` per page in
both `sched_fork.c` and `elf_run.c` -- so the count is not a limit, it is
a per-process tax. A 1 MiB stack would be a megabyte of physical memory
handed to every process at spawn whether it recursed or not, and this
system runs a desktop, a taskbar, a handful of clients and an init: the
cost scales with the process count and buys nothing for the processes
that never go deep.

So: **reserve, and commit on touch.** `UADDR_STACK_MAX_PAGES` is 2048
(8 MiB, deliberately Linux's default `RLIMIT_STACK` -- a number chosen
because it is what every C program has been tested against, not because
anything here measured it). The loader maps `UADDR_STACK_INIT_PAGES`, 4,
which is a starting working set rather than a limit, so the common
client never takes a growth fault at all. Everything below arrives
through `uheap_fault()`, the hook that already existed for the heap.
This is Linux's `expand_downwards()` in miniature.

**The rule that makes it safe is the GAP, and it is the only interesting
decision here.** A fault inside the reservation but more than
`UADDR_STACK_GROW_GAP` (64 KiB -- Linux's own constant) below the mapped
bottom is REFUSED, logged, and left fatal. Without that rule an 8 MiB
window would answer any wild pointer with memory instead of a fault
report, which is strictly worse than the four-page stack it replaced.
With it, the properties are:

- a function opening a frame extends the stack;
- a pointer aimed megabytes below the stack still faults, as it always
  did;
- a single frame LARGER than the gap dies rather than growing -- which
  is the Stack Clash shape (CVE-2017-1000364) seen from the inside, and
  is why `-Wframe-larger-than=2048` in `USERLAND_CFLAGS` is half of this
  guarantee rather than an unrelated warning. `kernel/proc/uaddr_test.c`
  asserts the two numbers stay on the right side of each other, because
  raising the frame limit past the gap is the edit that would quietly
  cash this in.

**Three things had to move with it, and each was a real bug avoided:**

- **`signal.c`'s `frame_fits()` tests the FLOOR, not the bottom.** It
  used to check a signal frame landed at or above `UADDR_STACK_BOTTOM`.
  With a moving bottom that would refuse a legal frame on any process
  that had grown -- the deeper the call chain, the likelier the refusal,
  which is exactly backwards for a signal. A frame landing on a
  reserved-but-unmapped page is fine: `vmm_copy_to_user()` goes through
  the same fault hook on the way in.
- **The guard widened from 1 page to 16.** The old comment asked for
  exactly this if the guard hole ever mattered; a stack that can now
  grow 8 MiB is when it starts to.
- **A refused growth is LOGGED, because the fault report cannot say
  it.** `idt.c` names a fault in the guard as a stack overflow, and a
  refused growth is not in the guard -- it is inside the reservation,
  indistinguishable from an ordinary wild pointer. Without the log the
  two most interesting failures here are both a bare "Page fault".

**Why the stack goes through the same hook as the heap** rather than
being wired into the `#PF` handler: a syscall whose output lands in a
not-yet-grown stack page reaches it through `vmm`'s copy helpers, which
walk page tables rather than dereferencing user addresses and so never
fault at all. A growth path in the fault handler alone would work for
ordinary code and fail for `read(fd, buf, n)` with `buf` a deep local --
which is the same bug the heap had before the hook existed, arriving
from the opposite direction.

`userland/tests/stackgrow_test.c` proves it, and **no KTEST can**:
`uaddr_test.c` asserts the map's arithmetic, and every one of those
assertions passes just as happily on a kernel whose handler grows
nothing. Only a ring-3 process with a deep call chain can show the pages
arriving. It descends ~1.6 MiB in 1 KiB frames, fills each with a
pattern derived from the frame's own ADDRESS (a constant fill cannot
tell a working stack from two depths sharing one physical frame -- both
read back the constant), and verifies every frame on the way back OUT,
which is the half a growth bug breaks. With `grow_stack()` disabled it
faults at `0x807fefcff8`, the first byte below the four mapped pages.

## The keyboard tap is off by default, and `kbd` never reads a key

`/bin/kbd` prints every stage of a keypress at once -- scancode,
keycode, character, modifiers. Two things about how it gets them were
real forks.

**Why it is OFF by default, and why that REVERSES what this entry first
said.** The original design recorded unconditionally, and the argument
was good: the question people actually have is *"what did the key I just
pressed do?"*, asked after it did the wrong thing, and an arm-and-drain
tap can only ever watch keys pressed from now on -- so every use of it
begins by reproducing the bug with the tool already open, and an
intermittent one may not oblige. `dmesg` makes exactly that trade.

What the argument left out is that **a ring holding the last ~128
keystrokes is a keylogger by any honest description, and this kernel has
no privilege model**: `SYS_QUERY` performs no check of any kind, so while
the tap is on any ring-3 process can read what was typed, including at a
prompt. For a single-user hobby OS the practical risk is small. The
posture is not small: "kernel keystroke buffer, enabled out of the box"
is not a property to ship in a system anybody else might run or read the
source of, and what it buys is a debugging shortcut on a machine you are
already sitting at. `dmesg` can make the opposite trade because a kernel
log line is not a record of what somebody typed.

So `kernel.kbdtap` is an ordinary tunable beside `kernel.kstack_track`,
off out of the box, persisted to `/etc`. Two details carry the weight.
**Disabling WIPES the ring** -- a switch that stops new records while
leaving the last hundred keystrokes readable is decorative, so off means
there are none in kernel memory; enabling wipes too, so a session starts
clean. The sequence numbers deliberately survive, being a counter rather
than data: restarting them would let a reader see a number it had
already seen, which is the one thing `seq` exists to prevent. And
**`/bin/kbd`'s live mode arms the tap for its own duration**, disarming
on every way out, so the common case is still one command -- while a tap
somebody deliberately left ON is left alone, since disarming it would
silently undo their choice and discard the history they were keeping.

The cost, stated rather than glossed: with the tap off, `kbd --last`
cannot explain a key that already misbehaved. That was the whole
argument for the original default, and it is the price of this one.

**The gate is proved by a KTEST, not by the GUI tool, and finding that
out cost a positive control.** `/bin/kbd --last` originally checked the
switch and returned before reading the ring, which reads as obviously
correct -- and made the tool structurally unable to notice the single
failure that matters: a tap recording while reporting itself off.
Deleting the gate from the kernel changed nothing it printed, so every
check written against the tool stayed green while the in-kernel KTEST
went red on exactly the right assertion. `--last` reads the ring FIRST
now and reports a non-empty ring under an "off" switch as a loud
anomaly, which is both better behaviour and what makes a test of it
discriminating. **Generalises: a tool that consults a flag before
looking at the thing the flag describes cannot check the flag.**

**Linux is not a precedent for keeping a history, and an earlier version
of this entry wrongly said it was.** Linux keeps none: evdev allocates
its ring buffer per OPEN CLIENT in `evdev_open()`, so with nobody
holding `/dev/input/eventN` the client list is empty and nothing is
retained -- which is exactly why `evtest` and `showkey` can only ever
watch keys pressed after they start. The nearest thing the input core
keeps is a current-state bitmap (`EVIOCGKEY`: which keys are down right
now), and the tty keeps queued input for a reader; both are state, not
history. Its reason is scale rather than privacy -- a ring per device
per client -- but the posture that falls out is the same one toy-os has
now landed on. Copy the shape, not the size -- and **check the claim**,
which is the rule this entry briefly broke, and which is most tempting
to skip exactly when the claim supports the conclusion already reached.

**Why it is `evtest`'s shape and not `showkey`'s.** Linux ships both,
because there are two questions. `showkey` puts the console keyboard
into `K_RAW`/`K_MEDIUMRAW` and prints what arrives -- it takes the
keyboard, works only on the console, and needs a keyboard MODE to
switch. `evtest` reads the input core's own events from a device node:
non-exclusive, sees everything regardless of focus, and changes nothing.
toy-os has no keyboard modes to add and no device nodes to read, but it
does have one confluence point every driver passes through
(`keyboard_key_event()`) and a registry for facts, so the log is a
`SYS_QUERY` class. That choice is what makes `kbd` usable inside a
Terminal window while the compositor owns the keyboard -- it is reading
a record, not competing for a queue.

**A version of it did read fd 0, to tidy up.** Keys pressed during a
live session stay queued for whoever reads fd 0 next, so the shell gets
them when its prompt comes back; draining them at exit seemed polite.
It was wrong twice. fd 0's non-blocking flag lives on the DESCRIPTION,
which is shared with that shell, so a tool killed mid-drain would hand
the shell a broken descriptor. And a console read PARKS while a
compositor owns the keyboard whether or not the descriptor says
non-blocking -- so the tidy-up hung the tool on precisely the ordinary
graphical boot it is most useful on. Not draining is also what `evtest`
does, and what any command that ignores stdin does.

**And live mode refuses the legacy `run` loader by name.** There is no
scheduler slot there, so `SYS_SLEEP` is refused (`-EPERM`) and -- the
part that is not written down anywhere else -- the monotonic clock never
advances either, because that context does not reach a timer tick. A
poll loop with a deadline then spins at full speed against a deadline
that can never arrive and takes the machine with it. `/bin/less` carries
the same hazard as prose in a comment; prose is not enough when the
failure mode is a dead machine rather than a slow pager, so this one is
a guard: `sys_sleep_ms(0) < 0` means "no slot", and it says so and
exits. It also moved the idle timeout from counting polls to reading the
clock, since a poll count silently assumes the sleep between polls works
and takes the length it asked for.

## Every key on the keyboard reports something now, and the keypad reports characters

The `KEY_*` vocabulary grew one code per caller, which is a reasonable
way to start and leaves a bad end state: pressing **Insert, the Menu
key, Caps/Num/Scroll Lock, Pause, Print Screen or anything on the
numeric keypad produced nothing at all.** Not an unknown code -- nothing.
The layout had no entry, `keyboard_layout_translate()` returned 0, and
the key was indistinguishable from one nobody pressed.

That is a bad property for an input layer specifically: **an app cannot
bind what it never sees**, and "does this keyboard work?" had no answer
for about a third of the keys on it. Found by porting Doom, which binds
F1 through F11 -- of which this kernel emitted F2, F3, F4 and F10.

Four decisions inside it:

- **The function row is complete, F1-F12.** Half a row is worse than
  none: F6 and F9 are quicksave and quickload, the two anybody actually
  reaches for.
- **The keypad emits CHARACTERS, not codes.** Its whole purpose is
  typing numbers, and an app that had to learn twelve new `KEY_*` values
  to receive a `7` would be the wrong shape. Keypad Enter sends the same
  `\n` the main Enter does, as on every OS.
- **NumLock's off-state is deliberately not modelled.** On real hardware
  NumLock off turns the keypad into a second set of arrows, and the
  failure mode of getting it wrong is a keypad that types nothing while
  the light says otherwise. Always-numeric is what a keypad is for; the
  arrows are a few inches to the left.
- **Caps Lock is the one lock STATE kept** (since 2026-09-29; it
  reported its press and changed nothing before). It inverts Shift on an
  "alphabetic" key -- xkb's rule: unshifted a lowercase letter, shifted
  its capital -- decided from the layout, so digits and punctuation are
  untouched and Caps+Shift types lowercase. Toggled on the press and not
  on a typematic repeat. The PS/2 keyboard's light follows through
  `input_source.set_leds` (0xED and the LED byte, advanced by the ACKs in
  the interrupt, as Linux's atkbd does); a USB keyboard's does not yet.
  Scroll Lock still reports its press and changes nothing.

Two scancode wrinkles worth knowing, both in `keyboard_feed_byte()`:

- **Pause is six bytes and has no break code, so the parser reports its
  release.** `E1 1D 45 E1 9D C5`, and nothing else uses the `E1` prefix
  -- so the press is reported when the prefix arrives, the release at
  once after it (e08a7073; a press with no release had cost a reserved
  slot in the key streams and a stuck key in the positional one), and
  the five bytes behind it are counted out.
- **The fake shifts around Print Screen are dropped.** A PS/2 keyboard
  brackets PrtSc with `E0 2A` / `E0 AA` so a DOS-era reader saw a
  shifted key. Taking those at face value would report a Shift nobody
  pressed -- and leave `shift_pressed` stuck on if the release half were
  ever missed.

### And the waiver that had never worked

Growing that switch past twenty branches made `tools/check_dispatch.py`
fail -- on a construct whose author had already waived it, inline:
`switch (keycode) { // dispatch-ok: ...`. The checker looked only at the
lines ABOVE the construct, so a trailing comment on the switch's own line
could never be seen. Nothing had failed before because both constructs
written that way were under the branch limit; the first one to grow past
it reported a chain that had been "waived" for months.

The checker accepts the trailing form now. **A waiver mechanism that
silently does not waive is worse than none**: it reads as protection at
the call site and provides none, and the moment it matters is exactly
the moment somebody is busy with something else.

## Readiness is a syscall the kernel does nothing with, and the barrier always expires

`After=` in `/etc/services.d` ordered SPAWNS: a service was "started"
the instant `sys_spawn()` returned a pid. That is systemd's
`Type=simple`, and it is an honest description of a fire-and-forget
spawn -- but it is not what the key usually wants to mean. The desktop
is spawned in about a millisecond and is not usable for another three
hundred; in between it takes the framebuffer grant, initialises the
font, loads the cursor theme, decodes a wallpaper and reads fourteen
desktop entries. Nothing ordered after it could tell that window from a
working desktop.

**The transport, and why the two obvious ones do not port.** systemd's
`Type=notify` has the service send `READY=1` to an `AF_UNIX` datagram
socket named in `$NOTIFY_SOCKET`, and attributes it to a sender with
`SO_PASSCRED`. s6 strips that to a byte written to an inherited fd
(`notification-fd`). Neither works here. There are no unix sockets, so
`$NOTIFY_SOCKET` has nothing to name. And a pipe is worse than it looks:
`PIPE_MAX` is 8 kernel-wide and shared with every shell pipeline, so one
held open for a whole boot is an eighth of the supply -- and a pipe
carries no credentials, so with one shared channel the child would have
to declare its own pid and be believed. Passing a per-service pipe would
also mean extending `SYS_SPAWN` to hand a child a third descriptor,
which today inherits only 0/1/2.

So the service calls the manager instead, which is Windows' shape:
`SetServiceStatus(SERVICE_RUNNING)`, with the SCM making services listed
in `lpDependencies` wait for RUNNING. `SYS_NOTIFY_READY` takes no
arguments, because **the caller's identity is the entire message** --
and having the kernel supply it is exactly what a pipe could not do.

**What the kernel does with it: nothing.** It sets a bit in the process
slot, reports it through `SYS_PROC_INFO`, and never reads it again.
Nothing waits on it, wakes on it, or schedules differently because of
it. That is the line that keeps a service-manager concept out of the
scheduler: init decides what readiness is worth, and the kernel stores
an announcement the way it already stores a name and a group. The cost
is that init POLLS rather than being woken -- bounded twice, since it
polls only while a notify service is outstanding and each one resolves
within its own `ReadyTimeout=`. An idle machine never reaches that code.

**One bit, and the trade is real.** `sd_notify` also carries `STATUS=`,
`RELOADING=`, `MAINPID=` and a watchdog ping. None has a consumer here,
and this project's bar is a second real caller -- but a richer protocol
later means a different channel, not a longer message. Said plainly
rather than left to be discovered.

**The barrier always expires, and that matters more than the barrier.**
`ReadyTimeout=` defaults to five seconds; when it runs out init logs a
line naming the service and starts the dependents anyway. systemd waits
90 s (`TimeoutStartSec`) and then KILLS the unit, failing its
dependents. Both differ here deliberately. Ninety seconds of a black
screen is not a diagnosis anybody waits for on a machine with one
console; and killing a service that is merely slow removes the thing the
timeout exists to protect. This is the same rule that makes an ordering
cycle drop an edge, an unknown `Restart=` keep the default and a crash
loop give up rather than spin: **no key in a descriptor may be able to
leave the machine with nothing started**, because there is no rescue
target and no journal to read afterwards. systemd can afford the strict
answer; this cannot.

Every "it can never answer now" case releases the barrier for the same
reason -- a service given up on as a crash loop, one whose descriptor
was deleted, one that exited cleanly, and a `Restart=no` service past
its single run. Waiting for any of those is waiting for something that
cannot happen.

**Where the call goes is the service's decision, and is the whole
design.** The kernel cannot know when a process became useful. `toywm`
announces at its first COMPOSITED FRAME rather than at the compositor
claim, and `tosh` at its first prompt rather than at `main()`; both have
an easy wrong answer that looks perfectly reasonable. Calling it from a
program init does not supervise is a no-op, deliberately, so a program
need not know how it was started in order to be correct -- the same
property `sd_notify()` has in a process launched from a shell.

**One implementation note that bit during the build.** init computed
"how many services am I still waiting on" BEFORE starting the services
in that pass, so a service started on the pass got counted as
not-waited-on, init blocked in `waitpid(-1)`, and nothing could ever
wake it -- a readiness bit sets no channel and produces no event. The
desktop came up perfectly and init never noticed. The count is taken
after `start_due()` now. The general shape: when a new state is entered
by an action in the same pass, anything derived from that state has to
be recomputed after the action, not before it.

## The kernel log leaves ring 0 as byte slices with absolute offsets, not as lines or a device

`dmesg` was a ring-0 shell builtin long after `ps`, `meminfo`, `kstack`,
`heap`, `ata` and `parttable` had become `/bin` programs, and not
because nobody got to it: the log had no way out of the kernel.
`klog_dump()` streams into a callback, which is the right shape for a
console pager and the wrong shape for a syscall, which must fill a
caller's buffer and return. So the command stayed in ring 0, and at a
`$` prompt — a Terminal window, the one place a person actually reads a
log — it resolved to a `/bin` lookup, found nothing, and failed.

**Why a query class and not a device.** Linux exposes the log as
`/dev/kmsg`: a character device, one record per `read()`, each carrying
a sequence number so a reader can tell that records aged out. FreeBSD
exposes the same ring through the `kern.msgbuf` **sysctl** — its general
introspection registry — instead. The second shape is the one that ports
here. There is no mount table, and `docs/query-design.md` is explicit
that `SYS_QUERY` is deliberately not `/proc`; a device node would need
infrastructure that does not exist in order to buy nothing the registry
does not already give. `api/query.h`'s opening argument applies directly:
a syscall per information class grows the syscall number by one per fact
and leaves nothing able to answer "what facts exist?".

**Why bytes and not lines.** A record per line is tidier to describe and
is what `/dev/kmsg` does. It would also cost a scan of the ring per
record — `klog.c` stores bytes and a line has no index, so filling record
N means finding the Nth newline: O(n) each, O(n²) to walk. And a line
longer than a record would still have to be split, so the tidiness would
not even be complete. A reader writing bytes to stdout does not care
where the boundaries fall.

**The absolute offset is the whole design.** Every slice reports where
its first byte came from, counted from the first byte ever logged rather
than from a position in the ring. The ring overwrites its oldest bytes
while a reader walks it, so a reader indexing by ring position would
silently re-read or skip whatever moved — and the result would look like
a valid log. With absolute offsets, a gap between one slice's end and
the next one's start is detectable, and `/bin/dmesg` prints it where it
happened. That is what `/dev/kmsg`'s sequence numbers are for, and Linux
prints a `-` for the same event. A log that silently splices two eras
together is worse than one with a hole in it, because only the second
kind can be noticed.

**The ring-0 copy became `rescue dmesg`,** which stretches that set's
stated rule. `rescue` is nominally "the commands you would need to put
`/bin` BACK", which is why `strace` went to `/bin` and not there. The
argument for the exception is that `shell_rescue.c`'s own opening says
its job is DIAGNOSIS, and a machine whose `/bin` will not load is exactly
a machine that cannot run `/bin/dmesg` to find out why. It costs one
table row and no second implementation — the ring-0 pager already
existed. It ignores its arguments rather than refusing them, because the
flags belong to the `/bin` program and this is the copy reached for when
`/bin` is what is broken.

**Pagination did not come along.** The builtin drew `-- more --` and
blocked on a keystroke, which meant it could not run inside a GUI
callback and carried a branch to detect that. `dmesg | less` pages it
with code that already works, which is what the rule against a builtin
shadowing a `/bin` program that does more is for. `--follow` polls at
200 ms rather than blocking, because a fact in this registry is computed
on every read and has no stored form to wait on.

## A thread is a slot with a `tgid`, not a second kind of object

The two shapes are both real. Windows NT has `KTHREAD` inside
`EPROCESS`: the process is a container and the thread is what the
scheduler runs, two object types from the first release. Linux has one
`task_struct`, and `clone(CLONE_VM | CLONE_FILES | CLONE_SIGHAND)`
produces another one that happens to share everything — the kernel has
no concept named "thread" at all, and `tgid != pid` is the only thing
that distinguishes one.

toy-os took Linux's. The argument is not elegance, it is the existing
code: `procs[]` is indexed everywhere, a pid was a slot index plus one, a
wait channel is `&procs[i]`, a kernel stack is `kstacks[i]`, and
`SCHED_MAX_PROCS` sizes four unrelated tables (`win_server.c`,
`win_events.c`, `mm_audit.c`, `kstack_query.c`). A separate thread
object would have had to be threaded through every one of those, and
each of them would then need to answer "and what about threads?"
separately. As a slot with one extra field, a thread is schedulable, has
a kernel stack, can be traced and shows up in `ps` with no work at all.

What it costs is honest and worth stating: **a thread consumes a process
slot**, so the process limit is the total across all programs (64 when
written; computed from RAM since 2026-10-02), and a program that creates
a thread per connection would exhaust it. The per-slot cost is a 16 KiB kernel stack plus 512 bytes
of FP state plus a 768-byte signal table.

**The fd table needed no change**, which is the best evidence the shape
was right. It is keyed by CR3 (`syscall_fd.c`), not by pid, so two slots
sharing an address space share their descriptors without a line being
written. That keying was chosen for the legacy loader, which has no pid;
it happened to be the threading-correct answer years early.

**`scheduler_current_pid()` kept its meaning and a second call was
added.** It answers the THREAD, which is what every existing caller
wanted (a wait channel, a kill target, a kernel stack, a trace);
`scheduler_current_tgid()` answers the PROCESS, and the handful of
callers that meant that — a window's owner, a terminal's owner, a
child's parent, `getpid`, `setpgid(0, …)` — were moved one at a time.
The alternative, redefining the old call to mean the process, would have
been a silent change to every one of those call sites in the direction
that fails quietly.

## A process exits as a whole, whichever thread calls exit()

POSIX has two calls: `exit_group(2)` ends every thread, `exit(2)` ends
one. toy-os has the same pair (`SYS_EXIT`, `SYS_THREAD_EXIT`) and the
same default, and here it is not a choice. `syscall_process_exit_cleanup()`
destroys the address space, and a surviving thread would be resumed into
unmapped memory a few instructions later. So `SYS_EXIT` releases the
group; a fault does too (the same path); and `SYS_KILL` naming any tid
redirects to the leader rather than tearing down an address space its
siblings are standing in. A `tkill(2)` — signal one thread specifically —
is the call that would behave differently, and there is none.

**The leader calling `pthread_exit()` exits the process**, where POSIX
keeps the process alive until the last thread leaves. Honouring that
means a zombie leader holding the tgid while its siblings run, and
nothing in this kernel can wait for "the group is empty" — the leader's
slot is what `waitpid()` names and what `scheduler_poll()` reaps. The
divergence is documented in `<pthread.h>` and in the ABI, because it is
the kind of thing a ported program discovers at the worst moment.

**Threads are excluded from every child walk.** `waitpid()` handing a
process one of its own threads would be a corpse the program never
created; Linux spells the exclusion `__WNOTHREAD`. A thread's `ppid`
still names its leader, so `ps --tree` nests it — a display fact, never a
wait one, and the three walks that had to learn the difference are
`scheduler_poll_any()`, `reparent_children()` and
`scheduler_stop_report_any()`.

## The kernel does not allocate thread stacks

`SYS_THREAD_CREATE` takes an entry point and a stack top and allocates
nothing. That is `clone(2)`'s shape; `pthread_create()`'s — where the
library picks a size, maps a region and installs a guard page — lives in
`userland/libc/pthread.c`.

The reason is that none of that work needs a privilege ring 3 lacks. A
thread stack is ordinary process memory, `malloc` already exists in ring
3 (the kernel's own allocator, compiled twice), and the heap is
demand-paged, so a 64 KiB stack costs only the pages actually touched.
Putting the allocation in the kernel would mean a second address-space
region with its own layout rules in `uaddr.h`, a policy about default
sizes, and a way to express "I want a bigger one" through the ABI —
three decisions bought for nothing.

Two consequences that are real limitations rather than tidy trade-offs,
and both are named in `<pthread.h>` rather than left to be discovered:

**A thread stack has no guard page.** The initial thread's is grown on
fault with a guard gap below it, and the kernel's own stacks have guard
pages; a thread's is a malloc'd extent, so an overrun walks into another
allocation instead of faulting. Fixing it needs an `mprotect`-shaped
syscall, which this system does not have and which is a roadmap item on
its own merits.

**A detached thread's stack is never reclaimed.** Nothing can safely
free memory a dying thread is still standing on: between "I am about to
exit" and the syscall that ends it, the thread is still pushing. Linux
solves it with `CLONE_CHILD_CLEARTID` — the kernel zeroes a word in the
dying thread's memory and futex-wakes whoever is watching, *after* the
stack is no longer in use. That needs a futex, which is the same missing
primitive the mutex is spinning around, so both are one roadmap item.
Joining a thread reclaims everything; the leak is the price of not
joining, and it ends at process exit.

## errno became thread-local before anything could see it be wrong

`userland/rt/sys.c`'s errno had carried a comment for months saying it
was the variable that would need TLS once two threads could be in a
syscall at once. Making it `__thread` in the same change as threads was
deliberate rather than tidy: a shared errno is invisible to every test
that does not deliberately fail a call in two threads and compare, so it
would have shipped working-looking and broken. The thread test asserts
exactly that pair, and it is the only check in the file that a shared
errno fails.

Doing it required the whole TLS mechanism — a PT_TLS segment, a linker
script that keeps `.tdata`/`.tbss`, a block installed before `main()`,
and a kernel that reloads FS.base on every switch — which is why TLS is
not a separate later milestone. There is no half of it that is useful
alone: `%fs` with nothing behind it is a fault, and a thread with a
shared errno is a bug generator.

**The layout is the psABI's variant II**, with the thread pointer at the
END of the block and variables at negative offsets. The size the runtime
allocates must be `memsz` rounded up to the SEGMENT's own alignment,
because that is the number the linker subtracted when it resolved every
`%fs:offset`; rounding to a convenient 16 instead shifts the whole block
under the offsets reading it, and nothing fails loudly.

**GCC does not believe a linker symbol's address is data.** The address
of a declared object cannot be null, so `for (i = 0; i < (size_t)__tls_filesz; i++)`
compiles bottom-tested and a `filesz` of 0 counts to 2^64. It was a page
fault in every ring-3 program, a few thousand bytes past the buffer, on
the first build that had TLS in it. `linker_value()` laundered the number
through an empty `asm` and emitted no instruction -- until dynlink
Stage 1 (2026-08-28) retired the whole mechanism: `-fpie` cannot reach
an *ABS* symbol RIP-relatively at all, so the geometry became three
QUADs `link.ld` writes into `.rodata` (`__rt_tlsdesc`), read as an
ordinary struct. A bound loaded from memory is data GCC believes, so
the laundering went with the symbols.

## A service is controlled by a file and a doorbell, and init cannot be killed by a signal it has not caught

Two things landed together because neither works without the other: a
`service start|stop|status|list` command, and the discovery -- made
while triaging why `tools/init_test.py` failed 12 of its 27 checks --
that `kill 1` had been killing init since signals landed.

### The control channel

**What real systems do.** `systemctl` talks to pid 1 over D-Bus, or its
private `AF_UNIX` socket at `/run/systemd/private`. SysV `telinit`
writes a fixed-size record into the `/run/initctl` FIFO. runit's `sv`
and s6's `s6-svc` write a command byte into a FIFO inside the service's
own supervise directory. Windows' SCM is reached by RPC.

**None of the transports port, and the same two facts kill all four.**
There are no unix sockets here, and no named pipes: `PIPE_MAX` is 8
kernel-wide and a pipe has to be inherited, so there is nothing an
unrelated program started minutes later could open. What is left is the
filesystem and signals -- so the request is a FILE and the signal is a
DOORBELL, which is runit's object plus SysV's `kill -HUP 1`.

`/bin/service` appends `<verb> <name>` to `/run/init.ctl` and sends
`SIGHUP`; init reads every line on its next pass, acts, and deletes the
file. **The signal cannot be the message** -- it carries no payload, and
a handler may do nothing but set a flag -- and the file cannot be the
signal, because with a service running init BLOCKS in `waitpid(-1)` and
would not read it until something died. Each half is doing the thing the
other cannot.

**The answer comes back the same way, as `/run/init.status`** -- one line
per service. init is the only thing that knows a service is down on
purpose: the process table shows an absence, and an absence cannot tell
`stopped` from `crash-loop` from "never declared". runit writes the same
file per service. This one is text rather than runit's packed binary, so
`cat /run/init.status` works on a machine whose `/bin` is damaged, which
is exactly when somebody wants it.

**It is published ON DEMAND and only when the machine is SETTLED, which
is two gates and both earn their place.** Nothing is written until a
doorbell has arrived, because there is no tmpfs here and every write is
a real disk transaction -- so `/bin/service`'s read half rings the bell
itself when it finds no file, and every later read finds one already
current. And nothing is written on a pass with a service in a restart
backoff or yet to announce itself, which is the honest description of a
file that says where things CAME TO REST.

The second gate was also forced. init writing one ~120-byte file at
0.8 s WEDGED THE COMPOSITOR -- it presented nothing at all, cursor
included, while `ps` still showed the desktop ready and accruing CPU
(3 runs in 3; a write at 1.5 s is harmless). That is a compositor bug
and it is filed with its measurement in `docs/bugs.md`; deferring past
the startup is not a fix for it, and the entry says so. What made the
deferral the right call anyway is that it is what the file MEANS: the
same gate covers a desktop restarted with `service start`, which lands
in the same window.

**Both live in `/tmp` because that is the only runtime directory here.**
On a real system they would be under `/run`, whose one relevant property
-- it is a tmpfs, so it is empty at boot -- `/tmp` does not have
(`docs/filesystem-layout.md` says it is not emptied). So init deletes
the control file at startup: a request left behind by a machine that
lost power is not a request, and obeying it would be a service stopping
itself on the next boot for no visible reason.

**An admin stop is its own flag, not the existing `stopped`.** A stopped
service is `SIGTERM`ed and therefore exits with `128 + SIGTERM`, which
`Restart=on-failure` and `Restart=always` both read as a failure to
recover from -- so reusing the "exited cleanly, stay down" flag would
have put the desktop straight back. The stop outranks `Restart=`
entirely, which is systemd's behaviour and the only one that makes
`service stop` mean anything.

**`sys_waitpid()` had to gain an interruptible sibling.** The wrapper
retries `-EINTR` on purpose -- a shell woken by its own child's SIGCHLD
must not have its wait fail underneath it -- which meant init went
straight back to sleep with the flag its handler had just set unread,
and `service stop` did nothing at all with no error anywhere.
`sys_waitpid_intr()` is a separate entry point rather than a flag, for
the reason `sys_waitpid_untraced()` is one: it returns something the
existing callers are written not to expect.

### Why `kill 1` worked, and what fixes it

`scheduler_kill()` has refused pid 1 since init existed. Signals then
added a SECOND way to terminate a process, and it does not go through
that function: when the victim is the process about to be resumed,
`do_default_action()` takes SYS_EXIT's path --
`syscall_process_exit_cleanup()` then `scheduler_on_exit()` -- which
never asks whose pid it is. So `/bin/kill 1` sent a SIGTERM that init
had no handler for, and init died. The desktop was reparented to the
kernel, orphans stopped being reaped, and nothing said anything.

The guard goes in `do_default_action()`, which is the one place the
default action is decided and therefore covers both branches. That is
Linux's `SIGNAL_UNKILLABLE`, and it keeps Linux's split: init still
CATCHES what it has installed a handler for -- which is what the
doorbell depends on -- and simply cannot die of what it has not. A fault
in init is unaffected, because a fault reaches ring 3 through
`signal_deliver_fault()` and never through the default action, which is
also what `force_sig()` arranges on Linux and for the same reason: a
pid 1 that segfaults should die and say so, not loop.

**The lesson is the one this tree keeps relearning**: when a subsystem
gains a second implementation, re-read every guard named after the
first. The invariant was tested -- `tools/init_test.py` had a `kill 1`
check -- but it asserted `1 in ps`, and a zombie passes that.

## mmap is a fixed region list, its files are remembered by path, and MAP_FIXED refuses overlap

`SYS_MMAP` (`kernel/mm/mmap.c`, 2026-08-28) is Stage 0 of
`docs/dynlink-design.md`, built the way it is for four reasons a future
session will want re-litigated.

**The metadata is a fixed 16-slot array embedded in `struct sched_mm`,
not an allocated list.** An allocated list needs a teardown path, and
this kernel has TWO of those (exit and kill) with a history of one
forgetting what the other freed. Embedded metadata dies with the slot
by construction, and the frames behind the mappings are ordinary owned
user pages the address-space walk already frees. Sixteen regions is
plenty for the dynamic loader this exists for (an executable's
libraries are a handful of segments each); raising it is one constant.

**The arena is its own range (`UADDR_MMAP_BASE`, 32 GiB above the
window regions), not holes carved between existing regions.** Every
region below is either per-process-movable (`heap_base`,
`stack_bottom`) or derived and gigabytes wide (the window strides), and
carving between them is how the compositor's back buffer got landed on
twice before. Address space is free; a separate range means nothing in
the existing map moved and the fault classifier tells the two apart
with one range check.

**A file-backed region remembers its file by absolute path, not by
pinning the open file.** POSIX says the mapping survives `close(fd)`,
so the fd cannot be the record. Linux pins the `struct file`; this
kernel's nearest equivalent would be holding an `open_file` slot
hostage to a mapping, invisible to the process's own fd table, for as
long as the region lives -- a lifetime bug shaped exactly like the ones
the fd refcounting was built to end. A path re-resolved per fault costs
a lookup and has one honest failure: a backing file deleted or renamed
makes the next untouched page's fault fatal (logged by name). A page
cache would be the real fix, and is the roadmap's item, not this one's.

**A file-backed fault-in refuses while `scheduler_preempt_depth() > 0`
rather than making the filesystem re-entrant.** The fault path reads
the backing file, and if that ever runs inside an `FS_OP` it re-enters
the backend's module-level scratch buffers -- the recursion the
preemption guard exists around but cannot itself see. No such path
exists today: backends touch only kernel buffers, and every syscall
faults its user ranges in (validation and the copy helpers' hook)
before the backend is called. The refusal turns "no such path exists"
from an accident of current call orders into an enforced invariant: a
future path that violates it gets a named log line and a dead process,
not silent scratch-buffer corruption on a preempted boot in three.

**MAP_FIXED refuses overlap with -EEXIST where POSIX silently
replaces.** Replace-on-overlap is what makes Linux's `mmap` able to
unmap live memory as a side effect of a typo'd address. The one caller
that wants replacement here -- the dynamic loader carving segment
mappings out of a reservation it made moments earlier -- can say so in
two calls (`munmap` then `MAP_FIXED`), which is race-free in a
single-threaded loader running before `main`. If a real port ever
needs POSIX's semantics, widen it then; a refusal can be widened,
a silent unmap cannot be taken back.

## The dynamic loader is a fixed-base ET_EXEC, and the kernel never learns ET_DYN

Dynlink Stage 2 (2026-08-28). Linux's split is copied exactly one
level up from where Linux draws it: the kernel maps the executable,
sees `PT_INTERP`, maps the interpreter and enters it -- but toy-os's
interpreter is an ordinary fixed-base `ET_EXEC` linked at
`ELF_LDSO_BASE` (0x8010000000, 256 MiB above the image base), so the
SAME `elf_load()` loads both images and the kernel contains no ET_DYN
mapping, no base-choosing and no relocation code at all. Shared
LIBRARIES are ET_DYN, and the loader maps them itself with Stage-0
`mmap` -- reserve a span, `munmap`, carve segments in with `MAP_FIXED`.

Why not a relocatable loader like `ld.so`: a loader that relocates
ITSELF runs code whose own globals are wrong until it finishes -- the
classic rtld bootstrap, a class of bug that presents as a crash before
main with no output. A fixed base deletes it for the price of one
reserved range, which `spawn` guards (an executable reaching
0x8010000000 is refused by name -- none comes within two orders of
magnitude). The a.out `ld.so` was fixed-base for the same reason.

Three constraints the build carries so the loader can stay ~400 lines:

- **`--hash-style=sysv`** on libraries AND dynamic executables -- the
  loader's only symbol lookup is the sysv hash, and the exe must be
  lookupable too (a library resolving `dyn_test_callback`, later the
  exe's `__errno_location`, searches the exe FIRST).
- **`-z max-page-size=4096`** on libraries -- segments are mapped
  file-backed, and a 2 MiB-aligned .so's offsets are not congruent to
  its vaddrs under 4 KiB pages. The loader refuses such a file by name
  rather than copying it in.
- **`-mno-direct-extern-access` + `-z nocopyreloc`** -- GCC's -fpie
  otherwise accesses extern data directly and leans on R_X86_64_COPY,
  which this loader deliberately does not implement: the GOT
  indirection is strictly better and the linker relaxes it back to a
  direct `lea` in static links, so static binaries pay nothing.

The auxv is minimal on purpose (AT_PHDR/AT_PHENT/AT_PHNUM/AT_ENTRY,
abi/auxv.h): only a dynamic executable gets one, and a static
program's stack is byte-identical to what it always was. AT_PHDR
points into the mapped image -- link-dyn.ld's first segment carries
FILEHDR PHDRS -- so the kernel copies nothing.

## The userland is dynamically linked, and the three bugs that cost the afternoon

Dynlink Stage 3 (2026-08-28, the maintainer's call): every `/bin` and
GUI program links `/lib/libc.so`. What stays static, and why: **init**
(pid 1 -- the machine must reach a shell with `/lib` broken or
missing), **toywm** (a rescue happens on the desktop), and **`/tests`**
(the harness drives it through `run`, whose exit banner is its entire
assertion -- and `run` IS the legacy loader, which refuses `PT_INTERP`
by design). tolibc's `pthread.c` sits in `libc_nonshared.a` -- glibc's
own shape -- because its `__thread g_self` is TLS a library here may
not carry; `errno` needs no such trick, because `g_errno` lives in
libsys (static in every binary) and libc.so imports the executable's
`__errno_location`.

Flipping ~45 binaries surfaced three real defects, each worth its
lesson:

**The kernel shell's bare name spawns now, and fd inheritance names
its parent.** The `#` console ran `/bin` through the legacy loader, so
the flip broke every `cat` the harness typed. Bare names became
spawn-and-wait (gui3.c's hlt loop; `run` keeps the legacy loader on
purpose) -- which exposed that `fd_inherit()` read its parent off
CR3: a kernel-context caller runs with whatever address space the
scheduler last loaded, so `ps` typed at the console printed into a
Terminal window. The parent is an explicit argument now; SYS_SPAWN
passes its caller's pml4, kernel callers pass 0.

**A foreground job's terminal handoff rides on the spawn
(`SPAWN_FOREGROUND`).** tosh called `tcsetpgrp()` after `sys_spawn()`
returned, and the child's first read could beat it -- a whole
timeslice, once dynamic spawns got heavy enough to end the shell's
slice inside the syscall. The child then stopped on its own SIGTTIN
having already drawn its screen, which read as "the editor is broken"
(`[4.22] SIGTTIN fg=4` / `[4.24] tcsetpgrp fg=6` in the log was the
whole diagnosis). POSIX shells close this from the child's side
between fork and exec; a spawn ABI has no child side, so the kernel
does that half when asked -- musl added POSIX_SPAWN_TCSETPGROUP for
exactly this hole in posix_spawn.

**`/lib` pages are served from a kernel image cache, shared when
read-only.** Without it every dynamic process re-read libc.so from
disk page by page, each fault a preempt-disabled device poll -- the
whole machine stuttered a little per spawn, and marginal GUI checks
flaked under load. One cache keyed by (path, offset), `/lib` only
(immutable within a boot, so never-invalidated is honest): a
read-only page is mapped BORROWED into every process -- the design's
frame sharing, one frame of libc text total -- and a writable page
(GOT, data) is a memcpy instead of a disk read. Bounded by the size
of /lib; nothing evicts.

## The network stack is in the kernel, and a socket is a ping socket

**Where it runs.** ARP/IPv4/ICMP are ring-0 code reached through the
`SYS_SOCKET` fds that had been scaffolding since long before any driver
existed. That is Linux's and NT's shape. The alternative considered was
a supervised ring-3 `netd` with the kernel owning only a packet
interface -- a shared-memory rx/tx ring, which is exactly what
`sound_device` already does -- and it fits the direction this project
has been moving (the compositor is a process; the kernel contains no
applications). It was declined for one concrete reason and one general
one. Concretely, **sockets would become IPC to a service, and this OS
has no IPC that can carry them**: there are no unix sockets, `PIPE_MAX`
is 8 kernel-wide and a pipe carries no credentials, which is the same
wall `SYS_NOTIFY_READY` hit. Generally, it would strand the fd namespace
that already works -- `SYS_CLOSE` and process-exit cleanup have handled
socket fds for free since the scaffolding landed, because neither ever
looked at file-specific state.

**RE-ARGUED 2026-10-07 in `docs/netstack-design.md`**: the channel, the
shared memory and the wakeword this paragraph found missing exist now,
and what is left of the objection is what a socket's fd becomes.

The honest cost is that protocol parsing of hostile input runs in ring
0. That is the same call this kernel already made for TrueType fonts,
and it is bounded the same way: every read is length-checked against the
frame, and what the receive path allocates is capped (reassembly's slots
and each socket's byte budget -- see the fragmentation entry below).

**Why a ping socket and not a raw socket.** `AF_INET` + `SOCK_DGRAM` +
`IPPROTO_ICMP` is the whole supported set, and the kernel builds the
ICMP header: an application sends a payload and receives a payload. This
is Linux's ping socket (its `IPPROTO_ICMP` datagram socket), not
`SOCK_RAW`. The reason is not tidiness. A raw socket lets any process
emit any ICMP type it likes -- forged unreachables, redirects -- and
Linux gates that behind `CAP_NET_RAW`. **toy-os has no privilege model
at all**, so the only gate available is not offering the primitive; a
raw socket can arrive with the uid that would police it. The identifier
field is the demux key, which is what it is for, and is why two `ping`s
can run at once without reading each other's replies.

**Why the receive path is split across the interrupt.** `net_rx()` does
nothing but copy a frame into a static queue; the protocols run from
`net_poll()`, called from `scheduler_idle()` and from the socket
syscalls. Linux does the same thing (`netif_rx`, then a softirq), for
throughput reasons. Here the reason is correctness: `kmalloc` is not
interrupt-safe, the filesystem is not re-entrant, and `klog` is not
either -- so an ISR that parsed a packet would be reachable from every
one of those. The queue costs a memcpy per frame and buys the driver its
DMA buffer back immediately.

**Why `SYS_RECVFROM` does not block.** A blocking receive needs a wait
channel per socket, which the scheduler supports (`SCHED_CHAN_*` is an
address) but which also needs a wakeup from the receive path -- and the
receive path currently runs from the idle loop, so a process blocked on
a socket would have to not be the thing preventing the idle loop from
running. That is a real design question about where `net_poll()` belongs
once something can wait on it, and it is deferred rather than guessed
at. Returning 0 for "nothing yet" is unambiguous because a datagram
socket has no end-of-stream to confuse it with.

**Why addresses are host byte order across the whole API.** POSIX puts a
big-endian address in `sockaddr_in` and expects every caller to know it.
A `uint32_t` is the same type in either order, so a missed conversion
compiles perfectly and produces a packet nobody answers. Converting only
at the wire edge (`kernel/net/`) leaves exactly one place where the
mistake can be made, and `/bin/ping` never calls `htonl` at all.

## IPv4 fragments are reassembled in a bitmap, four at a time, and DF is never set

**What real systems do.** Linux keys an in-progress datagram on
(source, destination, id, protocol), holds its fragments in an
offset-ordered tree under a per-namespace memory ceiling
(`ipfrag_high_thresh`) and a 30 s timeout (`ipfrag_time`), evicts the
oldest under pressure, sends ICMP Time Exceeded (code 1) when a datagram
with its first fragment times out, and -- since FragmentSmack
(CVE-2018-5391) -- discards the whole datagram on any overlap. It
fragments UDP locally when a datagram exceeds the path MTU, and sets DF
on TCP so Path MTU Discovery can find that MTU. NT and the BSDs have the
same shape with a fixed slot count.

**What toy-os does, and where it differs.** The key, the timeout, the
eviction and the overlap rule are Linux's. The storage is not: each
datagram in progress is ONE 64 KiB buffer and a bitmap with a bit per
8-byte block (the unit the offset field counts in), allocated on its
first fragment and freed on completion or timeout, and at most four
exist. RFC 815's hole list and Linux's fragment tree both pay for
memory proportional to what arrived; the bitmap pays the maximum per
datagram and buys "have I got this range" as a bit test and
completeness as a byte count. Four slots bound what an attacker who
sends only first fragments can pin to 256 KiB; the obvious alternative,
a static pool, would cost that on every machine that never sees a
fragment.

**Exact duplicates are ignored, partial overlaps drop the datagram.** A
fragment whose blocks are already all held changes nothing -- the first
copy stands, so there is no ambiguity to exploit. One that partly
overlaps is either corruption or an attack on whoever reads the
datagram differently, and a parser here REJECTS rather than guesses.

**DF is never set, and there is no PMTUD.** Setting DF only pays with
Path MTU Discovery behind it -- without it, a smaller link on the path
turns every full-size packet into an unanswered ICMP Frag Needed and a
silent black hole. TCP does not need fragmentation (its MSS is
MTU-sized), and UDP falls back on routers fragmenting. Revisit when a
path with a smaller MTU (a VPN, PPPoE) shows up in testing.

**The socket queue became a byte budget.** A datagram can be 1 byte or
64 KiB, so four fixed 1472-byte slots per socket became four slots of
kmalloc'd messages under a 64 KiB budget per socket -- Linux's
`SO_RCVBUF`, which counts bytes for the same reason. The first datagram
into an empty queue is always taken, so a maximal one fits whatever the
budget says.

**A burst of fragments exposed two queues that never mattered before.**
A 64 KiB datagram is 45 frames. `e1000.c`'s 32-descriptor receive ring
lost the tail of every such burst from QEMU's SLIRP, and the USB
adapters' 16-slot transmit rings reported full with no completion ever
reaped while `net_tx()` waited. Both are fixed (a 64-descriptor ring;
the transmit op calling `xhci_service()` before it says full), and the
rule they add is in `docs/conventions/kernel.md`.

## UDP's port demux is the kernel's; DHCP and DNS are not

**Where the line falls.** `kernel/net/udp.c` is ports, checksums and
demultiplexing — mechanism that every consumer needs and that only the
place holding the socket table can provide. Everything above it is a
ring-3 program: `/bin/dhcp` runs the lease exchange, `/bin/host` and
`userland/lib/uresolv.c` resolve names. That is the same split the
compositor made, and the test for it is whether the decision is a
POLICY: which offer to accept, how long to wait for one, which server to
ask, what to write where. None of those get better for being in ring 0,
and each of them is a state machine talking to whoever answered a
broadcast first — which is not something to run with the whole address
space mapped.

The concrete evidence that the line is in the right place: `/bin/dhcp`
applies its result through `SYS_NET_CONFIG`, the same call `netctl`
uses. There is no privileged path in the DHCP client that a person could
not take by hand, which means the client cannot do anything wrong that
`netctl` could not also do.

**Why bind takes a device.** `SYS_BIND`'s `struct net_msg` carries a
device name, which looks like over-generality until you write a DHCP
client: it must send from 0.0.0.0 to 255.255.255.255 out of a
*particular* interface, before any interface has an address for routing
to work from. Linux has the same thing (`SO_BINDTODEVICE`) and dhclient
is the canonical user. The alternative — picking the first device — is
wrong the moment a machine has two cards, and wrong silently.

**Why a ping socket is still not a raw socket, now that UDP exists.** It
would have been easy to let ICMP sockets carry their own headers once
there was a second protocol to be consistent with. The reason not to is
unchanged and is about this kernel specifically: a raw socket lets any
process emit any ICMP type, Linux gates that behind `CAP_NET_RAW`, and
toy-os has no privilege model to gate with. UDP does not raise the same
question, because a UDP payload is the application's by definition.

**Why the port-unreachable report is sent but not received.** Sending it
is ~20 lines and turns a peer's silent timeout into an immediate answer.
Acting on one that ARRIVES needs somewhere to put it — an error queue on
the socket, which POSIX exposes as an error return on the next call, and
which needs a socket that can fail asynchronously. That is the same
missing machinery a blocking receive needs, so the two arrive together
or not at all. Dropping incoming reports is recorded in `icmp.c` where
it happens rather than left to be discovered.

## `/etc/resolv.conf`: Unix's name, this repo's format

The file that names the DNS server is at the path every Unix uses, and
its contents are `nameserver=10.0.2.3` rather than `nameserver 10.0.2.3`
— this repo's `key=value`, read by the `etc_config` parser that is
compiled into both rings.

The argument for the traditional format is compatibility, and there is
nothing here to be compatible with: no ported resolver reads this file,
and the one that does (`userland/lib/uresolv.c`) is ours. The argument
against writing a second parser is the one CLAUDE.md already makes about
config files generally — two `name=value` implementations drift, and the
drift surfaces as the system and `config` disagreeing about what a file
says, which is the shape of bug nobody looks for. A one-field format
would be twenty lines of parser to save a reader one unfamiliar
character.

The path is Unix's because that is where a person will look.

## A blocking receive: one wait channel, and the deadline on the socket

**Where the stack runs while a reader sleeps.** `net_poll()` runs from
`scheduler_idle()`, which is only reached when nothing else is runnable
— so a process blocked in a receive could sleep through a packet that
had already arrived, on any machine with something else to do. Three
options were weighed. Running `net_poll()` from the timer tick is
Linux's softirq, approximately, and bounds latency at 10 ms; the cost is
that ARP replies, echo responses and driver MMIO writes all happen in
interrupt context, which is a much larger claim to keep true. Leaving it
in the idle loop and waking only on delivery is the smallest change and
makes receive latency depend on what else is running, which is exactly
the kind of thing that presents as a flaky network.

What was built is the third: **the driver's ISR wakes, and the woken
reader runs the stack itself.** `net_rx()` already runs in the
interrupt and already does nothing but copy a frame into a queue; it now
also calls `scheduler_wake()`, which is documented as interrupt-safe
because it only flips state and writes an already-saved trapframe. The
woken process re-enters `sys_recvfrom`, which calls `net_poll()` at its
top — so the parsing happens in a process context, and the property the
whole receive path was built around survives.

**One channel for the whole stack, not one per socket.** The waker has
parsed nothing: it holds an Ethernet frame and cannot know which socket
it is for without doing the work that must not happen there. So every
blocked reader is woken and each looks again. With eight sockets that
costs a spurious wake or two, and the alternative is either parsing in
the ISR or a second mechanism to carry "which socket" out of it.

**Why the deadline lives on the socket.** A blocking syscall here is
RE-RUN rather than resumed — `SYS_RETRY` sends the caller back through
libsys's retry loop, and a signal rewinds the instruction so the call
re-enters from the start. A deadline computed from `timeout_ms` on each
entry would therefore restart on every wake, and a socket on a busy
network would never time out at all. Storing an absolute deadline on the
socket also gives the behaviour Linux's `restart_block` provides: an
interrupted wait resumes against the time it had left, not the time it
originally asked for.

**Why a timeout returns 0 rather than -EAGAIN.** POSIX's `SO_RCVTIMEO`
fails the call with EAGAIN. Here a datagram socket has no end-of-stream,
so 0 is unambiguous — and every call site was already written as
`n > 0`, so the divergence costs nothing and removes an errno check from
each of them. The timeout is on the CALL rather than on the socket
because this kernel has no `setsockopt`, and inventing one for a single
option is a worse trade than the divergence; `recvmmsg(2)` takes a
timeout argument for the same reason.

**A general primitive has to be general in both directions.** Widening
`scheduler_wake_timers()` from "sleepers on SCHED_CHAN_TIMER" to "anyone
with a deadline" made a field that had been written by exactly one
caller readable by all of them -- and slots are recycled, so a stale
`wake_at_ns` from a previous tenant became a deadline in the past. Every
blocking wait on the machine then returned instantly, and the whole GUI
suite failed at once. The fix is that every park writes the field, 0
included: an initialiser that sets "the fields this caller needs" is the
same shape as the fd bug below, and both were found the same day.

## A pooled fd description must be zeroed, not partially initialised

`fd_desc_alloc()` set the fields the new descriptor needed and left the
rest as the previous owner had them. `nonblock` therefore survived a
close: a program that set it on a socket handed the flag to whatever
reused the slot, so the NEXT program's blocking receive returned 0
immediately and lost every reply.

It is worth recording because of how it presented rather than what it
was. `host` and `ping` failed **only after something unrelated had
run** — a fresh boot was always fine, and so was any single command.
The failing pair was `udp_test` (whose checks deliberately set
non-blocking) followed by anything that received a datagram, which
reads as "DNS is broken" and sends you into the resolver. Two theories
were wrong before instrumenting: a lost wakeup, and a checksum rejecting
the reply. What settled it was the observation that the frame ARRIVED
(the device counter moved) while the socket never saw it, and that a
fresh boot could not reproduce.

The fix is to zero the whole slot at allocation rather than to clear the
one field, because the bug is the pattern and not the field: any future
per-description flag would inherit exactly the same way, and the next
one might not have a symptom this loud.

## TCP's timers ride the blocking receive

A TCP connection needs a clock: retransmission is the whole difference
between TCP and a datagram with sequence numbers. This kernel has no
softirq, no timer wheel and no kernel threads, so there was no obvious
place to put one.

Three options were weighed. **A periodic tick from the timer IRQ** is
BSD's `tcp_slowtimo`/`tcp_fasttimo` and is correct regardless of what
any process is doing — but retransmitting means building a segment and
handing it to a driver, so the whole stack would run in interrupt
context, which is exactly the property the receive path was designed to
avoid. **A kernel timer process** would put the work in a normal
context, at the cost of adding a scheduled entity that exists only to
tick, in a kernel that has deliberately avoided kernel threads.

What was built is the third: **the process waiting for data is the one
that drives the connection.** A blocked reader parks until
`net_wait_deadline()` — its own timeout, or the earliest retransmit
across all connections, whichever comes first — wakes, runs `tcp_tick()`
inside `net_poll()`, and parks again. It reuses the bounded wait added
for the blocking receive, costs nothing when nothing is outstanding, and
keeps every line of TCP in process context.

**The gap this leaves is real and is stated where it happens: a
connection nobody is reading has nobody to wake it**, so its retransmits
wait for the idle loop. For a client that is survivable — a client is by
definition waiting for its response. It is exactly what a server could
not do, which is the same boundary that made listen/accept a separate
piece of work rather than half of this one.

## A connection block outlives its socket

`close()` on a TCP socket cannot free the connection: the peer is still
owed a FIN, and the FIN is still owed an acknowledgement. So the block
is marked an ORPHAN and the stack keeps driving it after the application
has gone.

Something then has to reclaim it, and "when it reaches CLOSED" is not
enough — a peer that vanished never acknowledges anything. Real stacks
hold the block through TIME_WAIT for two maximum segment lifetimes
(minutes) so a delayed duplicate cannot be mistaken for part of a new
connection. That is the right answer for a host with thousands of
connection blocks and the wrong one here: there are four, and holding
one for two minutes because a peer went away would exhaust the pool long
before it protected anything. The linger is 2 seconds.

It was found the way these things usually are: the seventh TCP KTEST
failed with `-ENOSPC` while the six before it passed, because each had
left a block behind. The tests now complete the close — the fake peer
acknowledges the FIN and sends its own — which is both a better test and
what stopped them leaking.

## The passive open, and why the server is one connection at a time

`listen()`/`accept()` complete the shape: `wget` proved this OS can
reach out, `/bin/httpd` proves something can reach in.

**Accept returns a NEW socket**, as POSIX says, because a listener and a
connection are different objects with different states — and because
the four-tuple demultiplex needs the connection to exist separately
from the port it arrived on. The listener is untouched by a connection
it produces, which is asserted behaviourally in the KTESTs (a second
client connects and is accepted) rather than by reading a state
variable: "it still works" is the property, and the variable is only
evidence for it.

**The server handles one connection at a time**, and that is a real
constraint rather than laziness. A connection's retransmission timers
are driven by the process reading it, so a design where several
connections are open and only one is being read would leave the others'
timers to the idle loop. The alternative this kernel could express is
inetd's: `dup2` the connection onto fds 0 and 1 and spawn a handler,
which works precisely because only 0/1/2 are inherited across
`SYS_SPAWN`. That would make a handler an ordinary filter — `cat` could
be a service — and it is a roadmap item rather than something
listen/accept is missing.

**A full backlog drops the SYN rather than answering with a RST.** That
is Linux's default (`tcp_abort_on_overflow=0`) and the reasoning is
about what a client experiences: a drop lets its own SYN retransmission
succeed a moment later, where a reset turns a momentary burst into a
hard failure. The cost is that a client talking to a permanently
saturated server waits out its connect timeout instead of being told,
which is the trade every stack makes the same way.

**The backlog is not a queue of its own.** A connection whose handshake
finished needs a full connection block regardless, so `TCP_BACKLOG`
counts blocks that are finished and unaccepted. Adding a separate queue
would mean holding half-open connections more cheaply — which is what
SYN cookies are for, and which matters when somebody is attacking you.

## A connection per child, and why the spawn names fd 0 rather than dup2

`/bin/inetd` accepts on a port and gives each connection its own
process, with the socket on fd 0 and fd 1. It is the one concurrency
this kernel can express without `fork`, and it works because only 0/1/2
cross a spawn.

**The obvious design does not work here, and the roadmap predicted the
wrong one.** Unix inetd `dup2`s the connection onto 0 and 1 *in the
child*, between `fork()` and `exec()`. There is no such window in a
spawn ABI: the process is already running when the call returns. A
parent doing it beforehand would have to point its OWN fd 0 and 1 at the
connection and put them back afterwards, so anything it printed in
between — a log line, a diagnostic — would go to the client instead.
`struct spawn_msg` gained a `stdin_fd` instead, beside the `stdout_fd`
it already had, and both accept a connected socket. That is
`posix_spawn`'s `file_actions` at the size this kernel needs it.

**fd 2 is deliberately not redirected.** It stays the kernel log, so a
handler's diagnostics reach `dmesg` and never reach the client. The
asymmetry is the useful part rather than an omission.

**The point is not concurrency, it is that a handler is an ordinary
filter.** `/bin/cat` copies fd 0 to fd 1 and knows nothing about
sockets, so `inetd -p 7 /bin/cat` is a real echo server. That is what
makes this worth building over teaching `httpd` to spawn copies of
itself: the mechanism has more than one user the day it lands.

**And it closes a real gap, not only a performance one.** A
connection's retransmission timers are driven by the process blocked
reading it, so a server holding several connections and reading one
leaves the rest to the idle loop — the honest limitation the serial
`httpd` was written around. One process per connection means none is
unattended.

**At the child cap, inetd stops accepting rather than refusing.** The
connection waits in the backlog and, past that, the SYN is dropped and
the client's own retransmission covers it — the same trade the backlog
itself already makes, and for the same reason: a refusal turns a
momentary burst into a hard failure. The cap's maximum is arithmetic
rather than policy, since `SOCK_MAX` and `TCP_MAX_CONNS` are 8 apiece
and a listener costs one of each.

**A RST is now validated before it is believed.** It must acknowledge
our SYN in SYN_SENT, and afterwards sit exactly at `rcv_nxt`. The
previous code accepted any reset naming the right ports, which is the
blind-reset attack RFC 5961 exists for — cheap to close, and the reason
it was noticed at all is that a test needed a deterministic way to tear
a connection down and the obvious one worked far too easily.

## The compositor's wait reports READINESS, and does not deliver

`SYS_WAIT_READY(ms)` parks a process until its event queue is non-empty
or a deadline passes, and hands back nothing. The obvious call to build
was a timed `SYS_WAIT_EVENT` -- the same signature as the existing one
plus a timeout, returning the event. It would have been wrong, and
silently.

**A compositor drains its own queue.** `userland/wm/wm_rawin.c` loops on
`sys_poll_event()` and dispatches every type: raw input it keeps, client
requests it hands to `wm_client.c`, anything it does not recognise it
drops on purpose. A wait that returned one event would have taken that
event OUT of the queue, so the drain that follows would never see it --
one event lost per wait, and the ones lost would be whichever arrived
while the compositor was idle. That is invisible in every test that
drives the desktop, because driving it means the queue is never empty.

So this is `poll()` beside `read()`, and `/tests/waitready_test`'s
load-bearing check is that a `sys_poll_event()` after the wait still
returns the event.

**The wait needs BOTH kinds of wake source.** Input and client requests
arrive as events. The tray clock, the client pings, the `/etc`
generation polls and -- the load-bearing one -- its clients'
`WIN_REQ_TIMER` timers do not, because the compositor is the thing
serving them. Blocking on events alone stops the clock and starves every
client timer; waiting on a deadline alone is the poll this replaced.

**A 0 return deliberately does not say WHICH happened.** Woken-by-event
and timed-out lead to the same next action -- drain, re-check deadlines,
loop -- so the distinction has no correct use, and a caller that
branched on it would be wrong the first time both happened at once.

**THE KERNEL HALF ALREADY EXISTED, AND THAT IS WHERE THE TRAP IS.**
`scheduler_block_current_until()` (Linux's `schedule_timeout()`) landed
with the blocking socket receive, not with this. Its deadline wake does
NOT write 0: `scheduler_wake_timers()` writes 0 only for a
`SCHED_CHAN_TIMER` sleeper and `SYS_RETRY` for anything else, because a
socket handler woken at its deadline has to look again and decide
whether an empty queue is a timeout or a spurious wake. `SYS_WAIT_READY`
parks on the EVENT channel, so its timeout arrives as `SYS_RETRY` --
which is outside the errno range, so `err()` passes it through
untouched and ring 3 sees `-4095` where the ABI promised 0.

`sys_wait_ready()` folds it, and must NOT loop on it the way
`sys_read()`/`sys_write()` do: looping on a sentinel that means "call
again" would turn a bounded wait into one that never ends. The general
shape is worth the entry -- **a sentinel whose meaning depends on which
channel a process parked on is one a second caller on a different
channel will read wrong**, and it fails as a wrong return value rather
than as a crash.

**WHAT IT MEASURED.** Host CPU of the QEMU process over 30 s with
nothing touching the guest, three samples each: **0.78/0.73/0.78 s
before, 0.56/0.58/0.58 s after** -- about a quarter less, with the
ranges not overlapping. The guest's own accounting shows NOTHING; `ps`
reports the same CPU seconds either way and the only column that moves
is the state, `ready` to `block(event)`, because `ps` bills whoever was
current at the tick and something always is. Absolute figures from
`idle_cpu.py` are mostly TCG and mostly the host's load at the time --
an earlier pair on this same change read 42% of a core against 37-38%,
and neither number is comparable with these. Quote the DIFFERENCE.

## The kernel invents no address, and a link-local claim is ring-3 policy

`net_autoconfig()` used to give the first card 10.0.2.15/24 via 10.0.2.2
at boot, QEMU's user-networking defaults, and its own comment called
itself "a placeholder for DHCP, which is where an address is supposed to
come from". It is gone. A card comes up unconfigured and `/bin/dhcp` --
run at boot as init's `dhcp` one-shot -- is the only thing here that
assigns an address.

**Why not keep it as a fallback**, which is the cheaper option and the
one that breaks nothing: because the address it invents is a routable
address belonging to somebody else's network. On QEMU it is right by
construction; on real hardware, or on any segment that is not SLIRP's,
a machine whose DHCP exchange failed would silently claim 10.0.2.15 and
answer ARP for it. Linux's kernel assigns no address at all (bar `ip=`
for an NFS root); Windows assigns none and falls back to APIPA. Neither
invents one that only makes sense on one emulator, and the placeholder
had already outlived the thing it was standing in for.

**What replaces it is RFC 3927 link-local**, the same answer Windows
reached: a device nobody offers a lease to claims 169.254.x.y/16 with no
gateway. That keeps the property the hardcode was protecting -- a
machine on a segment with no server can still talk to its neighbours --
without asserting anything about a network this OS has not been told
about.

**The split is the one the DHCP client already made.** Choosing an
address, probing it, deciding how many times and how long to wait, and
what to do about a collision are all policy, and policy is `/bin/dhcp`'s.
The kernel supplies exactly the thing ring 3 cannot do for itself:
`SYS_NET_ARP_PROBE` puts one ARP request on the wire and says whether a
reply has been cached for it. It does not block, so the answer is only
ever "not yet" and the caller asks again -- which is what makes the
probe COUNT and the probe SPACING ring 3's, rather than constants
compiled into a kernel that would then own an RFC.

That syscall is almost free because `arp_resolve()` was already the
probe: it broadcasts a request whose sender field is the device's own
address, which is 0.0.0.0 on a device that has none, and that is exactly
RFC 3927's ARP Probe. Applying the address and asking once more sends
the same frame with the new address as its sender, which is exactly the
RFC's ARP Announcement. One mechanism spells both because the difference
between them is which address the device holds at the time.

**What this does NOT do is defend the address.** The RFC asks a host to
keep watching for a conflicting ARP afterwards; `arp_input()` learns
from a conflicting frame without noticing that it conflicts, and there
is no channel to report one on. That is a roadmap item, and it needs the
same missing machinery an ICMP error queue does.

**The cost paid for all of this is that the network comes up after the
console does.** The kernel used to have an address before the first
process ran; now a lease lands about a second into the boot. Anything
that pings a freshly booted guest has to wait for it -- `tools/net_test.py`
polls, and a phase that did not was failing as `no such device`, which
looks nothing like what it is.

## S5 is not one write: the wake sources have to be turned off first

**Measured on hardware, 2026-08-31.** An ASUS notebook running the live
image rebooted instead of powering off, every time, and Linux Mint shut
the same machine down. Two things were missing, both of which Linux does
before every sleep and neither of which any test here can reach -- a
QEMU guest has no pending wake event to come back up on:

- `PM1_STS` was never cleared. `PM1a_EVT_BLK` was not even parsed, only
  `PM1a_CNT_BLK`; the wake-status bits live in the former.
- The GPE blocks were never parsed either, so nothing disabled them.

**Which one bit is known, because it was measured rather than reasoned
about.** The `nogpe` boot flag skips the GPE disable and keeps the
`PM1_STS` clear; with it the machine reboots, without it the machine
stops. **The GPE disable is the fix.** That laptop's `GPE0_BLK` is 32
bytes -- 128 general purpose events, among them its lid, its embedded
controller and USB -- and one of them was armed and pending. Clearing
`PM1_STS` alone was NOT sufficient. Whether it is needed at all was not
tested and is kept regardless: the spec requires it, Linux does it, and
the cost is two `outw`s.

**AND THE ENABLES ARE A DILEMMA, WHICH TOOK THREE TRIES.**
Masking every GPE and leaving it masked shut the machine down and then
took **two presses of the power button** to start it again -- the second
symptom of one mistake, reported by the same person on the same machine.
Linux disables every GPE, clears the statuses, and then re-enables the
WAKE-CAPABLE ones, which it knows from each device's `_PRW` object.
There is no AML interpreter here to evaluate one, so what goes back is
what the firmware had enabled: a superset of the wake set, and the
alternative is leaving the power button's own GPE masked. **And restoring them all brought the reboot straight back**, which is
the finding that matters: clearing a status does nothing if the source
is still ASSERTING it. A level-triggered GPE -- an embedded
controller's is -- re-latches within a few port cycles, and a set status
with a set enable is a wake. So the two obvious fixes each break what
the other fixes, and there is no correct middle by guesswork. That is
exactly the problem `_PRW` exists to solve.

**The middle by MEASUREMENT was tried and it failed, which is the most
useful result of the three.** Mask, clear, read the status back, rearm
only the bits that stayed quiet: the machine rebooted, and NOTHING had
re-latched to be masked. So the source that wakes it is not asserting
when the sleep is prepared -- it fires during or after the transition,
and no measurement taken at one instant can find it.

**What shipped is therefore the first version: disable everything and
leave it masked.** That laptop then takes two presses to start
(`docs/bugs.md`), and this entry used to blame the masked GPEs for it,
calling `_PRW` load-bearing. **Measured on 2026-10-09, that was wrong.**
The machine's power button is the fixed PM1 button (FADT `PWR_BUTTON=0`,
no `PNP0C0C` device in the DSDT), so no GPE mask can reach it. All 17 of
its `_PRW`s are Methods, and none wakes from S5 (`tools/aml_walk.py
--prw`). Linux re-arms a device's GPE for S5 only when `_PRW` says it
wakes from S5, so Linux's wake set there is empty: masking everything
IS the correct S5 answer on this machine. And a shutdown done by the
firmware itself needs one press. So the two presses come from something
toy-os's path does or skips -- entering ACPI mode at the last moment,
or not running `_PTS` -- and the boot words `acpimode=` and `gpewake=`
isolate it one boot at a time.

**The answer, measured the same day: the ACPI-mode switch.** Same build,
same machine, one boot word apart: entering ACPI mode and then writing
S5 needed two presses to start again; writing S5 in legacy mode needed
one. So when the firmware leaves ACPI mode off at boot, `acpi_poweroff()`
now writes S5 in legacy mode first -- the path the firmware's own
power-button shutdown takes, which on many chipsets is an SMI trap of
the sleep write, where the firmware does its own preparation -- and
enters ACPI mode only if the machine is still running half a second
later. In ACPI mode the firmware expects the OS to have run `_PTS`
first; on that DSDT `_PTS` tells the embedded controller the sleep
state and sets two chipset bits, and toy-os, with no interpreter, cannot
run it. Linux and Windows do the opposite -- ACPI mode from boot, `_PTS`
run -- and toy-os deliberately differs because the half of that it can
do is the half that broke the machine. `acpimode=poweroff` keeps the old
order for an A/B on the next machine.

**Confirmed on a second machine the same day:** the Kaby Lake desktop,
whose firmware also leaves ACPI mode off, powers off through the legacy
path and starts on one press (maintainer). The ACPI-mode fallback has
not run on any machine yet: every one so far stopped on the legacy write.

**One thing that was missing throughout and is now fixed regardless:
the sleep write is TWO writes**, the sleep type first and the enable
second, as `acpi_hw_legacy_sleep()` does. One write carrying both is
what this used to do and what some chipsets are documented not to
accept. It did not fix this machine; it is correct anyway.

**Why a flag rather than a bisect.** Both fixes landed together, so a
machine that stops is consistent with either, and the machine in
question is not one any harness here can drive. `nogpe` turns the
attribution into one boot -- the argument `nopat`, `notsc` and
`novirtio` already make, and the reason those exist.

**And why `acpidebug` exists at all: the failure destroys its own
evidence.** A machine that reboots takes the log with it, a live image
has no disk to keep one on, and the console is gone after the reset. So
the flag prints the plan on screen and pauses ten seconds. Its limit is
worth knowing: shutting down from the DESKTOP leaves the compositor
owning the screen, so the text is invisible and only the pause is
observable -- reading it needs a shutdown from the text target.

**The theory this disproved.** `acpi` reported `ACPI mode: no` on that
machine, which made the firmware handing over in legacy mode look like
the cause: writing `SLP_EN` to `PM1a_CNT` outside ACPI mode is trapped
to an SMI on many chipsets, and what the handler does is its own
business. `acpidebug` printed `ACPI mode ON` -- the SMI handover at port
0xb2 works, and `acpi` says `no` at boot only because
`enable_acpi_mode()` does not run until the poweroff attempt. A cheap
check retired a plausible mechanism that would have cost a day.

## ACPI stops at the tables, and `_S5_` is the one deliberate exception

`kernel/acpi/` finds the RSDP, walks the RSDT or XSDT, and decodes the
FADT and the MADT. It also byte-scans the DSDT for one AML object. That
combination looks inconsistent, and the inconsistency is the decision.

**What real systems do.** Linux and Windows both carry a full AML
interpreter -- ACPICA and the NT AML engine -- and *evaluate* `\_S5` to
get the sleep type before writing PM1a_CNT. Neither hardcodes a port or
a value; both derive everything from the FADT. ACPICA is around 100,000
lines and implements a bytecode virtual machine with its own object
model, namespace, mutexes and operation regions, because AML methods can
touch PCI config space and the embedded controller and call each other.
That is the price of the general answer, and it buys battery status,
thermal zones, S3, hotplug and GPE dispatch.

**toy-os wants exactly one thing out of AML.** The FADT gives the port
to write and every bit of the sleep register's layout; the only fact it
does not carry is the three-bit sleep type for S5, and that lives in a
`Name (\_S5, Package (...) {...})` object in the DSDT. That object's
grammar is fixed -- a NameOp, a PackageOp, a PkgLength, an element
count, then constants -- and decoding it is sixty lines with no
namespace, no evaluation and no side effects, because a Name is data.

So the boundary is: **anything that is a fixed-layout structure gets
parsed; anything that requires EVALUATING AML does not exist here.**
Battery, thermal and S3 all fall on the far side, which is why they are
still roadmap items rather than "nearly done now that ACPI is in".

**THIS BOUNDARY WAS MOVED ON 2026-08-31, and the old line was drawn in
the wrong place.** A laptop needed its GPE wake set to stay powered off,
and that set is written down as a `_PRW` object per device. `_PRW` is a
`Name` holding a constant `Package` -- data with a fixed grammar,
exactly like `_S5_` -- so it falls on the NEAR side of the boundary as
stated, while this entry's own list of far-side things named "GPEs". The
list was imprecise: what GPE *dispatch* needs is evaluation; what a wake
SET needs is a walk.

The decision now is that toy-os gets a NAMESPACE WALK -- declarations
parsed, every Method body skipped by its PkgLength, nothing executed --
and still no interpreter. `docs/aml-design.md` stages it and carries the
honest case against, including the part that has not changed: the
things a person actually wants from ACPI on a laptop are all Methods,
so a walk lands one wake set and stops at the same wall one level
further in.

**Why a walk rather than a second byte scan.** `acpi_scan_s5()` is a
signature hunt with no notion of scope, which works because `_S5_` is
unique and at the root. `_PRW` is neither: it appears once per
wake-capable device. A second scan would be the third copy of a pattern
that wants to be a parser, and each copy works by luck.

**Why not skip AML entirely and assume a sleep type.** This was the
cheaper option and it is what makes the difference between working and
not working on the hardware that motivated the change. QEMU's `_S5_`
says 0, so `outw(0x604, 0x2000)` -- the old hardcode -- is accidentally
correct there; VirtualBox and most real firmware say something else, and
writing the wrong sleep type to a real PM1 control register is a request
to enter a state that is not S5. "Assume 5, it is the common value" has
the same shape as the hardcode it would replace: right until the machine
where it is not, which is the machine that is hardest to debug.

**The scan REFUSES rather than guesses**, which is this repo's parser
rule and matters more here than usual. It accepts a `_S5_` introduced by
NameOp (directly or behind a root or parent prefix) whose package
elements are ZeroOp, OneOp or a byte constant, and nothing else -- not a
DWordPrefix, not the four letters appearing inside a string. A refusal
leaves ACPI_F_S5 clear, `/bin/acpi` prints `poweroff: no`, and
`system_poweroff()` falls through to the legacy write and then to a halt.
An acceptance that guessed would write a real sleep request built out of
whatever bytes happened to follow.

**Why the fallbacks stayed.** `system_poweroff()` tries ACPI, then the
0x604 shortcut, then halts; `system_reboot()` tries the FADT's reset
register, then the 8042 pulse, then halts. Each rung covers a machine
the one above it does not, and the ordering is "what the firmware asked
for" before "what usually works". The cost is that the observable
outcome is the same at every rung -- the machine stops -- so which rung
ran is only visible in the log, which is why each one prints a line and
why `tools/poweroff_test.py` asserts on those lines rather than on the
machine stopping.

**The MADT is here for a different reason** -- it is
`docs/smp-design.md`'s Stage 1, which wanted the table walk anyway.
Parsing it alongside the FADT costs one file and makes `lscpu` able to
count cores at all, which CPUID cannot do. Nothing is started on them,
and the list says `online: no` for every entry so that it reads as a
fact rather than a claim.

**One thing the tables are NOT used for yet: HPET.** Its table is found
and listed now, and registering it as a third clocksource is the item
that was waiting on exactly this. It was deliberately not done in the
same change: it is a clocksource question, not a power question, and
bundling it would have made the poweroff fix untestable on its own.

## A write-combined range splits the 2 MiB page it lands in, rather than the allocator avoiding the page

Found on 2026-09-03 by `gfxbench` on the laptop, after the Intel
driver's extra scanouts (2026-09-02) moved write-combined memory from
the firmware's framebuffer into ordinary frames: a console scroll cost
37 ms a line. `pat_apply()` retyped every 2 MiB identity-map page a
range touched, so nine huge pages of RAM around the scanouts became
write-combined and gfx's back buffer, allocated next, read at bus speed.

Two fixes were possible. The allocator side: align every write-combined
allocation to 2 MiB and hand out whole huge pages, so nothing shares
one. The paging side: split a partly covered huge page into 4 KiB
leaves and type only the covered ones, Linux's `set_memory_wc` and the
guard-page splitter this file already has. The paging side won because
the bug is a property of the TYPING, not of one caller: a driver that
allocates a DMA ring beside a framebuffer, or a test typing one frame,
hits it just the same, and an alignment rule at every such call site is
the pointer-someone-must-remember shape. The cost is a fixed pool of
split tables (8, 32 KiB of bss) and a slightly larger TLB footprint for
the split ends; a range that covers a huge page whole is still typed
as one, so a 256 MiB aperture stays huge. The pool exhausting is logged
and leaves the range partly cached -- slow, never wrong -- rather than
retyping whole pages, which was the bug.

What the measurement then said about the blitter that prompted it is
under "Intel blitter acceleration" in `docs/roadmap-details.md`.


## A service's stdout goes to a second log ring, addressed by sequence, and a spawn names it with a sentinel

The kernel ring holds a few hundred lines, and on 2026-09-06 a driver
polling a device that had gone away logged a line a second and flushed a
laptop's whole boot log — the boot that mattered could not be diagnosed
at all. `logd` and `/var/log/toyos.log` were the answer to keeping it.
This is the second half: attributing what a **program** said, so
`log -u netd` means what it says.

**Why a second ring rather than more klog.** The obvious move is to have
a service's stdout call `klog_write()` with its name prefixed, and it is
wrong for the reason the file exists: a chatty program could then evict
kernel evidence, which is exactly the failure being fixed. systemd's
journal merges kernel and userspace into one store and gets away with it
because the store is on disk and large; toy-os's is 64 records of `.bss`
in front of a one-second poll. Two rings mean a program flooding its own
ring costs only its own lines. Linux draws the same line the other way —
`/dev/kmsg` accepts writes from userspace — and toy-os deliberately
differs, because a kernel ring nothing can flood is worth more here than
one stream.

**Why records and not bytes.** `QUERY_KLOG` hands out byte slices because
a kernel line is already prefixed with its subsystem and nothing needs to
attribute it afterwards (see the entry above). Application output is the
opposite: the text is whatever the program printed and the useful fact is
*who*. A tag per byte slice would be a lie the moment two processes wrote
in the same slice, so the ring stores one record per write, each carrying
its writer.

**One write is one record, and the reader joins them.** The first cut of
this assumed `stdio` line-buffering made a record a line in practice.
That was wrong, and real hardware showed it within a boot: `cmd_fail_err()`
in `lib/cmd.h` sends five separate `sys_print()` calls, so `netd`'s one
DHCP failure arrived in `/var/log` as five lines. Thirty-five files here
write through `sys_print` chains that bypass `stdio` entirely —
`service.c` has 21 calls, `meminfo.c` 19 — so fragments are the common
case, not the corner.

The fix is one bit, not a buffer. Stripping the trailing newline destroys
the only evidence that a write finished a line, so each record now
RECORDS it (`eol`), and `logd` joins fragments per tag until one carries
it. The buffer lives in ring 3, where `logd` already reassembles the
kernel's byte stream; the kernel would need one per open descriptor to
hold the same state, which is what makes ring 3 the right side of the
boundary for it.

**Per TAG, not by arrival**: two processes interleave freely in one ring,
so joining whatever arrived next would splice one program's line into
another's. For the same reason a write of nothing but a newline closes
only the SAME writer's pending fragment — handing one program's
terminator to another's half-line is the exact bug the tag exists to make
impossible.

**The tag is the KERNEL's, not the caller's.** `sys_write()` reads the
writing process's name out of its scheduler slot. A tag passed in would
be a tag a program could forge, and the entire value of `log -u toywm` is
that it cannot be. It costs the truncation from `PROC_NAME_MAX` (24) to
`APPLOG_TAG_MAX` (16), which is a real limit and is cheap next to a
forgeable one.

**The sequence is the interface**, the same call `QUERY_KLOG`'s absolute
offset makes: a reader compares the sequence it wants against the
`oldest` every record reports, so records lost to the ring moving are
*visible* rather than silent, and `logd` writes a line saying how many.
`applog_get()` re-checks the sequence against the record after the bound
check, because the kernel is preemptible and the slot can be overwritten
between the two — handing back a newer record wearing an older sequence
would be worse than reporting the gap, since nothing downstream could
ever notice.

**`SPAWN_FD_LOG` is a sentinel, not an fd**, and that is what makes
per-service logging cost nothing. The alternative is a pipe per service
with `logd` reading them, which is what a Unix-shaped answer looks like
and is unaffordable here: `PIPE_MAX` is 8 **kernel-wide** against six
services, so it would leave the shell unable to run `ls | grep`. A
sentinel needs no resource, cannot fill, and cannot block its writer —
which a pipe to a stalled reader does, and a service blocked on its own
log output is a worse failure than losing the line. systemd solves the
same problem with a socket per unit; that needs a socket type and a
per-unit connection this system has no reason to build yet.

**The trap the sentinel introduced**, and it cost a red test to find:
`sys_spawn()` guarded its stdout handling with `if (stdout_fd >= 0)`, so
`SPAWN_FD_LOG` (-2) fell through to "the console" with nothing refused
and nothing logged. The test is `!= -1` now — `-1` is the only value
meaning the console, and every other negative is a sentinel or a mistake,
so an unknown one is refused rather than silently reinterpreted.

**`StandardOutput=` and its one exception.** A service's output goes to
the log by default, which is systemd's default (`StandardOutput=journal`)
and for the same reason: once a compositor owns the screen there is no
console anybody is reading. `StandardOutput=inherit` is the opt-out, and
`tosh` is the only descriptor carrying it — an interactive shell whose
prompt went to a log file would answer nothing.

## The connection log records flows, and hostnames come from the resolver rather than from DNS in ring 0

Asked for on 2026-09-05 as "log all the outgoing connections -- IP /
hostname and port and time". Three forks, each with an obvious wrong
answer.

**What to log.** The obvious reading is "every outgoing packet", which
is a packet log: on a stack whose ring holds 128 records, one `wget`
buries the DHCP and DNS that preceded it, and CLAUDE.md's rule about a
probe outrunning its log applies to the log itself. Every system that
does this in production records the START of a flow instead --
netfilter's `--ctstate NEW`, bcc's `tcpconnect`, Sysmon's Event 3 -- so
a TCP open is one record and a datagram socket makes one per
destination. The narrowing is a setting (`system.conn_log`: `all`,
`tcp`, `off`) rather than a compile-time choice, because the
interesting cases differ: a person debugging DNS wants the UDP flows
and a person watching a server wants only the TCP opens.

**Where to hook.** `ipv4_output()` is the one place every outbound
datagram passes, and it is the wrong place twice over. It cannot tell a
retransmit from a new connection, so the dedupe would have to be
rebuilt above it; and it is reached from `net_poll()` on behalf of
whichever process is in a syscall, so it would attribute connections to
the wrong program -- which is exactly why an inbound connection is
recorded at `accept()` rather than when its SYN arrives. The socket
layer sees a connection as one object and runs in the caller's context,
so both problems disappear.

**Where hostnames come from.** The kernel has no resolver and never
sees a name. Two shapes exist in the wild: snoop DNS on the wire (Zeek,
dnstap) or have the resolver report what it looked up (Sysmon's Event
22 feeding its Event 3). Snooping needs a DNS parser in ring 0 over
attacker-shaped data, which is the surface `ttf.c` is bounds-checked
for and the one Windows moved out of the kernel entirely -- and it would
be a SECOND parser, since `uresolv.c` already has one. So
`SYS_NET_RESOLVED` is a report: `uresolv_lookup()` calls it, the kernel
keeps a 16-entry address-to-name cache, and a record picks up whatever
name is there.

The cost is stated rather than hidden: any process can claim any name
for any address, and there is no privilege model here to gate that with.
It is acceptable because the address in a record is never affected --
`netlog` prints a name in brackets after the address, never instead of
it -- so the worst a lie achieves is a wrong label beside a correct
fact. The alternative that would fix it (a resolver daemon owning the
cache, D-Bus-style) is a bigger structure than the log it would serve.

**Not persisted, deliberately.** The ring is memory, so the log dies
with the machine. Writing it to `/var/log` from the socket layer would
mean a filesystem write inside a preemption-guarded `FS_OP` on the
connect path; the shape that works is a ring-3 drainer reading the ring
and appending, which is `journald` reading `/dev/kmsg`, and it is a
roadmap item rather than something smuggled in here.

## An interface name is the card's identity, not the socket it is in

Interface names used to be `net` plus the table index, handed out in
probe order. That broke twice in one afternoon on a two-card machine: a
USB adapter unplugged and replugged registered a second time and was
listed as both `net1` and `net2`, and adding a built-in NIC renamed the
USB one from `net0` to `net1` -- which matters because sockets bind to a
device by NAME (`socket.c` keeps a string, not a pointer), so a reused
name silently moves a bound socket onto different hardware.

**What everyone else does.** Linux's kernel names by probe order too
(`eth0`) and then lets udev rename; systemd's predictable scheme derives
the name from the bus path (`enp3s0`), with `enx<mac>` available as an
alternative. Windows keeps a per-adapter GUID with a friendly name in
the registry. FreeBSD never renames at all -- `re0` is the driver plus a
unit number, and moving the card changes it.

**Why not `enp3s0` here.** It encodes the SOCKET. The machine this OS is
actually tested on has a USB Ethernet adapter that gets moved between
ports, and under that scheme every move is a new interface with a new
lease file and a broken socket binding. Linux can afford it because
servers do not get rearranged and because udev offers `enx<mac>` for
people who need otherwise; this has two cards and one of them is on a
cable that moves. So the identity is the CARD: the last three bytes of
the MAC, which are the part a vendor assigns per device.

**And the location is less stable than it looks.** On 2026-09-05 the
USB adapter on the test laptop reported `at usb12` on one boot and
`at usb1` on the next without being moved: a USB3 controller gives the
same physical socket two port numbers, one per speed range, and the
adapter had fallen back from SuperSpeed to high-speed. A
location-derived name would have renamed an interface because of a
link-speed renegotiation. That was not the reason the MAC was chosen,
and it is a better one than the reason that was.

**Location did not go away, it stopped being the name.** `dev->location`
carries `pci3.0` or `usb13` and `netctl` prints it as `at pci3.0`, so
a card is still findable physically. Splitting them is the whole point:
one answers "which card is this" and never changes, the other answers
"where is it right now" and changes freely. A single string cannot do
both, which is the mistake `enp3s0` makes.

**Why the kernel does not read /etc/net.conf.** A MAC-derived name is
correct and useless to type. The rules that turn it into `lan` live in a
file, and the file is read by `/bin/netd` in ring 3, which renames
through `SYS_NET_RENAME` -- the kernel validates a name and applies it
and has no opinion about what it should be. That is udev renaming what
the kernel called `eth0`, and it is the same split already made for NTP
(`SYS_SETTIME` plus a ring-3 client), DHCP and DNS. Doing it in the
kernel would also have needed a rename pass at `INIT_CONFIG`, because
cards register at PCI bind long before a filesystem is mounted -- a
moving part that exists only to work around being in the wrong ring.

## A network daemon, rather than a bigger DHCP client

`/bin/dhcp -k` was resident and supervised one card. On a machine with
two it renewed the first and left the second's lease to expire, and its
own comment recorded the assumption that had stopped being true: "no
machine here has ever held two leases at once".

The obvious fix -- make `dhcp -k` supervise every card -- was tried on
paper and is the wrong shape twice over. Its loop SLEEPS between
renewals, so the second card's timer waits behind the first's; and its
carrier wait is ten seconds, so a port with no cable in it stalls every
other card on every pass. Both are correct behaviour for a command
somebody typed and wrong for a daemon, which is the tell that the two
want to be different programs.

**What everyone else does.** Linux splits it three ways: the kernel
names a card, udev renames it from rules, and networkd/NetworkManager/
dhcpcd hold addresses. Windows has NDIS plus a user-mode DHCP Client
service. dhcpcd historically ran a process per interface; systemd-
networkd runs one process for all of them. Nobody puts naming policy in
the kernel.

**Per-card processes were the other candidate here**, and would have
reused `/bin/dhcp` unchanged -- netd spawning `dhcp -k <card>` each. It
was rejected for the sleeping: each child still blocks its own ten
seconds on carrier, and supervising N children to notice cards
appearing and going away is most of a daemon anyway. Inverting the
loop instead -- `udhcp_step()` returns a deadline and never sleeps --
made one process serve N cards in about 150 lines, and made
`/bin/dhcp` a front end over the same library rather than a second
implementation.

**The rules are a file because the alternative was a rebuild.** A
MAC-derived name is correct and useless to type; `lan` is what a person
wants. Compiling that in would mean a kernel change to rename a card,
which is the thing udev exists to avoid. `/etc/net.conf` carries the
scheme, the prefix and per-card overrides, and is re-read on every pass
so an edit needs no restart -- the file is under 2 KB and the pass is
seconds apart, so caching it would buy nothing and cost the property
that makes it a rules file.

**The name is applied once, at discovery.** Re-applying on every pass
would let an edit rename a live interface, and a socket bound by name
(`socket.c` keeps a string) would silently follow to whatever now holds
that name -- the hazard the naming scheme was changed to remove. A
rename takes effect on the next boot, or when the card is replugged.

## Shared memory is a NAMED object, and the name is the point

`SYS_SHM_OPEN` creates a run of kernel-owned frames under a name; a
process maps it with `SYS_MMAP`'s `MAP_SHARED`. It arrived because two
ring-3 processes here could not share a byte: every cross-process
mapping was bespoke and kernel-managed -- the sound ring to its one
owner, a window buffer to the registered compositor -- and `MAP_SHARED`
was refused outright.

**The NAMESPACE is the harder half, and it is what was actually
missing.** Shared frames alone are useless when neither side can say
which frames: there are no unix sockets here, no fd passing and no
connect-by-name, so two processes that never shared a parent had no way
to agree on anything. A name in a kernel-held namespace is that
agreement, and `QUERY_SHM` -- which lists every object with its size,
reference count and creating pid -- is how a server finds the clients
that have opened a channel to it. The roadmap had this the right way
round before it was built: *"the missing primitive is rendezvous, not
shared memory"*.

**Why not the compositor's pattern.** Windows already do cross-process
sharing here: `WIN_REQ_SET_COMPOSITOR` registers one role and
`WIN_REQ_MAP_WINDOW` lets that role map another process's buffer. A
sound server could have been a second copy of that -- kernel-owned
per-client rings plus a privileged mapper -- and it would have been less
code. It was rejected because the third caller would have been a third
copy: a settings daemon and `AF_UNIX` both want the same thing, and
three bespoke role-gated mechanisms is the shape this project deletes
wherever it finds one. POSIX's `shm_open` + `mmap` is the portable
spelling and costs one syscall pair.

**Two reference holders, and both are counted.** A descriptor and a
mapping. POSIX says `close(2)` leaves an existing mapping alone, so the
mapping table in `kernel/mm/shm.c` is not redundant with the fd table --
it is the half that survives the close, and without it the frames are
freed under a live mapping. An unlinked object keeps working for
everyone already holding it and merely accepts no new openers, which is
what makes "no new clients" expressible.

**Frames are zeroed at CREATION, not at fault.** A private mapping can
zero on first touch because each mapper faults separately; a shared one
has no per-mapper first touch to hang it on, and the second mapper must
never read what a previous owner of those frames left behind.

**A DEAD CREATOR'S NAME IS RELEASED AT ONCE, and that is not tidiness --
it breaks a deadlock.** A server holds a reference to each client's
object, so the object outlives the client; while it lives the NAME is
still in the namespace, and the namespace is the only thing the server
can watch. So the server never learns the client is gone, its own
reference is what keeps the name alive, and the next process to reuse
that pid unlinks a name it cannot free and creates a SECOND object
behind it -- which the server, still matching by name against the
corpse, never adopts. The client's ring is then read by nobody. That is
not a hypothetical: it is why `aplay` played its file and never exited,
found on real hardware after three wrong theories, and it took two
halves to fix -- the kernel releasing the name here, and the SERVER
skipping anything flagged `QUERY_SHM_UNLINKED`. Either alone leaves the
loop intact.

**No permissions, stated rather than implied.** Any process may open any
name. This system has no users, so a mode argument would be decoration
-- and a shared object is exactly as private as its name is unguessable,
which is not a security property and is not claimed as one.

**What it is not.** The size is fixed at creation: there is no
`ftruncate`, which is why `sys_shm_open()` takes a length and is
deliberately NOT spelled `shm_open(3)` in tolibc -- a function with that
name taking arguments that mean something else is worse than an honest
local one. The POSIX pair is on the roadmap.

## The kernel still never mixes: a sound DAEMON owns the one stream

`/bin/soundd` opens the machine's single exclusive PCM stream and mixes
every client into it. Nothing about the kernel's contract changed --
`SYS_SND_OPEN` is still exclusive, still refuses a second opener with
`-EBUSY`, and still knows nothing about mixing.

**Why a daemon and not kernel mixing.** The original sound decision
already said it: *"If two audible apps ever matter here, the answer is a
userspace sound daemon owning the one stream."* Mixing drags resampling,
format policy and per-client state in with it, which is why ALSA's
`dmix` is a library, PulseAudio and PipeWire are daemons, Windows' audio
engine is a service, and Android's AudioFlinger is one too. A malformed
stream can at worst take down the daemon, which init restarts.

**The problem it fixes was not theoretical.** An app asks for sound once,
at startup, and treats failure as "no sound is not an error" -- so
starting Minesweeper while the Audio Player held the card left the game
**silent for its entire life**, with the only notice in a log. It was
reported as "audio does not work on this machine", and the machine's
driver was fine.

**A client ring is the KERNEL's ring, shape for shape.** The daemon
plays the part the hardware plays: it advances `hw_pos` as it consumes
and zeroes each chunk before moving past it. That one inherited rule is
why a client that dies needs no cleanup path in the daemon at all -- its
ring goes quiet instead of looping -- and why `usnd_sink.h`'s two rows
share their cursor arithmetic.

**It blocks on TIME, never on its clients.** There is no `poll()` here,
so a server that waited on N clients would need N threads. The hardware
ring's position is the clock instead: the daemon wakes on a timer, mixes
whatever each client has left it, and a client that stopped writing
contributes silence. That is what lets one single-threaded loop serve
every client on a system with no readiness primitive.

**It still holds the card while it runs**, so a program opening
`SYS_SND_OPEN` directly gets `-EBUSY` -- `/tests/tone` and the `sound`
KTESTs both do, deliberately, and both need `service stop soundd` first.
Releasing the device when no client is playing is PipeWire's
`suspend-on-idle` and is on the roadmap; it was not built here because a
reopen that loses the race introduces a failure mode the exclusive hold
does not have.

## The tick is a registry, not an `if`, and the PIT is masked rather than stopped

The tick moved from the 8259-routed PIT to the LAPIC timer behind
`kernel/clockevent.h` -- a device with a `start`/`stop`/`rating`, the
same shape as `clocksource`, `display_driver` and `block_device`.

**Why a registry for two devices**, when this project's usual bar is a
second real caller rather than a plausible one. There are two real
implementations today, so the bar is met -- but the argument that
decided it is stage 3 of `docs/smp-design.md`. A per-core timer means
one clockevent instance per core, and the thing an application processor
needs at bringup is exactly "give me the tick device and let me start
it". An `if (lapic_present())` in `idt_init()` would have had to become
this anyway, with the per-core work layered on top; building the seam
now costs one file and a header.

**Why the PIT is masked rather than stopped.** Channel 0 keeps counting
when the LAPIC takes over. Anything calibrating against it still can --
including the LAPIC timer's own calibration, which is what makes
re-taking the tick a single write rather than a reprogram. Linux does
the same thing for the same reason.

**Why calibration is in `kernel_main()` and not in `lapic_init()`.** The
LAPIC timer counts at a bus frequency nothing reports, so it has to be
measured, and the only reference this early is the PIT. `coarse_ticks()`
advances only from the timer interrupt, so calibrating with interrupts
off waits forever -- the deadlock `cpuinfo.h` already describes for the
TSC, which is why `cpu_info_init()` is a separate call too. The
calibration reads RFLAGS and refuses rather than hanging, so getting the
ordering wrong is a boot that keeps the PIT rather than a boot that
stops.

**What was NOT done, deliberately.** TSC-deadline mode, which is what
modern Linux prefers: it needs no calibration, but it is one-shot, so
the tick has to re-arm on every interrupt and the periodic path would
still exist for machines without it. Two mechanisms for one tick is
worth it when tickless idle arrives and not before. And `coarse_ticks()`
kept its name through this change even though the PIT no longer feeds
it -- 163 call sites in 43 files, renamed separately so the interesting
diff stayed readable.

## A TLS client asks how good the randomness is, and refuses rather than warning

`SYS_GETRANDOM` always answers and deliberately never grades its answer.
`abi/syscall_abi.h` says why: a ring-3 program handed a quality flag
"would mostly use it to decide to carry on anyway". That reasoning holds
for the general case and breaks for exactly one caller -- a private key.

Under an emulator with no virtio-rng the source is TSC jitter, where the
"hardware" being timed is itself software. A TLS key drawn from it is
not secret from anyone who can model the emulator, and the connection
that key protects is theatre.

**So the grade is read from `QUERY_RANDOM`, not from the syscall**, and
nothing about `SYS_GETRANDOM` changed. The two answer different
questions -- "are these bytes usable?" asked per draw by code, versus
"what is this machine's entropy source?" asked once -- and the fact
class already existed for the second. Adding a quality flag to the
syscall would have attached a promise to it that this kernel cannot
keep, for one caller's benefit.

**It REFUSES rather than warning.** A warning printed above a page of
HTML is a warning nobody reads, and the failure mode it guards against
is silent by construction: a weak key produces a handshake that
completes perfectly. `/bin/wget --weak-entropy` is the override, and the
threshold is `>= QUERY_RANDOM_VIRTIO` rather than a list, because the
enum is ordered by trust -- a stronger source added later needs no
change here.

**The library states the condition and the program names the flag.**
`utls_connect()` says the randomness is too weak and what would fix it;
it does not mention `--weak-entropy`, because `<utls.h>` has three
callers coming and `-k` is already wget's spelling for something else
entirely. A library naming a particular program's flags is a wrong
sentence waiting for the second caller.

## HTTP and TLS are separate libraries, and neither may live in `userland/lib/`

`/lib/libhttp.so` (`<uhttp.h>`) speaks HTTP over a transport;
`/lib/libssl.so` (`<utls.h>`) is that transport when the scheme is
https. The split is visible in the headers: **`<utls.h>` names no engine
and `<uhttp.h>` names no cipher.** Replacing mbedTLS is a backend change
rather than a change to every caller -- not hypothetical tidiness, since
BearSSL was measured against it first and the loser lost on maintenance,
which can change.

**Neither can live in `userland/lib/`, and the reason is a build fact
rather than taste.** That directory is globbed wholesale into
`libuapp.a` and `libuapp.so`, which every program links -- so a TLS call
there would put mbedTLS behind every binary in the system, including
`init` and the window manager. `userland/dynlib/` is where a shared
library's implementation goes, and a program opts in with
`ULIB_SO_<name>`.

The same argument, one layer in, kept `ufile_slurp()` out of
`utls_mbedtls.c`: it lives in `libuapp`, and a crypto library depending
on the widget toolkit is backwards. The anchors are read with `fopen`
and `fread`, which are the C library rather than a hand-rolled loop.

**`MMAP_MAX_REGIONS` had to double for this, and the arithmetic is worth
knowing.** ld-toy carves each `PT_LOAD` into its own `MAP_FIXED` region
and ld gives a `.so` four of them, so the ceiling is mostly a program's
DT_NEEDED list rather than a count of the `mmap()` calls it makes -- a
program calling `mmap()` never can still exhaust it. wget's fourth
library took it past 16, and the loader failed on the LAST one with
"segment map failed", naming the library that ran out rather than the
one that filled it.

## The compositor opens a client's buffer by name, and the client is what grants it

Stage 5b of `docs/winserver-ring3-design.md`. A window's pixels were the
client's own shm object already (5a), but the KERNEL still mapped them
into the compositor at an address it carved per (pid, window) --
`comp_map()`, `WIN_REQ_MAP_WINDOW`, and the machinery that had grown
around revoking that mapping safely: `comp_span`, `comp_poisoned`, the
poison page, a retired slot, a recorded reference. The compositor opens
the name itself now and maps it wherever its own `mmap` puts it, and all
of that is gone with `adopt_buf()` beside it. `struct win_buf` is
`{ w, h, gen }`.

**THE CLIENT GRANTS, NOT THE KERNEL**, and the alternative was close.
A kernel-side `shm_grant()` at create time is strictly safer today: it
happens inside the syscall so it cannot be missed or arrive late, and it
needs no beacon. It was rejected because stage 6 deletes the hook it
hangs on -- once a client's create goes to the compositor over the
channel, the kernel never sees a window being created and has no moment
at which to grant anything. The client granting is Wayland's shape (a
client passes an fd; here it names a pid) and is the version that
survives.

The argument that nearly decided it the other way was wrong, and
checking it is what changed the answer: a kernel grant was supposed to
survive a compositor HANDOVER, re-granting every live window as the role
moved. It buys nothing. TWP has no window-enumeration request, so a
compositor only ever learns about windows through
`WIN_EV_CLIENT_CREATED` -- a handover with live windows was already
unsupported, and had been since the role existed.

**THE COST IS THAT A CLIENT WITH NO COMPOSITOR CHANNEL CANNOT OPEN A
WINDOW AT ALL**, where before it merely had no title. Toykit refuses the
create rather than opening a window that draws, presents and never
appears -- the loud version of the same failure, and what stage 6 makes
unavoidable anyway.

**A GRANT LIVES ON THE OBJECT, NOT ON THE NAME.** A resize unlinks the
object and creates a new one under the same name, and the new one's
grant list is empty. So the grant belongs beside every create, in
`buf_make()`, not once at startup: missed, the first resize would be the
last frame the compositor ever saw of that window.

**THE GENERATION IS ON THE PRESENT, not an event of its own.** The name
identifies the slot and never changes; the object under it does. So a
present carries which buffer is now front AND that buffer's generation,
packed into one `int32_t` (`WIN_PRESENT_B`), and a compositor holding a
different one re-opens the name. Three things follow. It is STATE rather
than a notification, so a lost or coalesced event costs one frame
instead of leaving a compositor reading a freed object forever -- which
matters here because the event queue is 32 deep and sheds the oldest
under a present flood. The compositor only ever reads the FRONT buffer,
so it maps lazily and never has to hear about a back buffer being
replaced. And the field belongs to the present MESSAGE rather than to a
carriage: after stage 6 that message travels on the channel instead, and
it goes with it unchanged.

**AND THE SIZE IS NOT A PROXY FOR THE OBJECT.** The generation goes up
whenever a client says it replaced a buffer, never only when the
dimensions changed: re-creating at the same size makes a new object
under the same name, and a compositor left on the old one shows a window
frozen at its last frame. A drag proposing the size a window already
has, and a restored geometry that matches, both reach it.

**THE SIZE CHECK MOVED TO THE READER, AND MAPPING IT IS THE CHECK.** The
kernel used to refuse a window whose object was smaller than the size
claimed; holding no object, it cannot. The compositor asks `mmap` for
exactly the extent the present claims, and `SYS_MMAP` already refuses a
length past an shm object's own pages -- so the check lives in the one
place that knows both numbers, with no new syscall and nothing to keep
in step. A client that lies costs its own window a frame.

What this removes from the ring-3 address map is worth stating, because
the map is read as a whole: `WIN_CLIENT_BASE`'s 64 MiB-per-window region
and `WIN_COMPOSITOR_BASE`'s 16 GiB of one slot per (pid, window) both
go. What is left above the stack is the font mapping and the
framebuffer grant.

## Out-of-order segments live in the receive buffer, not in a queue beside it

Linux holds them in a separate structure -- `tp->out_of_order_queue`, a
list until 4.4 and an rbtree since, one entry per skb -- and every
textbook draws reassembly as a hole list with its own storage. toy-os
does not, and the reason is an equivalence the textbook version does not
have available.

The window this stack advertises is exactly the free space in the
receive buffer (`TCP_RCV_BUF - rcv_len`). So the sequence range the peer
is permitted to send, `[rcv_nxt, rcv_nxt + window)`, maps one-to-one
onto the free bytes of `rcv`: a byte the window admits ALWAYS has an
offset waiting for it, `rcv_len + (seq - rcv_nxt)`, and that offset is
never occupied by anything else. Out-of-order data is therefore not
something to store somewhere -- it is already home the moment it is
copied. What is left to remember is which ranges are filled, which is
`ofo[]`: four pairs of sequence numbers, 32 bytes per connection.

That buys three things. **Nothing allocates**, so there is no queue to
size, no per-segment metadata, and no failure mode where reassembly
works until memory is tight. **No payload is ever copied twice** --
`ofo_drain()` raises `rcv_len` and `rcv_nxt` together and deletes a
range; the bytes do not move. And **the window cannot overcommit**: a
stack with a separate queue advertises space it may not be able to keep,
which is why Linux prunes and collapses its queue under pressure. Here
the promise and the storage are the same number.

The cost is real and worth stating. A separate queue can hold a segment
the window would refuse; this cannot, so a peer that runs ahead of the
window loses those bytes and resends them. And the offset identity has
to be maintained by everything that touches the buffer. (It was a
linear buffer compacted by `rcv_len + ofo_span()` on every read; it is
a ring now, where a read advances the head by what it takes and the
offsets stay put.) Moving the in-order part without the held part
delivers the held bytes displaced by however much the reader took, as
WRONG DATA rather than as a short read: the failure is silent, and a
test that only reads everything at once cannot see it.

The bound is `TCP_OFO_MAX` disjoint ranges, not bytes. It was four
against an 8 KiB window; with a 256 KiB one it is sixteen, which does
not cover the worst case (alternating loss across ~180 segments) and is
not meant to: a range past the list is refused and re-acked, which is
the same answer the stack gave to everything out of order before this
existed.

## TCP throughput: window scaling, NewReno, fixed rings, one guard

**The problem.** A connection could never have more than 8 KiB in
flight towards it (the receive buffer) or 4 KiB away from it (the send
buffer), and the sender put one segment on the wire per ACK. Over a
path with any latency that was the ceiling -- about 3 Mbit/s at 20 ms
-- whatever the link, which is what a speed test would have measured.

**What real systems do.** Linux negotiates window scaling (RFC 7323)
by default and autotunes each connection's buffers up to several MiB
(`tcp_rmem`/`tcp_wmem`); its congestion control is CUBIC, with an
initial window of ten segments (RFC 6928) and SACK-based recovery.
Windows autotunes the receive window the same way and has used CUBIC
since Windows 10 1709. Both hold a per-socket lock that the receive
softirq/DPC and the system call contend for.

**What toy-os does, and where it deliberately stops short.**

- **Window scaling, as RFC 7323 says** -- offered on every SYN,
  answered only when offered, both shifts or neither. Nothing to
  differ on.
- **Fixed rings, not autotuning**: 256 KiB receive, 64 KiB send, per
  connection, halved until the heap (whose runs are physically
  contiguous) can supply them. Autotuning is a feedback loop on the
  measured RTT, and this stack has no RTT estimate; 256 KiB covers
  ~100 Mbit/s at 20 ms, which is past what the drivers here have been
  measured to move.
- **NewReno (RFC 5681 + 6582), not CUBIC.** CUBIC's advantage is
  regrowing a very large window quickly after a loss on a long fat
  path; it needs a clock-driven cubic function and an RTT estimate,
  and it matters above the window sizes these rings allow. NewReno is
  the RFC baseline every stack must interoperate with and is a few
  dozen lines. Without SACK, recovery fixes one hole per round trip
  (the partial-ACK rule); a burst loss costs several RTTs, not a
  timeout.
- **A timeout goes back to `snd_una`** (go-back-N): with no SACK there
  is no telling which later segments arrived, and resending only the
  first one left the rest to their own timeouts.
- **One guard over every entry point, not a socket lock.** One core, so
  the only concurrency is the scheduler; the preemption guard is the
  whole lock and cannot deadlock. A sleeping mutex would add waits
  where there are none. The cost: a send filling a large window runs
  with preemption off for as many segments as the window admits.

**A blocking stream write came with it.** A full send ring used to
return `EAGAIN` to a blocking descriptor, which a program had to spin
on; it parks now until an ACK arrives, which is POSIX's blocking write
and what an upload loop needs.

## The TCP receive buffer is guarded by disabling preemption, not by a lock

`tcp_recv()` compacts the receive buffer with a memmove; `tcp_input()`
writes arriving segments into the same buffer. (Since window scaling the
buffer is a ring and the read advances a head instead of moving bytes;
the race below is the same, between the copy-out and the advance.) Both run in SYSCALL
context, and a ring-3 process is preemptible inside a syscall — so a
reader can be stopped between copying bytes out and shifting the rest
down, and `net_poll()` (reached from `scheduler_idle()` and from a dozen
syscalls) can run `tcp_input()` into that buffer before it resumes. The
reader then shifts a segment that was never there when it measured.

The obvious answer is a lock per connection. It is the wrong one HERE
for the same reason `vfs.c` reached for the same guard: this kernel is
single-core, so the only concurrency is the scheduler, and a mutex would
have to be a sleeping one (a spinlock cannot be held by a process that
can be descheduled). A sleeping lock introduces a wait where the
existing code has none, needs a wakeup path, and can be held by a
process that then blocks — turning a data race into a lifetime problem.
Turning preemption off for the few hundred instructions of a memmove
costs nothing and cannot deadlock.

**BOTH ends need it, and that is not belt-and-braces.** Guarding only
the reader stops the reader being interrupted, but a writer preempted
midway through placing a segment leaves the same torn state for a reader
that starts afterwards. Either side alone is a half-closed race.

**What this buys and what it does not.** It makes the buffer's
manipulation atomic with respect to the scheduler, which is the whole
hazard on one core. It is NOT a lock and will not survive SMP:
`docs/smp-design.md`'s big-kernel-lock stage is where this becomes a
real lock, and the guard is one of the places that has to be revisited
then. It is also not the filesystem's guard — `FS_OP()` protects the
backend's module-level scratch across a whole call; this protects one
connection's buffer across one manipulation.

The cost of NOT having it, measured: a 16 MB download came back with the
right length and the wrong bytes in 5 runs of 14, the damage being one
MSS-sized window holding the stream's own data from a few hundred bytes
earlier. Nothing reported anything — no error, no short read, no log
line. Zero in 12 with the guard.

## The argv vector is a flag on `SYS_SPAWN` and is sized by a length, not a terminator

`tosh` gained quoting on 2026-09-10, and quoting is worthless if the
kernel re-splits the result: `spawn_msg.args` was a space-joined string
the loader tokenised, so `echo "a b"` could parse perfectly in the shell
and still arrive as two arguments. Three ways to fix that were on the
table.

**Escape the string** -- backslash spaces, have the kernel unescape.
Smallest change, and the design `CreateProcess` has: a command LINE, with
every program (and `CommandLineToArgvW`) re-parsing it by rules nobody
quite agrees on. Every future spawner would have to learn the escape.
Rejected.

**A new field, or a new syscall.** `spawn_msg` grew a `flags` word
precisely so a capability of the act of starting a process could be
added without a second spawn; the file's own comment says the next
thing goes in the struct. A FLAG (`SPAWN_ARGV`) reinterpreting the
existing `args` pointer, plus one length field read only when it is
set, leaves every caller that predates it byte-for-byte unchanged --
and an old kernel refuses the unknown flag rather than silently
splitting the vector, which is what the "unknown bits are -EINVAL" rule
was for. Chosen.

**A vector, execve's shape,** was never in doubt; what its ENCODING
should be was got wrong once. The first version copied `env`'s
"a\0b\0\0" run, and the test's empty argument silently ended the
vector: `"a b", "", "c"` arrived as `"a b"`. The empty string is the
terminator, so the format cannot express it -- a known ambiguity of
Linux's `/proc/<pid>/cmdline`. `args_len` replaced the terminator; the
environment keeps its shape because an environment entry is never
empty. The vector includes argv[0], as execve's does, so a shell can
one day set it to what was typed rather than the path.

The kernel-internal representation became the vector as well, rather
than threading an "is this a string?" flag down to the loader: the
string form is split at the two edges it enters (`sys_spawn()` and the
kernel-side `scheduler_spawn_env()`/`elf_run_from_fs()` wrappers), and
`spawn_from_fs()`, `scheduler_spawn_group()` and
`elf_build_argv_on_stack()` know only vectors. Splitting once at the
edge is the same call as `elf_argv_from_string()` being one function:
two tokenisers would be two places to disagree about what a space is.

## fork shares frames through a refcount in `pmm`, and a shared frame is never swapped

Asked and answered 2026-09-11, when `fork()` was built (`docs/fork-design.md`).
The obvious way to fork is to copy every page; the way every real
system does it is copy-on-write, which needs to know how many address
spaces reference a frame. Linux keeps that count in `struct page` and a
reverse map beside it; NT in the PFN database. toy-os had two bitmaps
and nothing per frame.

**The count lives in `pmm`, and `pmm_free_frame()` DECREMENTS.** A
`uint16_t` per frame beside the bitmaps, set to 1 by every allocation,
raised by `pmm_frame_ref()`, and the last owner's free is the one that
frees. Putting the decrement inside the existing free call -- rather
than adding a `pmm_unref()` for shared frames -- is what made every
teardown walk correct with no edit: `destroy_pt()`,
`vmm_release_user_page()` and `vmm_set_swap_entry()` all call
`pmm_free_frame()` and none of them can tell a shared frame from a
private one, which is exactly the property wanted. A double free, which
used to be a silent no-op, is logged now for the same reason.

**No reverse map, still.** The swap entry above records why none was
built; fork keeps that premise by refusing a frame with two owners as a
victim. The cost is that a forked process's shared pages cannot be
evicted until one side un-shares them -- acceptable while page-out is
a debug command, and the entry to revisit when it is not.

**The COW bit is in the PTE, not a side table**, for the reason
`PAGE_BORROWED` is: the teardown and fault walks have the PTE in hand
and nothing else. Bit 11 was the last free software bit.

## A fork copies eagerly what the kernel holds a physical pointer into

The futex is keyed by the WORD's PHYSICAL address (`futex_key()`,
`kernel/proc/futex.c`), a waiter parks on that address as its channel,
and the wakeword is bumped through one. Copy-on-write breaks that
quietly: the parent forks, its waiter stays parked on frame P, the
parent writes the page and is moved to a private copy P' -- and every
later wake in the parent keys to P', finds nobody, and the waiter never
runs. A lost wakeup, in the process that did nothing unusual.

Linux does not have this problem because a private futex is keyed by
`(mm, vaddr)`, and a shared one by the page's `struct page` -- the key
survives a COW break. Re-keying here would mean either that (a futex
namespace per address space, which is a real change to a working
mechanism) or a hook from `vmm_cow_break()` back into the scheduler to
move channels, for one consumer.

**Chosen: the fork walk takes a list of PINNED frames and copies those
eagerly for the child**, so the parent is never un-shared away from
them. `scheduler_fork()` pins the wakeword's frame and every frame a
parked thread of the group has as its `wait_chan`. It is a list rather
than a rule because the scheduler is the only thing that knows where its
waiters are parked, and vmm should not learn. The cost is one extra
page copy per parked waiter at fork time -- and the one hazard left is
documented in `docs/fork-design.md`: until the child un-shares such a
page, a wait in the child joins the parent's waiters, which is a
spurious wakeup and not a lost one.

## An exec builds the new image before it touches the old one

POSIX says a failed `exec` returns -1 with the process unchanged, and
Linux honours it up to `flush_old_exec()` -- the point of no return is
as late as the checks can be pushed. toy-os pushes it later still: the
whole new address space, interpreter and stack are built by
`build_image()` (the half `spawn_from_fs()` already had) into a FRESH
PML4 while the caller's is untouched, and only a fully loaded image is
swapped in. A missing file, an unreadable ELF or an absent interpreter
all return `-errno` to a caller still running its old code; the
`fork_test` check "exec of a missing path returns -1/ENOENT to a live
process" is the positive statement of it.

**Why a fresh PML4 rather than reloading into the caller's:** the fd
table, the strace arm, shm maps, the futex wakeword and the accounting
are all keyed by the PML4's physical address, and every one of them
already has a "process gone" hook. Keeping the CR3 value would have
avoided `fd_rekey()` and `strace_rekey()` -- two small functions --
at the price of tearing down first, which is the failure mode above.

**`SYS_EXEC` takes `struct spawn_msg`.** An exec is a spawn into the
caller's own slot, and giving it a second message shape would have
been a second copy of the argv/env carriage and its refusals. The
fields only a child can take -- a stream, a group, `SPAWN_FOREGROUND`,
`SPAWN_TRACE` -- are refused with `-EINVAL` rather than ignored, the
same rule the flags word has always had.

**And no `CLOEXEC`.** A fork copies the whole descriptor table, an
exec keeps it, and nothing can mark one to close. The decision that
capped `spawn`'s inheritance at 0/1/2 already said a future fork would
copy everything; `fcntl(F_DUPFD, FD_CLOEXEC)` is on the ported-shell
list and the flag is one bit in `struct fd_space` when a program needs
it. A shell written against fork closes what it must in the child, as
every shell does.

## The timezone left the kernel, and what stayed behind is the SELECTION

**What moved.** The city database, the two DST rules and every
conversion from UTC to a local time are ring 3's, in the C library
(`userland/libc/tz.c`). `kernel/lib/tz.c` went from 663 lines to 58.
The kernel's clock was already UTC; what it did with that was convert
at the syscall boundary, so `SYS_GETTIME` answered in local civil time
and every filesystem timestamp was a local-derived epoch.

**Why.** Three reckonings that looked alike. `ktime` was UTC,
`SYS_GETTIME` was local, `SYS_SETTIME` took UTC and libc's `time()` was
local-derived -- so a client that read the clock, added a second and
passed it back moved the machine by the timezone offset, and
`QUERY_CLOCK` existed only because nothing else could report UTC at
all. One reckoning removes the whole class. And a 92-city table with
two legislatures' daylight-saving rules is policy, which is the same
argument that moved the font rasteriser, the image decoders and the
audio mixer out: the kernel is where mechanism goes.

**What a C library does.** This is glibc's shape, not an invention.
Linux's kernel knows UTC and an offset it is told; the zoneinfo
database is read by libc, and `tzset()` is the call that re-reads it.
toy-os was the outlier, and the roadmap had said so for months.

**What stayed, and why it is not a half-move.** One registered setting,
`system.timezone`, whose value is a city name. The kernel does no time
arithmetic with it and never reads an offset or a rule -- but System
Settings still needs a dropdown of 92 cities, and the registry is
kernel-side.

**So a setting's choices can be a FILE** (`setting.h`'s `choice_file`).
The registry reads the lines of a named file, takes the first
comma-separated field as the value and the last as the label, and
knows nothing else about them. `/etc/timezones` is a city database with
offsets and DST rules to the library that reads it, and an opaque list
of names to the kernel. The alternative -- keeping a name table in ring
0 purely to enumerate it -- is the database again, smaller.

It is cached on the filesystem generation, because enumerating is
O(choices) by construction: System Settings asks for all 92 rows to
fill one dropdown, and a whole-file read per row is the shape that made
a nine-entry desktop reload cost 54 of them. The mechanism generalises
to the next setting whose options are data rather than an enum -- the
cursor themes wanted exactly this, and got it when they became a
declaration with a `ChoiceDir=` instead (see "A setting the kernel does
not apply is DECLARED BY A FILE" above).

**And the registry became the gate for an enum's value.** An enum with
a choice list now refuses a value that is not one of them, in
`setting_set()`, for the same reason the integer range is checked
there: `config set` and a hand-edited `/etc` file reach it without
passing through any control. It used to be each setting's own `apply`
that refused an unknown value, which worked only for the ones that
remembered to.

**The two costs, stated.** A FAT32 timestamp is now written in UTC,
where the format specifies local time -- that is Linux's `tz=UTC` vfat
option made the only behaviour, because a filesystem write cannot ask a
ring-3 library for an offset. And a disk written before this change has
local-derived timestamps mixed with UTC ones; nothing converts them,
and on a hobby OS with a seeded image that is a `make clean-disk` away.

## The syscall stall histogram times with `rdtsc`, not with the clocksource

The interruptible-syscall work in `docs/roadmap.md` needs a number for
"the desktop feels slow under disk I/O". The compositor already reports
the EFFECT (`gui latency`'s `wake` overshoot); what was missing was the
attribution -- which syscall spent the time -- and the obvious way to
get it is to bracket the handler in `syscall_dispatch()` with
`clocksource_now_ns()`, the kernel's one monotonic clock.

That measures nothing, on every default boot. The clocksource is the
PIT, and its `read` is `coarse_ticks()` -- a counter the timer INTERRUPT
increments. A syscall handler runs with interrupts off for its whole
duration, so both reads return the same value and every stall is zero.
It is not a precision problem that a better source would improve; it is
the instrument being blind to exactly the window it is pointed at, and
the failure is silent: a full table of syscalls, every one reporting no
time, reads as a machine with nothing wrong. The all-zero first run is
what found it.

So this one subsystem keeps a clock of its own: `arch_rdtsc()`,
converted with the TSC frequency `cpu_info` already calibrates against
the PIT at boot. The TSC counts with IF clear, costs a couple of dozen
cycles, needs no I/O port, and is the same register ftrace uses for its
default trace clock.

**The cost is accepted rather than fixed.** Without an *invariant* TSC
the counter's rate changes as the CPU throttles, which is the exact
hazard `clocksource_tsc.c` refuses to register for -- and refusing here
on the same grounds would leave the default QEMU boot, where this work
actually gets debugged, with no instrument at all. The difference is
what the number is FOR: the clocksource's durations feed deadlines and
accounting that must stay correct over hours, while this measures a
single millisecond-scale interval and is read as a comparison between
two runs on one machine. A rate that is wrong by a few percent does not
change the answer to "did this stall get shorter".

The refusal that IS kept is a machine with no calibrated frequency at
all: arming returns 0 and `config set kernel.syscall_stall on` fails,
because converting ticks to microseconds with a guessed divisor would
produce plausible numbers rather than no numbers.

**And the two halves stay separate on purpose.** The compositor's
distribution could have been extended with a syscall breakdown, or the
kernel's table given its own overshoot figure, and either would make one
report instead of two. They are independent implementations measuring
opposite ends of the same event, which is what let the first real run
corroborate itself: `wake` max 207 ms and `ping` max 209 ms against
`write`'s 220 ms worst handler, on a guest where neither instrument can
see the other's numbers.

## sigsuspend is the one wait that is never restarted

Every blocking syscall here is woken the same way: `scheduler_signal_raise()`
finds the parked process, rewinds RIP back over the two bytes of `int
$0x80`, and lets it re-enter the kernel at the same call with its
arguments untouched. That is what makes `SA_RESTART` possible at all,
and it is right for `read`, `waitpid` and every other call whose job is
unfinished when a signal interrupts it.

It is wrong for `sigsuspend`, whose job is finished the moment a signal
arrives. Restarted, it re-installs the same mask and parks again, and
the caller never reaches the line that reads what its handler set. A
shell's wait loop is exactly that shape -- `while (!gotsigchld)
sigsuspend(&oldmask);` -- so the symptom is a shell that hangs after its
first background job, with nothing in the log and the process sitting in
a legitimate-looking `block(signal)`.

So the wait REASON carries the exception: `SCHED_WAIT_SIGNAL` is woken
by writing `-EINTR` into the saved trapframe instead of rewinding.
Linux spells the same thing `ERESTARTNOHAND`, decided per syscall rather
than per wait; a reason is the cheaper carrier here because this kernel
has exactly one call that needs it and the reason is already stored.

**Three things had to be true besides, and each was found by a test
rather than by reading:**

- **The mask is unwound in TWO steps, not one.** POSIX runs the handler
  under the mask `sigsuspend` installed and restores the previous one
  when the handler returns. Restoring it before building the signal
  frame is the obvious implementation and it deadlocks the common case:
  a shell suspends with everything blocked, so the restored mask blocks
  the very signal that ended the wait and the handler never runs. The
  pre-suspend mask is therefore installed just long enough for
  `push_signal_frame()` to record it as the sigreturn mask, and the
  suspend mask is put back for the handler's duration. That is Linux's
  `saved_sigmask`.
- **The unwind must not happen while the process is still parked.** The
  trap that parks a process runs its own tail afterwards, where "armed"
  and "current" are both true and nothing has happened yet. Unwinding
  there put the old mask straight back and left the process asleep under
  it forever. `scheduler_sigsuspend_take()` refuses while the slot is
  `SCHED_BLOCKED`.
- **The wait channel cannot be the process slot's own address.**
  `scheduler_wait_chan_pid()` already returns it, and that is the channel
  a child's exit wakes with `SYS_RETRY` -- so a `sigsuspend` parked there
  returned -4095 the moment any child died, which reads exactly like the
  syscall being wrong rather than like a collision. It parks on the
  address of a field inside the slot instead.

**Why a syscall at all**, rather than `sigprocmask()` plus a `pause()`:
a signal arriving between the two is lost, and the process sleeps
forever. One syscall, with interrupts off across both the check and the
park, is what closes that window -- the same argument that makes
`scheduler_block_current()` safe against an IRQ pushing an event.

## The spawn's string form splits with quotes, because its caller has no lexer

`SYS_SPAWN` takes arguments two ways: a VECTOR (`SPAWN_ARGV`), which a
ring-3 shell builds after its own lexer has run, and a STRING, which
`elf_argv_from_string()` splits into a vector at the edge. The string
form split on spaces and nothing else, which made a quoted argument
impossible to express through it -- `tosh -c 'echo hi'` typed at a `#`
prompt arrived as `'echo` and `hi'`, was rejoined by tosh into one word
and looked up as a program named `echo hi`.

Linux has no such decision to make: `execve` takes a vector, the kernel
splits nothing, and every quoting rule lives in the shell. Windows does
the opposite -- `CreateProcess` takes a command LINE, and the callee's
runtime splits it (`CommandLineToArgvW`), which is why quoting rules on
Windows are a property of the C runtime rather than of `cmd.exe`.

toy-os keeps both forms, so it had to pick where the string form's
quoting lives, and there are only two answers:

- **Give the ring-0 shell a lexer** and always spawn a vector. It is the
  POSIX shape and it puts the rule in one place -- but the ring-0 shell
  is the `rescue` target, deliberately shrinking, and this would grow it
  by the one piece of code the ring-3 shell exists to own.
- **Split with quotes at the edge**, Windows' shape. One function, no
  new state, and it fixes every string-form caller at once: `rm "my
  file"` at `#` now means what it says.

The second, with the cost stated rather than hidden: there are now TWO
implementations of one set of rules, and this project's usual answer to
that is a shared case table (`kfmt_cases.h`, `klineedit_cases.h`). A
table is not workable here -- tosh's lexer also produces operators and a
word array, and the kernel's produces a NUL-separated vector -- so what
keeps them equal is that both answer the same CASES:
`userland/tests/argv_test.c` drives the string form and the vector form
through one probe, and `kernel/proc/elf_test.c` checks the splitter
directly, with the same inputs.

An unterminated quote is REFUSED rather than guessed at, so the spawn
fails and nothing runs -- the same rule every other parser here follows.
The caller sees a failed spawn, which is what the `#` shell reports.

## Log levels arrive in-band, and are stored as text in the timestamp

Adding a severity to the kernel log ran into a number before it ran into
a design: there are ~940 `klog_printf`/`klog_write` call sites across 117
files. Any shape needing a per-site edit — a level parameter, a
`klog_err()` family replacing the calls — meant touching all of them to
supply a value that only a few dozen actually have. So the question was
never "what is the tidiest signature", it was "what can arrive without a
sweep".

**Linux answers that with an in-band prefix**, and it answers it for the
same reason: `KERN_ERR` expands to `"\001" "3"` on the front of the
format string, `vprintk` strips it, and a call with no prefix is
`KERN_DEFAULT`. That is the only construction where an unmarked call
keeps working unchanged, which is the whole requirement. toy-os copies
it, including the numbers, so `<3>` means here what it means in anyone's
`dmesg`.

**Where it goes afterwards is the part that differs.** Linux's log has
been a record ring since the structured-printk rewrite in 3.5, with the
level as a 3-bit field; toy-os's `klog` stores BYTES, and making it
records would change `QUERY_KLOG`'s ABI, `dmesg`, `logd` and
`crash_report.c` together — a session's work before a single level was
useful. The cheaper option that is not merely cheaper: put it in the
ring's own **timestamp**. That text (`[7.03] `) is already written by
`klog_write_timestamp()` into the buffer only and has never reached
COM1, so a level living beside it costs no ABI, leaves `serial.log`
byte-identical for every harness that parses it, and makes the level
visible to every reader that can already read the log — `grep '<3>'
/var/log/toyos.log` works, and `cat` remains a working reader, which is
the property `logd`'s own design note protects. The cost is ~4 bytes per
line of a 16 KB ring and a marker readers must hide; `dmesg` and `log`
hide it by default, as util-linux does, and show it for `--raw`.

**The threshold gates the console and never the ring**, which is also
Linux's split (`console_loglevel` versus what `/dev/kmsg` still holds).
It matters more here than it does there: this project has already lost a
laptop's entire boot log to a driver logging once a second, and the fix
for that class is a level on the chatter plus a quiet console — a
threshold that dropped bytes from the ring would be deleting the
evidence the ring exists to keep.

**A stray marker is stripped rather than stored.** A line here is often
built from several writes — `ata.c`'s DMA failure line is six — so a
level put on the second fragment is an easy mistake, and one that would
otherwise write a raw `\001` into the log. `klog_write()` takes a marker
off wherever a write begins and only lets it change the level at a line
start: the worst case is a level that quietly does not apply, instead of
a corrupted line. Seven such fragments existed in the first sweep and
were moved onto their line's opening write; the rule is what makes the
eighth harmless.

## A dynamic program carries an ABI stamp, and the loader refuses a mismatch by name

`/bin/about` on the bare-metal laptop was left one build behind while
`/lib/libc.so` was replaced. It did not fail to start. It page-faulted
inside `__rt_tls_init`, writing into its own text segment, because an
old executable's static TLS geometry does not match a new libc's --
`error=0x7`, `cr2` in the R+E segment, and nothing anywhere near the
message "your binary is stale". Two hours went into that fault before
the cause turned out to be a sync that had skipped a file.

The fix is the cheap half of what every real system does. A shared
library declares the ABI it provides; a program records the one it was
built against; `ld-toy.so` compares them before relocating anything and
refuses with a sentence:

    ld-toy: built for userland ABI 99, this system provides 1: rebuild the program

**IT IS A STAMP, NOT SYMBOL VERSIONING, and that is the whole decision.**
glibc carries several ABIs in one file through version nodes
(`GLIBC_2.34`, `.gnu.version_r`), which is why decades-old binaries
still run on Linux; the SONAME major (`libc.so.6`) is the coarser
version of the same idea, and macOS's `LC_ID_DYLIB` compatibility
version a third. All of them let an OLD PROGRAM KEEP RUNNING across
compatible change, and all of them cost machinery -- a linker script
per library, version nodes, loader support, and a standing discipline
about what counts as breaking.

This project has one userland, built together and shipped together. It
does not need an old program to keep running; it needs to be TOLD when
one cannot. So the stamp is a single integer, the check is an equality,
and the answer to a mismatch is "rebuild it" rather than a compatibility
path. That is the same shape as everything else here that refuses
rather than guesses -- a progressive JPEG before it was implemented, a
config parser, `fs_read_into()` on an oversized file.

Three details worth keeping.

**BOTH SIDES ARE OPTIONAL.** `resolve(..., 1)` is the weak lookup, which
answers 0 rather than dying, and a missing stamp on either side skips
the check. An object built before this existed carries none, and a
version gate that bricks every older binary is a worse failure than the
one it replaces.

**THE MAGIC IS CHECKED** because these records are read out of an object
nothing has relocated yet, so a wrong address has to be recognisable as
garbage rather than believed.

**ONE NUMBER, IN LIBC, NOT ONE PER LIBRARY.** libuapp.so carries no
stamp of its own: two version numbers that can only ever agree are a
second thing to keep true for no benefit.

When to bump it: anything that makes an already-built program wrong --
the TLS block's size or alignment, a struct passed or returned BY VALUE
by an exported function, a signature, the meaning of a wrapper's
arguments. NOT adding a new exported function; an old program never
calls it, and a new program against an old library already fails to
resolve it by name, which reports itself clearly.

## The settings generation starts at 1, so 0 can mean "never asked"

Every cache holder that watches the settings registry keeps its own
seen-generation in a `static uint32_t`, which zero-initialises, and
early-outs when the registry's counter matches it. The counter also
started at 0, so on a fresh boot the first poll compared EQUAL and the
holder never adopted anything -- it sat on its compiled-in defaults
until some unrelated setting happened to move the counter.

That is not a theoretical gap. `desktop.shadows=off` and
`desktop.animations=off` were being ignored on every boot: the file on
disk said off, the desktop drew shadows, and changing any setting in
Control Panel made them correct until the next reboot. `wm_idle`,
`wm_tray` and the tray popups share the shape and had the same latent
bug.

`cursor_theme.c` was the one that worked, because it has an
`cursor_theme_init()` that adopts unconditionally at startup. That is
the other possible fix, and it was rejected as the PRIMARY one: it
cures the instances, and the next poller written to the obvious shape
breaks again in a way that is invisible until someone reboots with a
non-default setting.

So the invariant moved into the counter instead: a real generation is
never 0, and 0 is reserved for "never asked". One line, and every
present and future poller of this shape adopts on its first poll.
`wm_setting_generation()` also returns 0 when the syscall fails, which
now means a failure re-reads rather than silently keeping stale values
-- the safe direction for a config cache.

Linux's sysctl and GSettings both avoid the question by pushing change
notifications rather than having readers poll a counter; a poll was
kept here because the compositor already runs a loop and a push would
need a subscription mechanism the registry does not have.

## The mmap region list is allocated and grown, not a fixed array

`struct sched_mm` carried `struct mmap_region regions[32]`, and a
process that wanted a 33rd got `-ENOMEM`. Two very different things
ran into it. The dynamic loader spends four regions per shared library
(one MAP_FIXED per PT_LOAD, plus an anonymous one where .bss runs past
the file), so `/bin/wget` with four libraries was already at 16 before
its own code ran. And the compositor spends THREE per client window --
a uchan ring and two buffers -- so its sixth window failed, measured at
16 baseline + 3 each.

The compositor's failure was the bad one, because of WHERE it landed.
With two slots left the create-time map of buffer 0 succeeded and the
first present's buffer 1 did not, so the window appeared with full
chrome and a taskbar button around a buffer nobody had ever written:
solid black, silently, every frame. Whether a window died at create or
opened black depended on `(32 - baseline) mod 3`, which is why it
looked intermittent and app-specific.

Raising the array was rejected. A region is **296 bytes** -- `path[256]`
is nearly all of it -- so 32 slots across 64 process slots is ~592 KB
of `.bss` resident on every boot, and doubling the ceiling doubles that
for a limit almost no process approaches. Interning the paths was the
other candidate (only file mappings use one, and a .so's four PT_LOADs
repeat it), which would have made a region ~40 bytes.

Neither was taken, because both keep a fixed ceiling that somebody has
to size correctly in advance, and **no real system does that**: Linux
keeps `vm_area_struct`s in a tree, NT keeps VADs in an AVL tree, and
both grow on demand with a high safety limit (`max_map_count`, 65530).
So the list is allocated, starts at 16 and doubles, with
`MMAP_MAX_REGIONS` kept as that safety limit rather than as a size.
Typical processes now use LESS memory than before, since nothing is
resident for a ceiling they never reach.

**What that costs, and where it is paid.** The old comment defended the
fixed array on the grounds that a slot's teardown cannot leak it. True,
and a pointer can, so the free lives at the one place both process-death
paths already funnel through -- `release_process_state()`, beside
`fd_release_all()` and the other per-address-space bookkeeping. Two
other sites matter: a spawn frees any array a previous occupant of the
slot left, so a recycled slot cannot leak; and **fork copies
`struct sched_mm` by value, pointer included**, so it deep-copies the
array or refuses the child -- sharing it would free one array twice.

## The fd tables grow, and "address spaces that may hold fds" is not a number

Three fixed arrays bounded the whole system: 32 open-file DESCRIPTIONS,
16 descriptors per address space, and 24 ADDRESS SPACES that could hold
fds at all. On a 1080p desktop that last one bound first. Every app is
an address space, so about a dozen services plus ten windows filled it,
and the desktop then could not open anything -- not a file, not a
window, not a shell. It presented as the machine being "slow" with the
CPU idle, RAM free and 22 of 64 process slots used, which is why it was
read as a performance problem for some time.

The tell was that closing windows did not always help and the count
that mattered was PIDs, not windows.

**What Linux and NT do.** Linux hangs a `struct fdtable` off the
process and doubles it on demand from 64; `struct file` comes from a
slab with no table at all. The ceilings are `RLIMIT_NOFILE` per process
and `fs.file-max` globally, the latter computed from RAM at boot. NT
grows a three-level sparse handle table per process, bounded by pool
quota. Both grow on demand and keep a high ceiling. Neither has
anything resembling FD_SPACE_MAX, because the table lives WITH the
process and the question never arises.

So here: descriptions are allocated one at a time behind a pointer
table that doubles, a descriptor table is allocated per live address
space, and the ceilings are what remain -- 1024 descriptions and 256
descriptors, for the reason `fs.file-max` exists, so one runaway
process cannot spend the kernel heap. The fd-space table grows to the
process limit plus a few rather than a picked number: an address space
that holds fds belongs to a process, plus the legacy `run` loader.

**Why still keyed by CR3, and not moved into the process slot.** That
was the tempting version and it breaks the legacy loader, which has no
scheduler slot and therefore no pid -- the trap `SYS_SBRK` documents.
CR3 is the identifier every path has. What went away is the fixed
COUNT, which is what actually bound.

**Why descriptions are individually allocated rather than one grown
array.** A grown array moves, and `fd_get()` hands callers a
`struct open_file *` that some of them hold across an allocation --
`sys_accept()` does. Growing a table of POINTERS leaves the
descriptions where they are. The mmap region list learned the other
way round in the same session: there the array moved under a held
pointer and corrupted the kernel heap.


## A delay uses the clocksource, because a tick needs an interrupt

Five drivers had each hand-rolled the same busy-wait -- read
`coarse_ticks()`, spin until it has advanced far enough. Two of them
(`usb_hub.c` and `rtl_usb.c`) were byte-identical copies of the same
function, and `xhci.c` had a sixth version that was the only one doing
it properly.

**A tick counter only advances on a timer interrupt.** Anywhere that
interrupt cannot land, every one of those loops is infinite. That is
not hypothetical: it hung the machine when an xHCI recovery path called
`xhci_power_ports()` -- whose spin was the naive kind -- from a syscall
(docs/bugs.md). The failure has no symptom other than the machine
stopping, which is the worst shape a wait can have.

`clocksource_delay_ms()` is the one implementation now, and it is
`xhci_delay_ms()`'s policy promoted to where the time actually lives:
spin on `clocksource_now_ns()` when the current source can be read with
interrupts off, and fall back to ticks only when it cannot -- which is
the one case where the caller had nothing better anyway. Callers get
REAL milliseconds on a TSC instead of everything quantised up to the
next 10 ms tick, which the speaker's tone lengths were paying for.

**What deliberately did NOT move**, because each is a tick spin for a
reason rather than by accident:

- `cpuid.c` and `lapic.c` calibrate the TSC and the LAPIC timer
  AGAINST the PIT. Using the clocksource there would be circular.
- `krandom.c` counts how many spins fit in a tick; the quantisation is
  the measurement.
- `ata.c` spins on `hlt`, which is a wait-for-INTERRUPT idiom and not a
  delay at all -- moving it would break the DMA wait it exists for.
- The timeout POLLS (`ac97.c`, `diag.c`) are a different shape: a
  deadline around a condition, not a fixed delay. They want a deadline
  helper, which is a separate change.

## A pointer button is an edge, not a level

Every stage between a mouse and a client kept the button mask as a
LEVEL and sampled it, and three of them could sample after the click
had finished. `hid_service_one()` drains every queued USB report in one
pass and keeps the last mask; `win_input_poll()` pushed an event only
when the mask differed from the one it last saw, and it runs from
`scheduler_idle()`; `wm_rawin_pump()` assigned the newest mask over
whatever was there, while the comment at the top of that file promised
the opposite. A press and its release landing between two looks cancel,
and the click never happened.

It was reported as "the File Manager's Back thumb button works maybe
one time in twenty, and holding it makes it work" -- which is the shape
of a lost EDGE rather than a lost button: holding one widens the window
until some look catches it.

Linux does not have this class of bug because evdev has no level to
sample: a HID driver emits `EV_KEY`/`BTN_SIDE` 1 and 0 into a per-device
buffer and the consumer drains it. NT's mouse class driver queues
`MOUSE_INPUT_DATA` with per-button transition FLAGS rather than a state.
This kernel's own keyboard already worked that way
(`keyboard_try_get_transition()`, added when key releases arrived); the
mouse was the outlier, so the fix is the shape already here, not a new
idea: `mouse_try_get_button_transition()` queues every change,
`win_input_poll()` drains it before sampling the position, and
`wm_rawin_pump()` hands the frame one edge per frame.

The position stays a sampled level on purpose, and that asymmetry is
the point -- Windows holds one `WM_MOUSEMOVE` per queue and X compresses
`MotionNotify` for the same reason. Ten queued moves are one position
and replaying them makes the pointer crawl; ten queued button changes
are ten things the user did.

**The loss is not reproducible under QEMU, which is why it shipped.**
The PS/2 path interrupts per packet and the idle loop that polls wakes
on those interrupts, so it always gets a look between a press packet
and a release packet; a USB guest is paced by the endpoint's polling
interval, with the same result. Both were MEASURED with the fix
reverted, and both passed every check. The deterministic control is the
KTEST ("a press and its release both survive one polling pass"), which
feeds the two reports through `input_report_buttons()` with preemption
off; the hardware is where the end-to-end claim was confirmed.

## The HID report descriptor is parsed now, and the boot protocol is the fallback

`input_usbhid.c` asked every device for the BOOT protocol: the fixed
format a BIOS can drive without understanding anything, 8 bytes for a
keyboard and 3 or 4 for a mouse. Its own header called the report
descriptor "scope this driver deliberately does not have", and for
"keyboard and mouse work" that was true for months.

It stops being true at the first device with something the boot format
has no room for. **A boot mouse report carries THREE buttons**, so on a
five-button mouse the fourth and fifth are not mis-decoded, they are
never transmitted: measured on a Logitech G305 receiver, `back` arrives
as a courtesy bit in the spare bit 3 and `forward` does not exist on
the wire at all. No amount of work downstream can recover a button the
device was never asked to send.

Linux has never had this problem because `usbhid` uses the REPORT
protocol and parses the descriptor; boot protocol is its fallback, for
quirky hardware. Windows does the same through HIDCLASS. So the shape
here is the ordinary one, arrived at late.

`kernel/lib/hid_parse.c` walks the item stream and derives ONE layout
per device -- where the buttons are, how many, how wide the axes are,
where the wheel and horizontal scroll sit. The driver asks for report
protocol **only when a layout was derived**, and otherwise sets boot
protocol exactly as before. That ordering is the whole safety argument:
a device switched to its own format before anyone knows what that
format is reports bytes nobody can decode, and a mouse whose axes are
read from the wrong bits is a pointer flying across the screen -- worse
than the missing button this was written to fix.

**What it deliberately is not**: a general HID parser. No Feature or
Output reports, no Push/Pop, no delimiters, no usages it has no use
for. Unrecognised fields still ADVANCE the bit offset, which is the one
thing it must get right -- a field skipped without advancing moves
everything after it.

What the G305 gains, beyond the missing button: sixteen buttons instead
of three (five of which this kernel has names for), SIGNED 16-BIT axes
instead of 8-bit -- so a fast flick stops clipping at +/-127 -- and the
horizontal scroll the boot format cannot express.

**AND THE BUTTON THAT STARTED THIS STILL DOES NOT WORK, FOR A REASON
OUTSIDE THIS KERNEL.** The G305's forward button is a **G-Shift** in
its onboard profile -- a modifier the mouse applies to its OWN buttons
and never reports. Measured after the parser landed, with every report
on that endpoint logged before the report-ID filter: 341 reports while
it was pressed repeatedly, and the button byte was only ever `00` or
`08` (back). No mouse report, no Consumer report on ID 3, no vendor
report on ID 8. A host cannot decode a button the device does not
send, in any protocol. Remapping it to "Forward" in the mouse's own
onboard memory is the fix, and then it arrives as button 5 -- this
kernel names the low five bits (kernel/input.h), so remapping it to
button 6 or higher would not help either.

**Verified against descriptors captured off real devices**, not
invented ones: `tools/hid_parse_hostcheck.py` runs the same parser on
the host over the G305's two interfaces and QEMU's pair, and KTESTs
cover the same bytes in the gate. A descriptor written by whoever wrote
the parser proves only that the two agree.

## A wake preempts only from a better level

A ring-3 driver is woken by its device and has to refill before its
buffer drains. Before this, `scheduler_wake_n()` only marked it READY,
and it ran when the rotation reached it. With four busy processes at a
10 ms slice, that meant waiting 40 ms against a 20 ms USB audio buffer.
Measured on the ASUS with a Sound BlasterX G6: the driver ran dry
**exactly every 40 ms** while a few telnet sessions were busy, and was
clean while they were idle. The in-kernel driver played the same file
clean through the same disturbance, because it refills inside its
interrupt handler.

**What real systems do.** Linux marks the woken task (`check_preempt_curr`
setting `TIF_NEED_RESCHED`) and switches on the way out of the interrupt
or syscall. PipeWire's data thread runs `SCHED_FIFO` so that it wins that
check. Windows boosts a thread whose wait completes and gives audio
threads a priority band through MMCSS. Both have **wake preemption plus
a better level**, and neither works without the other: preemption alone
only reorders equals, and a better level alone still waits for the tick.

**What toy-os does.** `scheduler_wake_n()` raises a flag when the woken
process's level is strictly better than the running one's.
`scheduler_trap_exit()`, at the bottom of `isr_dispatch_body()`, rotates
when that flag is set. It uses the same rotation and the same refusals
(`g_preempt_depth`, the legacy loader) as `scheduler_tick()`, which
already rotates from an interrupt, so it adds no new place a switch can
happen. `snddrv` asked for nice -10 then; since 2026-10-10 its service
runs it `SCHED_FIFO` ("Two scheduling classes", below). Strictly better
only: a wake at the same level preempting would rotate the CPU on every
interrupt.

**The preempted process has to run first when its level comes back**,
Windows' rule for a preempted thread. Without it the round-robin
restarted just past the driver's slot on every wake, 250 times a second,
so the slots after the driver's always won and one scanned late
(soundd, as pid 9 behind a driver at pid 15) went **762 ms** without a
turn -- half-second dropouts with the driver reporting 0 dry; the same
build with soundd at pid 15 gave 33 ms. A per-level head
(`g_preempted[]`) did this until 2026-09-26; it is now a consequence of
"Within a level, whoever has run least runs next", since a preempted
process has run less than whoever kept going.

**This explains the older measurement** that raising `snddrv`'s priority
"starved the fillers" (docs/decisions/workflow.md, "Audio is judged by
RECORDING it"). A driver at a better level cut in on every wake and reset
the rotation each time, with or without preemption. The 2026-09-21 wedge
at -10 is NOT explained; it did not recur in about ten runs at -10 on the
ASUS and in QEMU. And **priority is still strict with no ageing**: a
process at a better level that spins takes the machine -- true of the
realtime class today, which is why it has a watchdog and a throttle
("Two scheduling classes", below).

Measured 2026-09-23 on the ASUS, same script each time (an MP3 with a
disturbance 20 s in), `--prio 0` against the new default: full speed
227 dry against 0; high speed 161-216 against 0 over three pairs; kernel
underruns 0 with the fix. By ear: the first of each pair crackled and
stuttered, and the fixed one was clean at both speeds.

## A disk wait sleeps, and the three things that made it pay

`ata.c`'s DMA and cache-flush waits park the caller on IRQ14 instead of
polling with interrupts off (2026-09-23), which is stage 2 of
`docs/blocking-design.md`. The obvious version -- swap the poll for
`scheduler_block_kernel()` and stop -- made the compositor WORSE:
0.6-1.7 s frames under a disk benchmark, against 5-22 ms before. Each
of these is why it now pays, and each is the obvious thing done
differently:

**An unlock hands the lock to a parked waiter rather than dropping it.**
Dropped, a releaser still running and about to make its next file call
can re-take it before the woken waiter is scheduled, and a waiter can
lose that race indefinitely -- the starvation Linux's mutex handoff
(4.10) exists for. `kmutex_unlock()` names the owner itself
(`scheduler_wake_one()`, best priority first) and the waiter claims it
on resume. Measured honestly: the probes after it showed every WM wait
was ONE holder operation (~17 ms), which is what handoff guarantees; how
much worse it was without handoff was not measured separately.

**A context woken MID-CALL cuts in within its level -- while it has run
less than what it would preempt.** An IRQ wake only made the waiter
READY, so every DMA completion cost it up to a whole slice behind a busy
peer -- and it was usually holding the filesystem lock the whole time,
so everybody behind it waited too. CFS's wakeup preemption, and 4.4BSD's
PRIBIO for the same reason. Never across classes, so the realtime audio
driver still wins. This is the one exception to "a wake preempts only from
a better level" above. It was UNCONDITIONAL until 2026-09-26 (a
`g_wake_next` naming the next process), which is the half of CFS's rule
that was copied without the other half; see the next-but-one entry.

**The cache FLUSH sleeps as well as the DMA.** Probed per syscall, the
DMA waits totalled 1-2 ms per hundred commands; the stalls were CACHE
FLUSH (0xE7), a host fsync under QEMU, 25-225 ms polled with interrupts
off. It completes with the same IRQ14 and the bus-master IRQ bit reports
it the same way, so it takes the same wait. The obvious reading of
"make the disk wait sleep" was the DMA alone, and the DMA was never
where the time was.

What it did NOT fix is the convoy behind one global lock: a caller of
the filesystem still waits one holder operation per call, which is why
the compositor's config reads came off its frame path (next entry) and
why `docs/fslock-design.md` exists.

## Within a level, whoever has run least runs next

Each process carries a `vruntime`, the CPU it has consumed as the picker
counts it, and so does the kernel context. `find_next_runnable()` keeps
the strict levels and, within the best one, runs the lowest `vruntime`;
the old rotation order only breaks ties. A wake at the same level may
cut in only when the woken context is behind what runs by more than
`WAKE_GRAN_NS`. That is CFS's rule (EEVDF's since 6.6), minus weights,
because the levels already are the weights here. (Since 2026-10-10 the
"level" is the fair class, and nice IS a weight on `vruntime` -- "Two
scheduling classes", below.)

**What it replaced was two tie-breaks with no bound between them.** A
context woken MID-CALL ran next (`g_wake_next`), and a context a wake
preempted resumed first (`g_preempted[]`). Each was measured to fix a
real starvation (the entries above). Together, a thread doing
back-to-back synchronous disk I/O and the kernel context handed the CPU
to each other on every IRQ, and the ordinary scan that would reach
everyone else never ran. Caught on a live guest through QEMU's gdbstub
(2026-09-26): 15 minutes into a 20-second `fsrace_test`, EVERY other
process was READY with its CPU time frozen -- the test's main thread,
toywm, init, the daemons -- and the debug console was dead, because the
kernel context serves it. `fsrace_test` alone failed 7 runs in 10 on
`2daae758`; with this, 0 in 10. The desktop saw the same thing in
milder form: under `diskbench`, `latency_under_io.py` measured the
compositor's `wake` at 22.9 ms average and 378 ms worst before, and
0.04 ms and 0.37 ms after (KVM, one run each).

**Both old behaviours fall out of the one rule**, which is why they are
gone rather than bounded: a preempted process has run less than the one
that kept going, and a disk waiter that sleeps most of the time has run
less than a busy peer -- until it has not, which is exactly when it
should stop cutting in.

**Four details that are the difference between this and a starvation
of its own:**
- **A wake PLACES the woken context** no further back than one slice
  behind the pack's floor (`vr_place()`, CFS's `place_entity()`). A
  process asleep for an hour is otherwise owed an hour.
- **The kernel context's idle halt is not running**
  (`scheduler_idle_halt()` bills around it). Linux's idle task is not a
  CFS entity at all; here the kernel context also runs the shell and
  the debug console, so it has to compete, but only for what it
  actually used.
- **A new process starts at the floor**, not at 0, or every spawn would
  be owed the whole uptime.
- **`SYS_YIELD` goes to the back**: the yielder's `vruntime` is raised
  to the most any peer at its level has run, CFS's old compat-yield.
  Without it a process that only yields has run nothing and is handed
  straight back the CPU it gave up -- `cputime_test` caught exactly
  that, billing 50% of a window to a pure yield loop.

**And one bug it exposed rather than caused.** `scheduler_kill()`
deferred a kill for a context BLOCKED mid-call (the D state) but tore
down one that had been WOKEN mid-call and not yet resumed -- READY, still
inside its kernel frames, possibly holding the mount lock. Under the old
rules that window was a few instructions, because a mid-call wake always
ran next; under this one it is up to a slice, and the desktop killing
its screensaver landed in it: the root mount's lock owned by a pid that
no longer existed, and every file call in the machine waiting on it. The
kill now defers on `parked_in_kernel` whatever the state.

**Not done, deliberately:** no weights within a level (nice values are
levels here, strict, and ageing across them is still absent -- a
better-level spinner still takes the machine; both changed with "Two
scheduling classes", below); and the same-level
cut-in stays limited to MID-CALL wakes, as before, rather than every
wake as in CFS, so ordinary interrupts still do not rotate the CPU.

## The compositor's config is pushed, into the queue it already waits on

The WM's pollers keyed on `fs_generation()`, one counter for every write
in the machine, so a program writing to `/var/tmp` had the render loop
re-reading `/etc` every frame -- and with a sleeping lock holder each of
those reads queued ~17 ms. Now the settings registry pushes
`WIN_EV_SETTING` on every generation bump, and `SYS_FS_WATCH` pushes
`WIN_EV_FSWATCH` for a change at, or directly inside, a watched path.

**Events on the compositor's existing queue, not an inotify descriptor.**
inotify's shape is a readable fd, and the WM has no `poll()` over
descriptors: it parks on its kernel event queue (`SYS_WAIT_READY`),
which the kernel already uses for exactly this kind of fact --
`WIN_EV_FONT`, `WIN_EV_SCREEN`. Windows delivers both halves the same
way, as window messages (`WM_SETTINGCHANGE`, `SHChangeNotifyRegister`).
The cost is that only the compositor can watch; a per-process
descriptor is a roadmap item once there is a wait over more than one
kind of object.

**Matched by hash, and coalesced.** A watch stores the FNV-1a of its
path and vfs.c hands over the hashes of the changed path and its
parent, because a stepped write finishes with no path in hand and 16
slots of a 4096-byte path would be 64 KiB of kernel data to avoid a
collision whose only cost is a spurious reload. The queue keeps one
pending event per watch and one SETTING, so a program writing all day
costs one event, not a backlog that evicts input.

**The fs generation is still there**, for a watch the kernel refused
(the WM falls back to it and logs once) and for the other pollers that
never ran on the frame path.

## The tick is a deadline among several, and a faster tick was not the answer

The question was "raise the 100 Hz tick, or get rid of it". What 100 Hz
actually limited was every timed wait: a sleep, a timed futex wait and a
client's `WIN_EV_TIMER` all expired only when a tick happened to check
them, so a 16 ms frame timer landed on 20 ms and a 1 ms sleep on ~10.

**What real systems do.** Linux keeps a periodic tick at `CONFIG_HZ`
(100-1000) for accounting, and since 2.6.21 puts precise timers on
one-shot clock events (hrtimers), stopping the tick on an idle CPU
(`NO_HZ_IDLE`, the default). Windows keeps a coarse 15.6 ms clock
interrupt, lets a process ask for 1 ms (`timeBeginPeriod`), gives
precise wakeups without raising it (high-resolution waitable timers),
and skips ticks while idle. Both keep a coarse periodic tick and put
precision on one-shot deadlines. Raising the rate alone is the answer
Linux gave up: 2.6.0 shipped 1000 Hz and 2.6.13 went to 250 for the
cost, before NO_HZ existed.

**So toy-os follows that shape** (`kernel/clockevent.h`):

- **One-shot.** With a LAPIC timer and a clocksource that runs without
  the tick, the timer is armed for the earliest of a blocked process's
  deadline, the running slice's end and the next periodic tick. Every
  context switch re-arms it. Without either (`nomsi`, the PIT as the
  clock), the tick stays periodic and everything still works at its
  granularity -- `highres=off` reaches that path on any machine.
- **Tickless idle.** In `scheduler_idle_halt()`, with nothing runnable,
  the tick drops out of that minimum. Idle work due at a time asks for
  its own wake; ONLY that helper stops the tick, so a wait loop nobody
  converted keeps it and stays correct. `nohz=off` is the A/B.
- **The slice is a deadline too** (`kernel.timeslice_ms`, 4 ms), not a
  tick count, so the tick rate stops deciding how long a process runs.
  At the old 100 Hz on the periodic path it is exactly the old
  rotate-every-tick.
- **The ACPI PM timer is a clocksource.** Plain QEMU TCG cannot expose
  an invariant TSC, so without it the default test guest -- every tool
  in `gui_regress.py` -- could never have run one-shot. It needs no
  calibration, and its 24 bits wrap in 4.7 s, which bounds how long a
  stopped tick may sleep (`clocksource_max_idle_ns()`).

**A DEADLINE WAKE PREEMPTS AN EQUAL -- an amendment to "A wake preempts
only from a better level".** The first measurement of the one-shot timer
found a 1 ms sleep ending 7 ms late under two busy processes: kept to
the microsecond, then left waiting for the busy one's slice. An expired
deadline now preempts a process at the same level once it has run 1 ms
(CFS's wakeup granularity). That entry's reason for "strictly better
only" still holds for INTERRUPT wakes, which keep the rule: a deadline
is one wake the process armed for itself, never "every interrupt".

**`coarse_ticks()` stays at 100 a second, and so does `SYS_TICKS`.** The
tick rate is a build option (`option hz`), and Linux's `USER_HZ` is why
ring 3 must not see it: `times()` still counts at 100 whatever the
kernel runs at. Inside the kernel the same split saved rewriting ~70
coarse timeouts in `coarse_ticks()` units. Everything precise already read
`clocksource_now_ns()`. And `coarse_ticks()` is now DERIVED from a
free-running clocksource, so a wait on it ends inside a syscall -- the
trap "A SYSCALL HANDLER RUNS WITH INTERRUPTS OFF" describes survives
only on the PIT clock.

**Choosing the rate.** With deadlines one-shot and the slice its own
deadline, HZ decides only how often a BUSY CPU is interrupted, and how
precisely the periodic fallback checks anything. `tools/timer_bench.py`,
2026-09-24, TCG, medians of two runs (KVM agreed on every conclusion):

| build | idle irq/s | 1 ms sleep late by | ...under 2 busy | work, 1 busy process |
|---|---|---|---|---|
| before (100 Hz periodic) | 100 | 9000 us | 29000 us | 17140 |
| 100 Hz one-shot | 26 | 62 us | 56 us | 33390 |
| 250 Hz one-shot | 34 | 64 us | 55 us | 33207 |
| 1000 Hz one-shot | 74 | 49 us | 46 us | 33082 |
| 1000 Hz periodic | 999 | 1000 us | 1000 us | 28236 |

Between 100 and 1000 Hz one-shot nothing moved beyond noise, so the
default is **1000** -- it costs nothing here and makes the periodic
fallback ten times finer. Two findings came with it:

- **A lone busy process used to get about HALF the CPU.** The kernel
  context is a round-robin participant, and the idle one spent its turn
  halted until the next tick. An idle kernel with a process ready now
  hands over at once (`clockevent_idle_halt()`). The periodic path still
  cannot -- nothing interrupts the halt early -- which is in
  `docs/bugs.md`.
- **Under KVM without `+invtsc` idle host CPU rose (2.5% to ~4.5%)**,
  and the periodic 100 Hz build shows the same, so it is the CLOCK: the
  PM timer is an I/O port, and every read of it is a VM exit where the
  PIT source was a memory counter. Real hardware reads the TSC. A
  paravirtual clock (kvmclock) is the fix, on the roadmap.

## A slot being built is claimed by a flag, not a new process state

A spawn picks a free slot, then reads the ELF from disk -- which sleeps
since disk waits park -- and only marks the slot READY at the end. The
slot stayed `SCHED_UNUSED` in between, so a second spawn in that window
took the same slot: both filled it, and the first child never existed.
Its descriptor table was never released, so a parent reading the
child's stdout pipe waited forever (`argv_test` in `block(pipe)`, about
1 full-suite run in 5), and a parent in `waitpid()` could lose its
child outright. Proven with a probe: the orphaned table had been opened
by the parent's spawn and was owned by no process.

Linux gets the same guarantee from `TASK_NEW`: the task is in the table
from the start, in a state nothing schedules, kills or reaps, and
`wake_up_new_task()` publishes it. **toy-os uses a flag instead**
(`g_slot_claimed[]`, `slot_claim()`), which only the four allocators
consult -- spawn, fork, thread create and the test fabricator. Every
other scan of the table still sees `SCHED_UNUSED`, "not a process",
which is exactly what a half-built slot is. A new state would have been
correct too, but `scheduler.c` compares against the states in about
sixty places, and any one that treated the new state as live would
have made a half-built slot killable, waitable or schedulable. The flag
changes four call sites and nothing else can observe it.

Measured: 0 pipe hangs and 0 vanished children in 20 full-suite runs
after it, against 4 pipe hangs in 25 before.

## The kernel log and the debug console are two serial ports

Every harness here drove the guest over ONE serial line that carried
two streams: the debug console's commands and replies, and the kernel
log, written asynchronously from any context. A reply had no framing,
so a log line could land inside it -- measured: `wrap_test: ` + two log
lines + `all checks passed`, a passing test reported failed 4 full
suite runs in 25. `readfile` framed the one reply that mattered most;
this separates the streams for every tool.

**The split.** COM1 carries the kernel log alone; COM2 the debug console
(`serial_dbg_*` in `serial.h`). The kernel probes for a second UART
with the scratch register and keeps everything on COM1 when there is
none -- most real machines have one port or none, and every tool that
builds its own one-port QEMU line keeps working untouched. Linux's
`console=ttyS0` plus a getty on ttyS1 is the shape; QEMU's guest agent,
on its own virtio-serial channel with framed JSON, is the same idea
taken further.

**The console's replies stopped being log lines.** It printed through
`klog_write()`, so every reply also landed in `dmesg` -- the reason the
two shared a stream at all. A reply is not an event; it goes to the
console's port only. The console coming up IS an event, so that one
line goes to both.

**The harness absorbs it.** `vm.py` gives a guest both ports and keeps
the debug console on the socket name every tool already uses
(`.vm.N.serial`), with the log on `.vm.N.log`. About forty GUI tools
wait for APP log lines (`settings: layout ...`, `uidemo: press`), which
are kernel log now, so `DebugConsole` reads the log socket on a thread
into the buffer `logs()`/`events()` have always read -- the tools did
not change.

**And the console is opt-in** (`debugcon`, docs/boot-flags.md). It is an
unauthenticated root shell on a serial port. Linux listens on a serial
line only when configured to (`kgdboc=`, a getty) and Windows' Special
Administration Console only after `bcdedit /ems on`. `make iso` bakes
the word in, because every test needs it; release media are built with
`DEBUGCON=0`.

**The console's port is a real TERMINAL** (`kernel/tty/serial_tty.c`,
a `tty_driver` on the debug port). A second port alone left `sh cat`
running a program whose stdio was THE CONSOLE -- output reached COM2
only through a global sink swapped for the command, so any other
program printing to the physical console in that window landed in the
reply, and nothing typed on COM2 could reach the program or stop it.
Now the port's RX IRQ feeds the line discipline; the console reads
whole lines from it; and a command's program gets the terminal as
0/1/2 (`fd_set_kernel_tty()`, a `tty_fd_ops` descriptor) and leads a
session on it (`tty_attach_kernel_session()`). So it can read the line,
Ctrl-C and Ctrl-D work -- in the IRQ, while the command runs -- and it
can ask the terminal's size or go raw. The sink swap stays for the
kernel shell's own `vga_write()` output.

**ONE COMMAND, ONE SESSION, and a leftover job is hung up -- onto the
machine console.** `sh spawn` goes through `/bin/spawn`, a foreground
command, so its child inherits the terminal; a dozen tools start
long-running programs that way, and their later output would land in
the middle of later replies. So the console hangs the terminal up when
each command returns (`tty_hangup()`, a new session GENERATION that
every `tty_fd_ops` descriptor carries). A descriptor from an old
session then behaves as a CONSOLE one: writes go to the machine
console, reads come from its keyboard, and termios and job control name
tty0 -- what a leftover job had before this terminal existed. Linux's
`vhangup()` fails those calls with EIO instead; that was the other
option, declined because `tools/ansi_cursor_test.py` and its kind
screenshot a spawned program's VGA output. Reads were first to end the
way Linux's do, at end of file, and a shell started with `sh spawn` then
exited the moment its command returned -- falling back in both
directions is the consistent answer. What a job prints BEFORE the
command returns still reaches the reply, as on any terminal.

**AND `sh spawn` DETACHES outright** (`SPAWN_DETACH`, which /bin/spawn
passes): where the caller's fd 0 or 1 is this terminal, the child gets
the machine console there instead, so a background job never touches
the port at all -- `tools/ansi_cursor_test.py` screenshots a spawned
program's drawing, and its first rows went into the reply. A pty or
the console is inherited as before, so `spawn` in a Terminal window
still prints into that window.

**TYPE-AHEAD IS THE RUNNING COMMAND'S**, as on any terminal: the
console reads its line one byte at a time, so what is typed after it
stays queued for the program the command starts. A harness therefore
sends a command only once the previous one's prompt is back
(`tools/serial_console.py`'s `send()`; `vm.py` and `DebugConsole`
always did) -- `ktest_run.py` sent `ktest` while `sh fsck repair` was
still running and the console received `nt`.

**Why the kernel leads the session.** The console's "shell" is kernel
code with no process, so the command's program is made the terminal's
owner, and the discipline delivers Ctrl-C to that owner's own group
(`kernel_session`) -- a pty holds it back from the owner's group,
because there the owner is a shell reading its own terminal.


## A description carries an ops pointer, not a kind

`struct open_file` has `const struct fd_ops *ops` where it had
`enum fd_kind kind`, and `sys_read()`/`sys_write()`/`lseek`/`fstat`/
close dispatch through it. Each kind's table lives with the subsystem
that owns the stream: `file_fd_ops` in `kernel/fs/fs_syscalls.c`, the
pipe ends' in `pipe.c`, `socket_fd_ops` in `kernel/net/net_syscalls.c`,
the terminals' in `kernel/tty/tty_fd.c`, the logs' in
`kernel/core/log_fd.c`, `shm_fd_ops` in `shm.c`.

**The obvious way was the one the code had**: an enum, and a `switch
(f->kind)` in each of read, write, close, fstat, lseek and `fd_tty()`,
with every kind's I/O in `syscall_fd.c`. It held while there were three
kinds and stopped holding at eleven. A new kind had to find five or more
switches, and missing one did not fail to compile -- it fell through a
`default:` into EBADF. That is how the app log once shipped logging
nothing.

**Why a pointer and not a table indexed by the enum.** Indexing
`fd_ops_of[f->kind]` would have been the smaller diff and kept every
`kind ==` test elsewhere valid. It was declined because the enum is the
part that does not scale: every kind still has to be named in one
central header, so a driver cannot define a stream in its own file.
Linux's `f_op` and NT's `DriverObject->MajorFunction` both put the
dispatch table's ADDRESS on the object for exactly this reason -- a
`/dev` node or a driver-as-a-process (`docs/umdf-design.md`) supplies its
ops without touching the descriptor layer. "Which kind is this" is then
asked by identity, `f->ops == &pipe_write_fd_ops`, which is what Linux's
`get_pipe_info()` does with `f_op == &pipefifo_fops`.

**What the table deliberately does not have.** No `poll` op, because
nothing would call it: there is no `poll()`/`select()`, and the slot
arrives with the syscall that needs it. And the ops do not change how
anything blocks. `read`/`write` return 1 when they PARKED the caller --
NT's STATUS_PENDING, the wake writes RAX -- which is the contract the
`sys_do_*` helpers already had, so each op keeps its own check-and-park
under the preemption guard rather than having one imposed.

## A file a release stops shipping is removed -- by dpkg's rule, not by mirroring

System Update keeps the manifest it last applied and, on the next run,
removes what that listed and the new one does not -- **only if the file
still has the crc it was shipped with**, and only in the managed trees.
That is dpkg's and rpm's shape (a package's recorded file list; an
edited conffile kept, rpm's `.rpmsave`), and the reason is what it
cannot do: delete a file nobody shipped, or one the owner edited.

The obvious alternative, MIRRORING the shipped directories (delete
whatever in `/bin` or `/etc/settings.d` the manifest lacks), was
declined because it deletes the owner's own additions -- a screensaver
dropped in, a settings declaration written by hand. An explicit
"obsolete" list per release was declined because it needs somebody to
remember to write it, which is exactly the step that was missed when
`desktop.week_start` was retired and lingered on the laptop.

The old objection -- deleting on the strength of an unauthenticated list
promises more than updating -- does not survive the rule: a hostile
manifest can only remove files an earlier manifest delivered and nobody
touched, which it could already have replaced with anything. A stale
LIBRARY waits for the boot like a changed one (`-<path>` in
`/var/lib/update/pending`), because a process may still map it.

## A shipped change to /etc replaces an unedited file -- dpkg's conffile rule

`/etc` and `/home` were NEW-ONLY: installed when absent, never touched
after. That kept the owner's edits safe and made every later change to
a shipped `/etc` file unreachable: the scheduling-class work gave
`/etc/services.d/toywm` two new lines (2026-10-10), and every machine
that already had the file updated to a kernel with realtime classes and
a compositor that never asked for one.

**What real systems do.** dpkg replaces an unedited conffile silently
and, for an edited one, asks -- or keeps the owner's and writes the
package's as `.dpkg-dist`. rpm's `%config(noreplace)` keeps the owner's
and writes `.rpmnew`. systemd avoids the question for units with a
split: vendor units in `/usr/lib/systemd/system`, always replaced, and
the owner's overrides in `/etc`.

**What toy-os does: dpkg's rule, with the record it already keeps.**
The manifest last applied (`/var/lib/update/installed`, the stale-file
rule's file list) says what crc each `/etc` file was SHIPPED with. A
changed file still carrying that crc is replaced; one that does not is
kept and said so, and the shipped version goes to
`/var/lib/update/new<path>`. Not beside it: init loads every file in
`/etc/services.d` and settings every one in `/etc/settings.d`, so an
rpm-style `.rpmnew` there would load twice. No record means no way to
tell an edit, so the safe answer -- keep -- is taken. The systemd split
was declined: it fixes services only, and changes init, the layout
check and every descriptor's home to do it, where this fixes every
shipped `/etc` file with a change to the one client.

## An update that touches a library or the kernel is applied BY THE KERNEL, at the next boot

**The problem.** `/bin/update` (and the System Update window) replace
files on a running machine. An EXECUTABLE is safe to replace live:
`elf_load()` copies its segments, so a running process holds no reference
to its file. A LIBRARY is not: `ld-toy.so` maps `/lib/*.so` through
file-backed mmap regions, and a region names a PATH
(`struct mmap_region.path`), faulting pages in on first touch. Replace
`/lib/libc.so` -- by any method, atomic or not -- and every running
process reads its untouched pages from the new build. Nothing reports
it; a process simply runs two builds of libc at once.

**What real systems do.** Linux avoids it by construction: a mapping
pins the INODE, and a rename over the file leaves the old inode alive
until the last mapping goes. dnf and apt then replace live and rely on
that. Windows cannot replace an in-use DLL at all, so Windows Update
records the rename in `PendingFileRenameOperations` and `smss` performs
it at the next boot, before anything maps the file; systemd's offline
updates (GNOME Software, PackageKit) reboot into `system-update.target`
for the same reason.

**What toy-os does.** Windows' shape, because it needs nothing new from
the VM layer: a set with no `/lib` file and no kernel is installed live;
any other set is downloaded and verified to `<path>.upd`, its targets are
listed in `/var/lib/update/pending`, and `fs_apply_pending_replacements()`
renames them into place in `kernel_main()` after `INIT_FS` and before
`INIT_CONFIG` and init. The KERNEL does it, not init, because after the
reboot the kernel is the one component certain to be the new build --
init is itself a file the list may be replacing, and an old static init
running on a new kernel is exactly the mismatch the reboot exists to
avoid. The kernel is in the rule for the same reason: a new kernel may
change an ABI the new userland already assumes, so kernel and userland
switch at the same reboot.

**The list names targets only** (the source is always the target plus
`.upd`), so it cannot be used to move an arbitrary file; and it is
idempotent (a target with no `.upd` left was done by a boot that lost
power), so a power cut during the apply is finished by the next boot.

**Revisit when** mmap regions pin an inode rather than naming a path --
the Linux answer, which would let a library update apply live. That is a
change to rename, delete and the fault path together, and this feature
did not need it.

## A process with a thread parked mid-call dies when that thread leaves the kernel

A single-threaded process killed while parked inside kernel code already
died on its way out rather than where it slept -- Linux's D state -- so
that its C frames could release the locks they held. Its THREADS were not
covered: `group_release_threads()` freed every sibling where it stood,
and a sibling parked in a disk wait holds the disk lock (`g_ata_lock`)
and often a mount's lock. System Update hashes files on a pthread from
the moment it opens; killing it mid-check left `g_ata_lock` owned by a
pid that no longer existed, toywm blocked on it while holding the root
mount's lock, and the whole desktop froze for good (reproduced 1 in 1,
the owner read with gdb).

**What Linux does.** `exit_group()` does not free sibling threads; it
sends each one SIGKILL (`zap_other_threads()`) and lets each leave the
kernel at a safe point. The `mm` is refcounted (`mm_users`), so the
address space lives until the last thread drops it, and the group leader
stays an unreapable zombie until its thread group is empty
(`delay_group_leader()`).

**toy-os copies that shape without the refcount.** Whoever ends a process
-- `scheduler_kill()` for a victim, `scheduler_exit_group()` for the
running thread's exit, fault or fatal signal -- first asks whether any
OTHER thread of the group is parked mid-call. If none is, nothing changes.
If one is, the death is deferred: every other thread gets a pending
SIGKILL (a mid-call one keeps it until its call completes, which is
`scheduler_signal_raise()`'s existing rule), the address space and fds
are left alone, the leader's slot becomes a ZOMBIE that `scheduler_poll()`
refuses to reap (`group_dying`), and the first death's exit code is kept.
The last thread out sees no parked sibling and runs the ordinary exit --
cleanup, zombie, parent notified -- with that code.

**Why "is a sibling parked" instead of a count of live threads**: a
thread in ring 3 or parked at a syscall ENTRY holds nothing, and freeing
it outright is what the code always did and is still correct; only a
context with live kernel frames can be holding a lock. Asking the one
question that matters keeps the common exit exactly as fast and as
simple as it was.

**The one shape that must not come back**: the pair
`syscall_process_exit_cleanup()` + `scheduler_on_exit()` at an exit call
site. It destroys the address space before anything can ask about
siblings, so every whole-process exit goes through
`scheduler_exit_group()`. Measured after the fix: ten kills of System
Update at delays from 0.5 to 10 s, seven of them landing mid-call and
deferred, the desktop alive after all ten and nothing left unreaped.

## Pids cycle, and a pid is a field of its slot

A pid was its slot's index plus one, so a reaped pid went to the very
next spawn -- every launch of one app was pid 4 -- and anything still
holding the old number (a parent, a terminal's owner, a test's "missing"
pid) named a stranger. It also meant the process table could never be
anything but a fixed array: the identity was arithmetic in some eighty
places, and the first pass at changing it found code that indexed
`procs[]` with a pid, and the kernel mutex handing a lock to `pick + 1`.

**What Linux does.** Pids are allocated past the last one handed out, up
to `pid_max` (32768 by default), wrapping to `RESERVED_PIDS` (300) so
low pids stay with early daemons; a `struct pid` lives on while a process
group or session names it, so a group's number is never reissued to a
stranger; and a pid is looked up through an IDR, never derived.

**toy-os copies that shape.** `procs[i].pid` is set in `slot_claim()` and
cleared in `slot_free()`; `pid_slot()` maps a pid through a flat 64 KiB
table (at 32k pids a flat array is the IDR's job without the tree) and
checks the slot still holds it, so a racing lookup misses rather than
misnames. Allocation skips pids in use -- zombies included, since a
zombie keeps its map entry until reaped -- and pids still named as a
pgid or sid by any slot, the field compare that stands in for
`struct pid`'s refcount. init stays pid 1 by being spawned first, as on
Linux. The kernel debugger's thread for the kernel context moved from
1000 to `SCHED_PID_MAX`, the first number no pid can be.

This is the step that lets the process TABLE become RAM-sized (a later
entry): with no code deriving one from the other, the table's size and
the pid space are independent.

## The process table is sized from RAM, and kernel stacks come 64 at a time

The process table was a static array of 64 slots, each embedding its
exec path and current directory (8 KiB of the 9.8 a slot was), beside a
static array of 64 guarded 20 KiB kernel stacks. 64 was a number, not a
property of the machine: a laptop with 8 GiB stopped launching at the
same count as a 512 MiB VM, and a leak of 36 zombies was enough to stop
the desktop opening anything.

**What real systems do.** Linux computes `threads-max` at boot so that
task structures and stacks may use at most an eighth of RAM
(`set_max_threads()`, floor 20) and allocates each task as it is created.
Windows has no fixed count at all: kernel memory is the limit.

**toy-os takes Linux's limit and allocates in between.** The limit is
`RAM / 8 / (slot + stack + ext)`, floored at one chunk (64, what the
table always had) and capped at `SCHED_PROCS_CEILING` (8192, so the pid
space always has room for each slot's pid, group and session): 2212 at
512 MiB, 8192 at 2 GiB. Then, the choice the maintainer made between
three shapes:

- **The slot table is allocated whole at boot, but slim**: the exec path
  and current directory moved into a per-slot `ext`, so a slot is 1.6 KiB
  and the whole table 13 MiB at the cap. Eager, so every scan can index
  `procs[]` with no lookup and nothing can find a slot half-allocated.
- **Kernel stacks and `ext` come in chunks of 64**, allocated the first
  time a slot past the last chunk is claimed (`slot_grow()`, under the
  preemption guard -- the frame allocator and the page split sleep on
  nothing). Each stack's guard page is unmapped as its chunk arrives;
  past the boot pool of four page tables, the split takes a table from
  the frame allocator.
- **A chunk is kept once used.** Freeing one would mean mapping its guard
  pages back before handing the frames on, a primitive nothing else
  needs; peak use stays allocated, and is at most an eighth of RAM by
  the limit's construction. The rejected alternatives were everything
  eager (236 MiB pinned at 2 GiB for slots most boots never use) and
  everything freed on demand (that missing primitive).

**Every scan stops at `g_slot_end`**, one past the highest slot ever
used, so a loop over the table costs what has been used rather than the
limit -- the timer tick walks the table, and 8192 slots per tick would
have been the cost of a feature nobody uses. It only rises, which keeps
it race-free; a machine that once ran 200 processes scans 200.

**Three things that scaled with the table had to change shape**, and
each would have failed only at a size no test reached: fork's list of
pinned frames was a stack array of the whole table (64 KiB at the cap,
past the 16 KiB stack) and is counted and allocated; futex's wakeword
table is allocated at the first registration; the fd-space table grows
by doubling like the descriptor table beside it.

## A crash's backtrace is recovered ON the machine, by a stack scan that checks for a call

The maintainer asked for a report viewer that shows what matters rather
than a file Notepad opens as text followed by binary (2026-10-02).
macOS names its crash frames on the machine; Linux's coredumpctl
unwinds with elfutils; Windows' Reliability Monitor shows a module and
an offset and leaves the rest to WinDbg. toy-os's executables carry no
unwind tables (`.eh_frame` is in the libraries only) and no frame
pointers, so the frames come from SCANNING the saved stack -- Google
Breakpad's fallback when CFI is missing, which it labels "found by
stack scanning". Two filters make the scan worth reading: only the
words from RSP up (the kernel copies from RSP's page, and below RSP is
dead frames), and only an address whose preceding bytes are a `call`
(E8, or FF /2 in its four lengths) -- the host scan in
`panic_resolve.py --crash` keeps every code address, and on a real
report that let three stale ones in. Names come from each binary's own
`.symtab`, which every shipped binary keeps; a binary replaced after the
crash is flagged rather than trusted. DWARF line numbers stay on the
host for now: the binaries carry `.debug_line`, and reading it is a
line-program interpreter, a project of its own.

## A card can be switched off, and netd answers on a channel

`netctl down|up|renew` (2026-10-03) needed two things that did not
exist: a way to switch a card off, and a way to ASK netd anything.

**Switched off is a kernel flag, Linux's IFF_UP.** `admin_down` on the
device, set from ring 3 through `SYS_NET_CONFIG`'s `NET_IFC_DOWN`/`UP`
flags; down, `net_tx()` refuses with `-ENETDOWN` and `net_rx()` drops
before counting, and no route -- default, on-link, gateway or
broadcast -- goes through it (`ipv4_route()`). It is separate from
`link_up` for Linux's reason: one is somebody's decision, the other a
fact about a cable. A netd-only "stop leasing" was the alternative and
was rejected because the card would keep answering on its old address.
`NET_IFC_CLEAR` came with it -- the zero-is-left-alone rule had made
clearing an address impossible. **`down` KEEPS the address**, unused, as
Linux keeps it on an `ip link set down` card: it first cleared it, and a
card addressed by hand or with `dhcp = no` then came back up with
nothing, because nobody leases it (review, 2026-10-04). The router
skipping a down card is what clearing was standing in for.

**netd answers on a uchan channel, "accepted" at once.** The shape is
init's `service` channel (`lib/uinitctl.h`) and systemd's
`networkctl` asking networkd. A DHCP exchange blocks netd's one loop for
4-12 s, so a reply carrying the OUTCOME would hold every caller that
long; netd replies when it has the request and acts on its next pass,
and `netctl` (or the tray card's once-a-second read) watches the card
for the result. Restarting netd with new config was the other option
and loses every card's lease. The lease FILE survives `down`, so `up`
asks for the same address first (INIT-REBOOT).

**`netctl` replaced `ifconfig` in the same change.** A command with a
read half and a write half moves as one piece, so `ifconfig`'s listing
became `netctl status` and its address-setting `netctl address`, and its
page went the same day.

## Task Manager's memory is a page-table walk on request, not a counter kept at map time

Task Manager's per-process figure was `mem_bytes`, a count kept as
pages are mapped: every present user PTE, whoever owns the frame. That
counted a window buffer in its app AND in the compositor, the scanouts
in the compositor, and the kernel in nobody, so the rows summed to
neither the machine's "in use" nor anything else.

**What real systems do.** Windows' Task Manager shows the PRIVATE
working set, computed by the memory manager from its page lists. Linux
keeps RSS as counters (`MM_FILEPAGES`, `MM_ANONPAGES`, `MM_SHMEMPAGES`)
but computes PSS -- a shared page divided by its mappers -- only on
request, by walking the page tables (`/proc/<pid>/smaps_rollup`),
because a page's share changes whenever ANOTHER process maps or unmaps
it, which no counter in this process sees.

**toy-os does Linux's PSS walk.** `vmm_audit_space()` already walked a
space for `meminfo --audit`; it now also sums `private_bytes` (owned
managed frames, a copy-on-write one as 4096 / refs) and `shared_bytes`
(borrowed managed frames). Its own fact, `QUERY_PROCMEM`, runs it per
process under the preemption guard, which on a uniprocessor kernel is
what keeps the owner from editing the tables mid-walk. NOT a
`proc_info` field: `scheduler_proc_info()` is read by signal delivery,
the connection log and every pid lookup, none of which should pay a
walk with preemption off, and `SYS_PROC_INFO` copies a fixed size with
no length from the caller, so growing its struct would overrun an
older binary's buffer. A query record carries the caller's size. The obvious alternative, a
private counter beside `mem_bytes`, cannot be right: fork raises a
frame's refcount in BOTH spaces without touching the parent's mapping
count, and a COW break lowers it from the other side. PSS is chosen
over "private = refs 1 only" because it makes the column SUM -- each
frame once over all processes -- which is the property the rows exist
for.

**The machine's rows are the walk's sum plus two facts and a
remainder.** Apps is the sum of `private_bytes`; Shared is every shm
object's frames (allocated at creation, so counted whether or not
faulted in); Graphics is what the display driver says it holds in RAM
(`display_driver.ram_bytes`) plus the console's back buffer; Kernel is
`used` minus the three. A remainder rather than a count because the
frame allocator has no owner tags and adding them to every allocation
site is a much larger job; the cost is that Kernel silently absorbs any
category the others miss (the /lib image cache and the block cache are
in it on purpose, as Linux counts buff/cache apart from processes). The
walk costs a few microseconds per process per Task Manager refresh;
`proc_info.mem_bytes` stays, unchanged, for `ps`.

## A lower mode is REAL when the monitor lists it, and scaled from native otherwise

Stage 4 of the Kaby Lake work gave the driver two ways to show a size
below the monitor's native one: re-light the pipe at that size's own
timing (the monitor scales), or keep the native timing and scale in
the GPU's pipe scaler (stage 3).

**What real systems do.** Windows lists a monitor's EDID modes and
sets them for real, offering "GPU scaling" as a separate control;
Linux's i915 sets an EDID mode for real and uses the panel fitter only
when a mode is not supported (eDP panels, which have one timing).
Wayland compositors (KWin, Mutter) list the connector's modes from
the EDID and set them as-is.

**toy-os does the same, and fills the gaps with scaling.** A size the
EDID lists (established, standard or CEA) with a known DMT timing at
or below the native pixel clock is set for real; every other ladder
size is scaled from the native timing, so the resolution list is the
same on a monitor that lists little. The cap at the native clock is
not cosmetic: the re-modeset keeps the firmware's DDI translations,
watermarks, DDB and CDCLK, each of which a FASTER mode could outrun.
An eDP panel (gen8) has one timing, so all its smaller sizes are
scaled -- the same answer i915 gives.

## Pipes, sockets and TCP blocks grow on demand, and a freed one is kept

Each was a fixed array of eight, machine-wide: eight pipes shared by
every shell pipeline and daemon, eight sockets, eight TCP connections.
Since 2026-10-07 all three live in `kslots` (`kernel/lib/kslots.c`), a
table of separately allocated objects addressed by a small stable
index, which an fd stores. The maintainer chose one shared helper over
three hand-grown arrays, and no machine-wide ceiling: the heap and each
process's fd table are the limits.

**A freed slot's memory is kept and reused, never returned.** The fixed
tables had a property nobody had written down: a closed socket's memory
stayed valid, so `net_poll()` preempted while delivering to it read a
dead socket rather than freed memory. Freeing on close would have turned
that into a use-after-free with no test able to see it. So the table
sits at its high-water mark, as a slab cache that never shrinks does --
twenty pipes once open are 80 KiB the heap does not get back.

**An object never moves**, because each is its own allocation: a pipe's
address is its wait channel, and `net_sock_accept()` holds a listener
across the allocation of the new socket. **The index arrays grow by
publishing the new array before the new capacity, and the old arrays
are not freed** (under twice the final size together), so a reader
preempted between loading the array and indexing it never touches freed
memory. Linux's equivalent is `idr`/`xarray` with RCU; this is the same
shape with "never free" standing in for the grace period.

## A reboot asks init; SYS_POWEROFF stays a plain stop

`reboot` and the desktop's Restart used to call `SYS_POWEROFF`, which
stops the machine at once: every service vanished mid-write. Real
systems put PID 1 in the middle -- `systemctl reboot` asks systemd,
which stops units in reverse dependency order before `reboot(2)`;
sysvinit runs its `K` scripts and `killall5` -- and keep the syscall
itself immediate, with `reboot -f` as the way around the manager.

toy-os does the same. The callers ask init over its channel
(`INITCTL_REBOOT`/`INITCTL_POWEROFF`, through `uinitctl_shutdown()`),
init stops the services in the reverse of its start order and then
everything else, and only init calls `SYS_POWEROFF`.

**Rejected: the kernel redirecting `SYS_POWEROFF` to init** for every
caller but pid 1. It would catch a future caller that forgot the helper,
but it puts a policy in the kernel, and the paths that must NOT wait on
ring 3 -- the kernel shell's and the debug console's -- would need a
flag to get past it. One helper with four callers is cheaper than that.

**The request is answered before anything stops**, because its sender
is among what gets stopped. When init does not answer (no channel, no
reply in 2 s) the helper stops the machine directly, and `reboot
--force` does so on purpose: a wedged init must not make a machine
impossible to restart. init serves the channel while services are still
starting too, or a `reboot` typed during boot would always take that
fallback.

**One service at a time, five seconds each by default** (`StopTimeout=`).
systemd stops independent units in parallel; with about ten services
that saves nothing worth a dependency graph walk, and a sequence is
what the console can report line by line. Five seconds, not systemd's
90 or launchd's 20, for the readiness timeout's reason: on a machine
with one console a long silent pause reads as a hang.

## Two scheduling classes: realtime above fair, and nice is a share

Until 2026-10-10 a process had one number, nice -20..19, and the picker
treated it as a STRICT RANK: any runnable process at a better level ran
before everything below it, with fair sharing (`vruntime`) only inside
one level. That was built for one measured reason -- a ring-3 audio
driver woken by its device had to beat the rotation (above, "A wake
preempts only from a better level") -- and it left three things wrong.
The compositor and a busy app were peers, so an actively working app
degraded the desktop. A nice value meant something no port expects:
nice 19 never ran while anything at 0 wanted the CPU, and nice -1
starved the machine if it spun. And any process could set any pid to
-20, since nothing checked.

**What real systems do.** Linux has scheduling CLASSES, compiled in and
queried in a fixed order -- stop, deadline, RT (`SCHED_FIFO`/`SCHED_RR`,
1..99), fair (CFS, EEVDF since 6.6), idle -- and nice is only a WEIGHT
within fair (`sched_prio_to_weight[]`, ~1.25x a step). RT is guarded
twice: `sched_rt_runtime_us` caps all RT at 950 ms of each second, and
`RLIMIT_RTTIME` signals one task that runs too long without blocking.
Raising either needs `CAP_SYS_NICE`, or asking rtkit, which grants RT
within limits. KWin runs its main thread `SCHED_RR` (the binary carries
`CAP_SYS_NICE`) with `SCHED_RESET_ON_FORK`, so what it starts is
ordinary; Mutter can ask rtkit for the same. PipeWire's data thread is `SCHED_FIFO`. Windows
uses one number but in two bands -- 16..31 realtime, 1..15 dynamic --
and MMCSS raises audio threads into the realtime band, then drops them
back when they overrun their reserved share. Pluggable schedulers were
proposed for Linux for years and refused; `sched_ext` (6.12) is BPF
experimentation, not how Linux schedules.

**What toy-os does: Linux's shape, two classes, sized down.**
- **`SCHED_FIFO`/`SCHED_RR` at 1..99 run before every `SCHED_OTHER`
  process**, highest priority first. Within one priority, FIFO order:
  a stamp (`rt_seq`) taken when a process goes to the BACK -- a wake,
  an RR slice's end, a yield -- and kept when it is preempted, so a
  preempted FIFO process resumes at the head as POSIX requires. No
  queues: the picker already scans the table, so a stamp is the whole
  data structure.
- **`SCHED_OTHER` is the old least-`vruntime` rule with nice as CFS's
  weight**: run time is added divided by the weight. The kernel context
  takes part at nice 0.
- **Only init may make anything more important** -- RT, a lower nice,
  or any change to another process; anyone may make itself less
  important (`-EPERM` otherwise). There are no users here, so
  `CAP_SYS_NICE` has one holder, and a service asks through systemd's
  `CPUSchedulingPolicy=`/`CPUSchedulingPriority=`/`Nice=`
  (`data/etc/services.d/README.md`). The obvious alternative, a
  self-raise like the driver's old `setpriority(-10)`, is exactly the
  any-process-can-take-the-machine hole this closes.
- **RT is never inherited** -- `SCHED_RESET_ON_FORK`, always on, for all
  three ways a slot is made. A realtime compositor spawns every app; an
  inheriting class would have made the whole desktop realtime.
- **Two guards, because a spinning RT process owns the machine.** A
  WATCHDOG: one that runs `kernel.sched_rt_watchdog_ms` (1000) without
  BLOCKING -- a yield does not count -- is moved to `SCHED_OTHER` and
  logged, MMCSS's demotion rather than `RLIMIT_RTTIME`'s SIGKILL, since
  the processes this protects (the compositor, the sound driver) are
  ones nobody wants killed. And a THROTTLE: all RT together gets
  `kernel.sched_rt_runtime_ms` (950) of each second while an ordinary
  process wants the CPU. Unlike Linux's it is WORK-CONSERVING -- a
  throttled RT process still runs when nothing ordinary is READY, since
  idling then helps nobody -- and the kernel context does not count as
  wanting the CPU, being the idle loop far more often than the shell.
  The watchdog is the one that ends the episode; the throttle keeps a
  shell alive for the second before it does.
- **A `SCHED_FIFO` process has no slice, only the guards**, so the
  timer still comes round every `kernel.timeslice_ms` to check them --
  without that a FIFO process is never billed and the watchdog never
  fires.

**Measured 2026-10-10**, KVM with a TSC clocksource,
`latency_under_io.py --load cpu` (four busy ordinary `spin_test`s, 15 s),
one build, toywm's descriptor with and without its two CPU lines: the
compositor's frame-wake lateness was 65 us average and 975 us worst as
an ordinary process, against 8-9 us average and 30-34 us worst as
`SCHED_RR 1` (two runs). And a `SCHED_FIFO 50` service that never
blocks was throttled at 950 ms and demoted at 1003 ms, while init's own
"started" line waited for the throttle -- which is the class doing what
it says.

**Not done, deliberately.** No rtkit: a program started by hand cannot
become realtime at all, so a hand-started `snddrv` runs ordinary and
its serving line says so (the roadmap's "Realtime for a program started
by hand"). No deadline class, no idle class, no per-process
`RLIMIT_RTTIME` (one machine-wide figure until a second caller wants a
different one). And no plugin interface -- a seam with one
implementation behind it is unvalidated (`struct win_transport`); two
concrete classes with two real users is the honest version of the same
idea.
