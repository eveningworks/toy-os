# Process isolation

> **Note on milestone numbers:** this document predates the current
> `docs/roadmap.md` numbering and every "Milestone N" below refers to
> the original pre-v0.1.0 scheme recorded in the git history.
> They are not the milestones in today's roadmap.

The build-up of real ring0/ring3 privilege separation in toy-os, told
as it happened -- what got added, what broke, and how each bug was
found and fixed. Moved out of README.md (which now keeps just a short
summary + a link here) because this is a full implementation
walkthrough, not a feature list entry. See `docs/decisions.md` for
shorter topic-indexed "why" answers, and `git log` for the history this
was assembled from.

By default, the kernel, drivers, shell, and GUI apps all run in one
shared address space at ring 0 -- there's no memory protection between
any of it, and no privilege boundary. `kernel/arch/x86_64/gdt.c`,
`kernel/mm/pmm.c`, `kernel/arch/x86_64/paging.c`, `kernel/mm/vmm.c`,
`kernel/proc/elf.c`, and the two test drivers (`ring3_test.c`,
`elf_test.c`) are the start of changing that.

**What's actually in place:**
- `gdt.c` builds a real GDT (kernel code, kernel data, user code, user
  data) and a TSS, and loads them. The TSS matters more than it might
  look: without a valid `TSS.RSP0`, the very first interrupt that fires
  while the CPU is running in ring 3 -- even just a routine timer tick --
  has no valid kernel stack to switch to and the machine triple-faults.
- `pmm.c` -- a bitmap physical frame allocator (4KiB granularity), built
  from the Multiboot2 memory map (`multiboot_mmap_foreach()`). Only
  memory GRUB reports as "available" is ever handed out; everything from
  address 0 through the kernel image's actual end (`_kernel_end`, a
  linker symbol -- see `linker.ld`) is reserved regardless of what the
  memory map claims about it, since firmware has no idea a kernel is
  sitting there. Also reserves any Multiboot2 module's memory (see
  `elf.c` below) for the same reason. `pmm_alloc_frame()` /
  `pmm_free_frame()` are the whole API; `meminfo` shows live
  total/used/free stats.
- `paging.c` can take any address and make just that one 4KiB page
  user-accessible in the kernel's own (shared) page tables, splitting the
  2MiB huge page that covers it into individual 4KiB pages first
  (everything else in that 2MiB region keeps its original
  supervisor-only mapping, unchanged). Used by the original Milestone 8's
version of
  the ring-3 test; superseded for that purpose by `vmm.c` below, but left
  in place as a general "punch a hole in kernel space" primitive.
- `vmm.c` -- real per-process address spaces. Every process gets its own
  PML4, but shares entry 0 with the kernel's own -- i.e. the same
  physical page-table structures that identity-map the low 4GiB, so
  kernel code, the IDT/GDT, and the framebuffer stay reachable no matter
  which process's `CR3` is loaded (interrupts don't switch `CR3` on
  entry, so this isn't optional). What's actually private per-process is
  a separate virtual range starting at 512GiB (`VMM_USER_BASE`) -- other
  processes' page tables simply have no entry pointing there at all.
  `vmm_create_address_space()` / `vmm_map_user_page()` /
  `vmm_switch_address_space()` are the API.
- `elf.c` -- a minimal ELF64 loader: parses `PT_LOAD` program headers
  (no relocations, no dynamic linking, no section/symbol-table parsing)
  and maps each one via `vmm_map_user_page()`, allocating real frames
  from the PMM. `userland/bin/hello.c` is a real, separately compiled and
  linked ELF64 test program (no libc, no crt0) that GRUB loads into
  memory as a Multiboot2 *module* -- see `grub.cfg`'s `module2` line,
  the standard mechanism for handing a bootloader-loaded file to the
  kernel (think a minimal initrd). `multiboot_get_module()` finds it.
- `ring3_test.c` (shell command `ring3test`) and `elf_test.c` (shell
  command `elftest`) are two variations on the same demonstration: both
  create a private address space with `vmm.c`, drop to ring 3 with a
  manual `iretq`, and run code that writes a marker into its own mapped
  memory before deliberately executing `hlt` (privileged; ring 3 can't
  run it). `ring3_test.c` uses 17 hand-encoded machine-code bytes;
  `elf_test.c` uses `elf.c` to load and run the real compiled
  `hello.elf` instead -- same proof, but now via an actual ELF binary
  with real program headers rather than bytes written directly into a
  page. Either way, the resulting fault is caught by the kernel's
  exception handler, which prints the marker value (proving the code
  really ran), the CS register (proving it was genuinely ring 3), and
  the faulting RIP -- a virtual address like `0x8000000010`, visibly
  different from the physical frame backing it, proof this is real
  address translation and not an identity-mapping shortcut.
  (Told as it happened, per this document's framing: `elf_test.c` and
  its marker-plus-`hlt` mechanism are both gone now, and today's
  `userland/bin/hello.c` is a plain greet-and-exit program -- see
  `docs/decisions.md`'s entry on why, and on the fault it left behind
  when the harness was removed from under it. `ring3_test.c` is
  unchanged and still does exactly what's described above.)

**Four real bugs this caught:**
1. The very first ring-3 attempt faulted with a *page fault* trying to
   fetch the first instruction, not the expected `hlt`-triggered `#GP`.
   The cause: x86-64 paging ANDs the USER bit down the *entire*
   page-table walk, not just the final page -- boot.asm's original PML4E
   and PDPTE entries were only `present | writable`, so even though the
   leaf 4KiB page was correctly marked user-accessible, the CPU still
   blocked ring-3 access because a parent several levels up wasn't.
   Fixed by adding the USER bit to those parent entries too (safe:
   actual access is still gated entirely by the leaf-level entries,
   exactly as before). The same lesson applied directly when writing
   `vmm.c`'s `ensure_next_level()` -- every intermediate PDPTE/PDE it
   creates sets USER too, not just the final PTE, and it was correct on
   the first attempt because the lesson was already documented here.
2. A `str_replace` mistake while adding `multiboot_mmap_foreach()`
   accidentally deleted part of the framebuffer-tag parser it was edited
   next to. Caught immediately by a standalone compile check
   (`gcc -c multiboot.c`) before it ever reached a full build.
3. Not a kernel bug: while testing `vmm.c`, one `ring3test` run appeared
   to hang with no fault at all. Turned out to be the QEMU test harness
   swallowing the first couple of keystrokes of the typed command
   ("ring3test" landed as "3test", an unknown command). Slowing down the
   synthetic keypresses fixed it, and it reproduced correctly and
   deterministically across 5 repeated boots afterward.
4. The first `elftest` run faulted with `entry point: 0x0` -- the ELF
   header read back as all zeros partway through loading, even though
   the file on disk and the raw bytes in memory (checked with `od` and a
   standalone userspace parser using the exact same struct, both
   independently before suspecting the kernel code itself) were
   correct. The actual cause: `pmm_init()` only reserved memory through
   the kernel's own image -- but GRUB places a Multiboot2 module
   wherever it likes in physical memory, which isn't necessarily
   anywhere near the kernel. `elf.c`'s very first `pmm_alloc_frame()`
   call was handing back the module's *own* memory, and the segment
   loader zeroed that frame before copying into it -- wiping out the ELF
   header while the loader was still reading from it. Fixed by having
   `pmm_init()` also reserve whatever `multiboot_get_module()` reports.

**What's actually in place beyond the four items above:** a real syscall
path. `kernel/proc/syscall.c` handles `int 0x80` (the gate needs `DPL=3`
in its IDT entry -- 0xEE, not the usual 0x8E -- otherwise ring 3 gets a
`#GP` just trying to invoke it). The only syscall implemented is `exit`
(number `SYS_EXIT`, code in `RDI`), but it's a real one: calling it
doesn't fault or halt -- `kernel/proc/process.c`'s `process_run_ring3()`
returns normally with the exit code, exactly as if it were an ordinary
(if unusual) function call. Since there's no scheduler to hand control
back through yet, this works via a small hand-written
setjmp/longjmp-style pair (`kernel/arch/x86_64/context_switch.asm`):
`process_run_ring3()` saves the current kernel execution context before
dropping to ring 3, and the exit syscall jumps straight back into it
from deep inside the interrupt handler -- a completely different call
stack (the TSS's kernel stack, switched to automatically on any
ring3-to-ring0 transition). `syscall_test.c` -- long since replaced by a real /bin binary (shell command
`syscalltest`) demonstrates the whole thing: it loads a second real ELF
binary (`userland/tests/exit_test.c`, GRUB's second `module2` line) that calls
`exit(42)` instead of deliberately faulting, and prints the exit code it
gets back. Unlike `ring3test`/`elftest`, this command *returns* -- the
shell keeps running normally afterward.

**Two more real bugs this caught -- both in the context-switch design itself:**
5. The first working version of `process_context_restore()` used a plain
   `ret` to jump back, trusting that the stack memory at the saved `RSP`
   still held the original return address. It didn't: after
   `process_context_save()` returns, `process_run_ring3()` immediately
   pushes more data (SS/RSP/RFLAGS/CS/RIP, building the `iretq` frame) --
   and those pushes legitimately reuse that exact, now-"freed" stack
   slot. By the time the exit syscall fired, that memory held part of
   the `iretq` frame instead of the original return address, and jumping
   to it landed on garbage (confirmed by instrumenting both sides with
   serial output and comparing the saved vs. actual memory content --
   they'd diverged). Fixed the way real `setjmp`/`longjmp`
   implementations do: capture the return address directly into the
   saved context as data (read once, before anything else can overwrite
   it), and `jmp` to that saved value directly instead of trusting the
   stack to still hold it.
6. Even after that fix, the shell stopped responding to the keyboard
   after a successful `syscalltest` run -- the exit code printed
   correctly, but no further input worked. Cause: `int 0x80`'s interrupt
   gate clears the interrupt flag on entry, same as every other
   interrupt gate here; the normal path re-enables it as a side effect
   of `iretq` restoring the saved `RFLAGS`. `process_context_restore()`
   bypasses `iretq` entirely with a direct jump, so interrupts never got
   re-enabled -- meaning IRQ1 (keyboard) could never fire again after the
   first exit syscall. Fixed with an explicit `sti` right before the
   jump.

**What's actually in place beyond `exit`:** a second syscall, `write`
(`SYS_WRITE`, buffer pointer in `RDI`, length in `RSI`, capped at
`SYS_WRITE_MAX` bytes per call, returns bytes written via `RAX`). Both
syscall numbers now live in one shared header, `syscall_abi.h`, included
by both the kernel's dispatcher and every userland test program, so the
numbers can't drift out of sync between the two sides the way `SYS_EXIT`
briefly did (it used to be defined separately in each). `write` is
simpler than `exit` to implement: it doesn't need the
`process_context_restore()` jump back to the kernel caller at all --
it just performs the write and returns normally, resuming ring 3 right
after the `int 0x80`, same as any hardware interrupt returns to whatever
it interrupted. `userland/tests/write_test.c` (shell command `writetest`,
GRUB's third module) is the first userland program in this project
whose console output the process produced *itself* -- watch for
"Hello from ring 3, printed via a real write syscall!" appearing inline
with the kernel's own messages, not printed by the kernel on the
process's behalf.

**One more real bug, and it's a genuinely useful one to know about:**
the very first build of `write_test.c` failed to *link* --
`relocation truncated to fit` against its own string literal. Cause: the
default x86-64 code model assumes a program's code and data live within
the low 32 bits of address space (or close enough for a RIP-relative
32-bit displacement to reach); `userland/rt/link.ld` places every test
program at `VMM_USER_BASE` (512GiB), and `write_test.c` was the first
one to reference something outside its own code (a string in
`.rodata`), which needs the compiler to compute that string's address
somehow. Neither `hello.c` nor `exit_test.c` hit this, since neither
ever referenced anything beyond hardcoded integer immediates. Fixed by
adding `-mcmodel=large` to `USERLAND_CFLAGS` (verified via `objdump`
that the fix produces a real `movabs` full-64-bit load instead of a
truncated relocation) -- applied to all userland builds now, so this
doesn't reappear the next time a test program needs a global.

**The safety gap from before is now closed.** The `write` syscall no
longer trusts its buffer pointer -- `vmm_validate_user_range()` (new in
`vmm.c`) walks the calling process's own page tables (read via `CR3`,
which a syscall doesn't change) and confirms every page in
`[buf, buf+len)` is present *and* user-accessible at every level of the
walk, not just the leaf -- the same "check every level" lesson from
the original Milestone 8's bug, applied here from the start rather than
rediscovered.
Without this, kernel-only memory is still *present* in every process's
page tables (`PML4` entry 0 is shared -- see above), just not
user-accessible, so a process could hand the kernel an address it could
never legally read itself and get the kernel, running at full privilege,
to read it on the process's behalf. `userland/tests/write_bad_test.c` (GRUB's
fourth module, shell command `ptrtest`) proves the fix actually works,
the same way `ring3test`/`elftest` prove isolation: it deliberately
passes address `0x1000` (real, present, but kernel-only) to `write`, and
the test passes only if the kernel *rejects* it (returns `-1`) rather
than reading it. Reverified that a genuinely valid pointer
(`writetest`'s message buffer) still works exactly as before -- the risk
with adding validation is rejecting something that should have been
allowed, not just failing to reject something that shouldn't.

**What this deliberately is not yet:** a real process model. `exit` and
`write` are the only general-purpose syscalls; there's no scheduler
(only one process can be "in flight" through `process_run_ring3()` at a
time -- see its header comment); and there's still no memory allocation,
file I/O, or general input syscall -- so a real interactive program
still couldn't do much.

**A first, deliberately narrow step toward GUI in user space** (retired
2026-09-09 -- the compositor's role-gated `WIN_REQ_FB_MAP` replaced it,
and an unguarded map of the screen for any process was a hole): two more
syscalls, `SYS_GUI_INIT` (maps the real linear framebuffer directly into
the calling process's own address space) and `SYS_GUI_POLL_KEY`
(non-blocking keyboard read). `userland/tests/gui_test.c` (GRUB's fifth
module, shell command `guitest`) is the first ring-3 process in this
project to draw real pixels and read real input with zero kernel-space
drawing code involved once it's running -- it fills the screen with a
color and cycles it on each keypress, exiting cleanly via `exit` on
`q`. Verified precisely: the color after each keypress matches the
exact 24-bit arithmetic `gui_test.c` performs (e.g.
`0x224477 + 0x335577 = 0x5599EE`), confirming the pixels on screen
really did come from ring-3 code doing real math, not something
kernel-side coincidentally matching.

This is **not** the window manager moved to user space -- see
`apps/README.md`'s "GUI in user space" section for the honest scope.
It's modal (no scheduler, so the process has the whole real screen to
itself while it runs, same as every other ring-3 test here), it isn't a
window inside `wm.c`, and `wm.c` doesn't know it exists. Confirmed both
GUI tracks coexist without interfering: ran `guitest`, then separately
opened the kernel-space window manager (`gui`, Notepad) afterward in the
same boot and both worked correctly.

**A real scheduler, at last:** everything above still ran at most one
ring-3 process at a time -- `process_run_ring3()` drops to ring 3 and
only gets control back when that process calls `exit`, via a
setjmp/longjmp-style save/restore of the caller's kernel context. Useful,
but fundamentally a function call, not scheduling. `kernel/proc/scheduler.c`
adds honest preemptive multitasking on top, without touching that
mechanism: the two coexist, chosen per-syscall by whether the exiting
process is scheduler-managed. The core trick: `isr_common` (`isr.asm`)
already saved a process's full register state onto whatever stack was
active when an interrupt fired, and -- unmodified since the original
Milestone 8 --
just popped those same registers back off *that same* stack and
`iretq`'d, resuming exactly what was interrupted. The scheduler
generalizes that last step: `isr_common` now reloads `rsp` from a global,
`g_next_kernel_rsp` (`idt.c`), right before the pop+`iretq`.
`isr_dispatch` resets it to a no-op at the top of every call; only
`scheduler_tick()` (called on every timer tick, vector 32, but only when
armed) or `scheduler_on_exit()` (called from the exit syscall, only for
scheduler-managed processes) ever point it somewhere else -- another
process's saved register block, or back to the shell. Up to four
processes (`MAX_PROCS`) can be READY at once, each with its own
dedicated kernel stack used as `TSS.RSP0` while it runs, so a preempting
interrupt always lands on that process's own stack rather than one
shared with anything else. `userland/tests/counter_a.c` / `counter_b.c` --
two tiny freestanding ring-3 programs with no yield syscall anywhere,
each printing its own letter 20 times with a long busy-spin between
prints -- are the proof: if the output interleaves instead of printing
20 As followed by 20 Bs, the only possible explanation is the timer
preempting one process mid-spin and handing the CPU to the other. Shell
command `schedtest` spawns both, arms the scheduler, blocks until both
exit, and disarms it again -- every other command behaves exactly as it
did before, confirmed by running `syscalltest`, `writetest`, and
`ptrtest` immediately before `schedtest` in the same boot and watching
all four complete correctly back to back, and by reconfirming `ring3test`
and `elftest` still panic and halt with their expected diagnostics,
untouched.

