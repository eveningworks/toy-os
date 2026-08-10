# toy-os

A small x86-64 hobby operating system. Boots via GRUB (Multiboot2) into a
64-bit kernel written in C + a bit of Assembly, with a text shell and a
basic graphical mode.

See [CHANGELOG.md](CHANGELOG.md) for the full history of what's been
built, in what order, and the bugs found (and fixed) along the way.
See [docs/decisions.md](docs/decisions.md) for short, topic-indexed
answers to "why does toy-os work this way?" for choices that come up
again once code has grown around them.

## Current features

- Boots via GRUB2 as a Multiboot2 kernel, transitions 32-bit -> 64-bit long
  mode itself (see `kernel/core/boot.asm`)
- Requests a linear RGB framebuffer from GRUB at boot -- 1280x720 by
  default (falls back to classic 80x25 VGA text mode automatically if no
  framebuffer is available)
- A unified console (`vga.c`) that renders through whichever backend is
  active -- everything else just calls `vga_write()` etc without caring
  which one it is
- Serial (COM1) debug logging, visible with `-serial stdio` in QEMU
- IDT + remapped 8259 PIC + basic exception handler (prints and halts on
  CPU faults instead of triple-faulting silently)
- PS/2 keyboard driver (US QWERTY, shift + arrow-key support) feeding a
  line-input shell with command history
- PS/2 mouse driver (IRQ12). Keyboard and mouse share the 8042 data
  port, so both IRQs route through one dispatcher (`i8042.c`) that sorts
  bytes by the controller's AUX status bit -- without that they eat each
  other's input.
- PIT timer (100 Hz tick count) and CMOS RTC (for `time` and the GUI clock)
- A persistent filesystem: a legacy PIO ATA/IDE driver (`kernel/drivers/ata.c`,
  primary bus, polling, no PCI/AHCI needed) backing `tfs.c`'s file
  table with a real on-disk layout (`tfs.c` sits behind a small VFS
  dispatch layer, `vfs.c`/`fs_ops.h`, so a future filesystem can be
  added as a second backend -- see CHANGELOG.md), so files written with
  `write`/`touch`/`append` (or through `SYS_OPEN`/`SYS_WRITE`) survive a
  full power-off, not just the in-VM `reboot` command -- see `about` for
  whether the filesystem currently on disk or RAM-only (no disk found).
  A separate `disk.img` (created once by `make run`/`run-nographic`,
  left alone by `make clean` -- see `make clean-disk` to wipe it) backs
  this; the ISO GRUB boots from is still a read-only CD-ROM image and
  was never involved.
- A real anti-aliased font: JetBrains Mono (OFL 1.1) baked into static
  bitmaps at build time (`tools/genttf.py`, see `tools/OFL.txt` for the
  license), alpha-blended into the framebuffer by `gfx_draw_char()` --
  not runtime TrueType rendering (no floating point or heap allocator in
  the kernel yet), but real font hinting + anti-aliasing baked in
  offline, not the blocky nearest-neighbor-scaled pixel art of the
  original hand-drawn 8x8 font it replaced. Three sizes are baked in
  (small/medium/large) and switchable at runtime with the `fontsize`
  shell command -- see `gfx_set_font_size()`. Used both for the
  framebuffer text console and for on-screen labels in graphics mode.
- A basic GUI mode: a real (if small) window manager -- movable and
  resizable windows (drag an edge/corner) with minimize/maximize/close
  buttons, a taskbar, and a Start menu for launching apps. Four apps
  included: Notepad (with Save/Load through the in-memory filesystem),
  About, Calculator (basic 4-function + %, keyboard or mouse -- see
  `apps/calc_engine.h` for how its fixed-point arithmetic works and how
  to add a new operator), and Terminal (runs the real shell dispatcher
  inside a resizable window -- see `apps/terminal.c` and the CHANGELOG's
  build 253 entry; a short list of commands that don't return or draw
  straight to the physical screen aren't available there). See
  `apps/README.md` for how to add more.
- A small app registry (`apps/apps.c`) so shell, gui, and future programs
  are self-contained, independently addable modules -- see
  **Project layout** below and `apps/README.md` for the walkthrough
- The beginnings of real process isolation: a proper GDT with separate
  kernel/user code+data segments, a TSS (so the CPU has a valid kernel
  stack to switch to if an interrupt fires while running in ring 3), and
  paging support for marking specific 4KiB pages user-accessible while
  everything else in the identity-mapped address space stays
  supervisor-only. See **Process isolation** below.
- A preemptive round-robin scheduler layered on top of that: up to four
  ring-3 processes can be genuinely in flight at once, timer-sliced at
  100Hz, with no cooperation or yield syscall required anywhere. See
  **Process isolation** below and the `schedtest` shell command.
- General-purpose syscalls beyond `write`/`exit`: `SYS_READ_KEY` (poll
  a queued keypress from ring 3) and `SYS_SBRK` (a simple per-process
  heap, backed by the physical frame allocator + `vmm_map_user_page()`).
  See the `echotest` shell command for a real ring-3 program using
  both -- it reads keys and echoes them back live until Esc, entirely
  from user space.
- A real per-window protocol -- `SYS_WIN_CREATE`/`SYS_WIN_PRESENT` --
  where a ring-3 process only ever draws into its own private pixel
  buffer, never the real framebuffer; the kernel composites that
  buffer plus a real title bar and close button onto the screen. A
  genuine client/server split, unlike the earlier `SYS_GUI_INIT` (which
  just hands a process the whole real screen). Still modal (see
  `wintest`'s honest scope note in **Ideas for what's next**), but a
  real step toward one. See the `wintest` shell command.
- Real file I/O syscalls: `SYS_OPEN`/`SYS_READ`/`SYS_CLOSE`, plus
  `SYS_WRITE` extended to take an fd (a breaking ABI change from its
  original always-console 2-arg form -- see `syscall_abi.h`). A ring-3
  process can now open a real named file in the in-memory filesystem,
  write to it, close it, reopen it, and read it back -- the piece a
  future libc's `fopen()`/`fread()`/`fwrite()` would sit on top of. See
  the `filetest` shell command.
- A single version string (`kernel/include/version.h`, `TOYOS_VERSION`)
  shown by both the shell's `about` command and the GUI About window --
  bumped by hand as things land.

### Shell commands

`help` (categorized; `help tests` for the developer/diagnostic ones
below), `clear`, `time`, `uptime`, `echo <text>`, `about`, `meminfo`,
`dmesg`, `color <name>`, `reboot`, `ls`, `cat <f>`, `touch <f>`,
`write <f> <text>`, `append <f> <text>`, `rm <f>`, `gui`, `apps`,
`run <app>`, `history`, `fontsize <tiny|small|medium|large>`, and the
developer/diagnostic set (`help tests`): `ring3test`, `elftest`,
`syscalltest`, `writetest`, `ptrtest`, `guitest`, `schedtest`,
`echotest`, `wintest`, `filetest`, `newsyscalltest`, `crashtest`

## Building on CachyOS

No cross-compiler is needed -- since the target and host are both x86-64,
the system GCC works fine with `-ffreestanding` and kernel-appropriate
flags.

```bash
sudo pacman -S --needed base-devel nasm grub xorriso mtools qemu-full
```

## Build & run

```bash
make          # build kernel.bin only
make iso      # build toy-os.iso (bootable GRUB image)
make run      # build + boot in QEMU with a graphical window
make clean    # remove build artifacts
```

If `make run` doesn't show a window (e.g. over SSH), use:

```bash
make run-nographic   # serial console only, no VGA window
```

Type `help` at the `>` prompt once it boots. Type `gui` for the window
manager -- click Start (bottom-left) to launch Notepad or About, drag
windows by their title bar, and use the minimize/maximize/close buttons
in the top-right of each window. Press Esc to come back to the shell.
Type `apps` at the shell to see everything registered at the console
level, `run <name>` to launch any of them.

## Project layout

toy-os is split into four layers, from the hardware up:

```
kernel/core/     -- boot, interrupts (IDT/PIC), timer, serial, power,
                     multiboot parsing, kernel_main, GDT/TSS (gdt.c),
                     the physical frame allocator (pmm.c), kernel-space
                     paging (paging.c), per-process address spaces incl.
                     user-pointer validation (vmm.c), an ELF64 loader
                     (elf.c), the syscall entry point (syscall.c) and the
                     process-run/return mechanism it uses (process.c,
                     context_switch.asm), and six ring-3 demos
                     (ring3_test.c, elf_test.c, syscall_test.c,
                     write_test.c, ptr_test.c, gui_test.c -- the last one
                     is the first-step "GUI in user space" experiment,
                     see apps/README.md). Hardware bring-up only; knows
                     nothing about apps.
kernel/drivers/  -- device drivers: console (vga.c), framebuffer graphics
                     with double buffering (gfx.c), the bitmap font, the
                     shared PS/2 controller dispatcher (i8042.c) plus the
                     keyboard and mouse behind it, and the in-memory
                     filesystem.
kernel/include/  -- all headers, including kapi.h -- the ONE header apps
                     are supposed to include. It aggregates the driver
                     APIs apps are allowed to use, so drivers can be
                     reshuffled internally without every app needing an
                     edit.
apps/            -- programs. Two kinds:
                     * console apps (shell.c, gui.c) registered in
                       apps.c -- they own the whole screen and run their
                       own loop
                     * GUI apps (notepad.c, about.c, calculator.c)
                       registered in gui_apps.c -- event-driven,
                       launched from the Start menu, drawn into a window
                       by the window manager (apps/wm/ -- split across
                       wm.c/wm_input.c/wm_render.c for readability, see
                       apps/README.md)
                     Adding either is "write the file, add one line to
                     the matching registry" -- see apps/README.md.
                     widgets.h/widgets.c hold small shared button-drawing/
                     hit-testing helpers used by the window manager,
                     Calculator, and Notepad.
userland/        -- freestanding ring-3 test programs (no libc, no
                     crt0), compiled and linked as real ELF64
                     executables via userland/link.ld (-mcmodel=large --
                     see the write syscall bug entry above for why) and
                     loaded by GRUB as Multiboot2 modules (see
                     grub.cfg). hello.c deliberately faults (see
                     ring3test/elftest above); exit_test.c and
                     write_test.c call real syscalls and return cleanly
                     instead -- write_test.c is the only one that
                     produces its own console output. write_bad_test.c
                     deliberately passes an invalid pointer to write, to
                     prove the kernel's pointer validation rejects it.
                     gui_test.c draws directly to the real screen and
                     reads real keyboard input -- see the "GUI in user
                     space" note in apps/README.md for its honest scope.
```

`kernel_main()` (in `kernel/core/kernel.c`) does hardware bring-up --
serial, console, interrupts, filesystem -- then calls `apps_start()`,
which launches the shell. It never mentions the shell by name; the shell
is just the first thing in the app registry. That's the seam that makes
this modular: the kernel core doesn't know or care what apps exist.

Other files:
```
linker.ld    -- links kernel at 1 MiB, matching Multiboot2 conventions
grub.cfg     -- GRUB menu entry pointing at kernel.bin
Makefile     -- build / iso / run targets (globs kernel/core, kernel/drivers,
                apps automatically -- new files are picked up with no
                Makefile edits needed)
tools/genttf.py  -- font source of truth; regenerate kernel/drivers/font_ttf.c
                     from the .ttf here, don't edit that file by hand. Only
                     needed to change the font -- the baked output is
                     already committed, so building/running toy-os itself
                     doesn't need this. Regenerating needs Python 3,
                     Pillow (`pip install pillow`), and the JetBrains Mono
                     font installed (`sudo pacman -S ttf-jetbrains-mono`
                     on CachyOS, or download from jetbrains.com/lp/mono).
                     tools/genfont.py is the retired hand-drawn-8x8-font
                     generator, kept for history/reference only -- nothing
                     includes its output anymore.
tools/OFL.txt    -- SIL Open Font License 1.1 text for JetBrains Mono,
                     the baked font's source face
```

## Process isolation

Everything above (kernel, drivers, shell, GUI apps) runs in one shared
address space at ring 0 -- there's no memory protection between any of
it, and no privilege boundary. `kernel/core/gdt.c`, `kernel/core/pmm.c`,
`kernel/core/paging.c`, `kernel/core/vmm.c`, `kernel/core/elf.c`, and the
two test drivers (`ring3_test.c`, `elf_test.c`) are the start of
changing that.

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
  supervisor-only mapping, unchanged). Used by Milestone 8's version of
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
  from the PMM. `userland/hello.c` is a real, separately compiled and
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
path. `kernel/core/syscall.c` handles `int 0x80` (the gate needs `DPL=3`
in its IDT entry -- 0xEE, not the usual 0x8E -- otherwise ring 3 gets a
`#GP` just trying to invoke it). The only syscall implemented is `exit`
(number `SYS_EXIT`, code in `RDI`), but it's a real one: calling it
doesn't fault or halt -- `kernel/core/process.c`'s `process_run_ring3()`
returns normally with the exit code, exactly as if it were an ordinary
(if unusual) function call. Since there's no scheduler to hand control
back through yet, this works via a small hand-written
setjmp/longjmp-style pair (`kernel/core/context_switch.asm`):
`process_run_ring3()` saves the current kernel execution context before
dropping to ring 3, and the exit syscall jumps straight back into it
from deep inside the interrupt handler -- a completely different call
stack (the TSS's kernel stack, switched to automatically on any
ring3-to-ring0 transition). `kernel/core/syscall_test.c` (shell command
`syscalltest`) demonstrates the whole thing: it loads a second real ELF
binary (`userland/exit_test.c`, GRUB's second `module2` line) that calls
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
it interrupted. `userland/write_test.c` (shell command `writetest`,
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
32-bit displacement to reach); `userland/link.ld` places every test
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
Milestone 8's bug, applied here from the start rather than rediscovered.
Without this, kernel-only memory is still *present* in every process's
page tables (`PML4` entry 0 is shared -- see above), just not
user-accessible, so a process could hand the kernel an address it could
never legally read itself and get the kernel, running at full privilege,
to read it on the process's behalf. `userland/write_bad_test.c` (GRUB's
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

**A first, deliberately narrow step toward GUI in user space:** two more
syscalls, `SYS_GUI_INIT` (maps the real linear framebuffer directly into
the calling process's own address space) and `SYS_GUI_POLL_KEY`
(non-blocking keyboard read). `userland/gui_test.c` (GRUB's fifth
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
but fundamentally a function call, not scheduling. `kernel/core/scheduler.c`
adds honest preemptive multitasking on top, without touching that
mechanism: the two coexist, chosen per-syscall by whether the exiting
process is scheduler-managed. The core trick: `isr_common` (`isr.asm`)
already saved a process's full register state onto whatever stack was
active when an interrupt fired, and -- unmodified since Milestone 8 --
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
shared with anything else. `userland/counter_a.c` / `counter_b.c` --
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

## Ideas for what's next

- ~~Scrollbars for Terminal and Notepad~~ -- done (see CHANGELOG.md's
  builds 263, 273, 283, 293): Page Up/Page Down, a real visual
  draggable scrollbar (click the empty track to page, drag the thumb
  to scroll directly), and the mouse wheel all work in both apps now
  -- Notepad picked up all three for free once it was converted to
  the same `text_scrollback` widget Terminal already used.
- ~~GUI terminal-emulator app~~ -- done (see CHANGELOG.md's builds 183,
  193, 203, and 253 for the finished `apps/terminal.c`). `Terminal` in
  the Start menu runs the real shell dispatcher inside a resizable
  window -- not a reimplementation of it. A short list of commands that
  don't return or draw straight to the physical screen (`gui`, `run`,
  `ring3test`, `elftest`, `guitest`, `wintest`, `schedtest`, `echotest`)
  print an explanation instead of running; everything else, including
  the ring-3 test commands, works for real.
- ~~Process exit/teardown so a faulted or crashed ring-3 process doesn't
  halt the whole kernel~~ -- done (see CHANGELOG.md's "process
  exit/teardown" entry, `crashtest`). `ring3test`/`elftest` still
  require a reboot after their deliberate fault, but on purpose now,
  not for lack of a recovery path -- they drop to ring 3 with their own
  raw, manual iretq instead of `process_run_ring3()`, so there's
  nowhere for the kernel to recover them TO (see `process.h`).
- `g_next_kernel_rsp`'s reentrancy fixed properly (see the
  `echotest`/`SYS_READ_KEY` entry in CHANGELOG.md) so a genuinely
  blocking read -- or any syscall that wants interrupts on while it
  runs -- becomes safe, instead of every blocking-style syscall having
  to spin-poll from ring 3 like `echotest` does today
- `fs_write()` only supports whole-string append/overwrite (no
  offset-based partial writes, no explicit length -- see `filetest`'s
  entry in CHANGELOG.md); a real libc's `fwrite()` would eventually
  want that
- Toward a real C library on top of `filetest`'s fd-aware syscalls: a
  CRT0 that sets up argc/argv from the initial stack, TLS (FS.base)
  support, and FPU/SSE context-switch save/restore -- none of which
  exist yet (see the `filetest` entry in CHANGELOG.md for the full
  breakdown)
- Making `wintest` (`SYS_WIN_CREATE`/`SYS_WIN_PRESENT`) non-modal: a
  `SYS_WIN_*` window is still handed the CPU exclusively while it runs,
  same limitation `guitest` always had, and it isn't a window inside
  the kernel-space window manager's (`wm.c`) own window list. This
  needs the scheduler to give the kernel-space WM loop and a scheduled
  ring-3 process fair turns -- `scheduler_tick()` currently only
  resumes kernel-space code when nothing is `READY`, and once any
  process is armed, kernel-space code doesn't get scheduled again until
  every process exits (see the CHANGELOG entry on why this stayed
  modal). Also: mouse input isn't piped to ring 3 at all yet, so
  `wintest`'s close button is drawn but not clickable.
- Reusable UI widgets -- buttons, text fields, scrollbars -- so GUI apps
  don't each hand-roll their own drawing and hit-testing (Notepad's
  Save/Load buttons and its toolbar are hand-rolled right now)
- ~~Dirty-rectangle rendering instead of the current full-screen repaint.
  Double buffering removed the flicker, but each frame still redrew
  everything and blit the whole screen, which was a lot of wasted work
  when only the cursor moved.~~ -- partially done (see CHANGELOG.md's
  build 337 entry): `gfx_present()` now blits only the bounding box of
  what actually changed instead of the whole screen, and mouse-only
  movement (by far the most common case) takes a cheap cursor-sprite
  save/restore path that skips the full window/taskbar/menu redraw
  entirely. Scene redraws (a click, a drag, a resize, a window opening)
  still repaint the whole back buffer, same as before -- true per-widget
  dirty tracking of the *scene itself*, not just the blit, is still the
  "Full dirty-rect compositor" option that was deliberately not taken
  here (see that build's own reasoning).
- Notepad's filename is currently fixed (`notepad.txt`) -- a simple text
  input widget would let you save/load under different names
- The persistent filesystem's on-disk layout (see CHANGELOG.md) is a
  custom fixed-16-file-slot format, not journaled, and not readable by
  anything but toy-os itself. A host-tool-compatible format (FAT16, most
  realistically) would let you inspect/edit the disk image without
  booting toy-os at all; journaling or copy-on-write would close the
  "crash mid-write corrupts one record" window the current write-through
  approach accepts. AHCI/SATA support would be the other direction to
  take this -- real modern hardware increasingly lacks the legacy IDE
  controller `ata.c` currently depends on.
- The VFS layer (see CHANGELOG.md's build 304, `kernel/include/fs_ops.h`)
  supports exactly one active filesystem backend at a time, chosen once
  at boot -- not multiple backends mounted simultaneously at different
  path prefixes. A real mount-point scheme (`/` on one backend, `/data`
  on another, say) is the natural next step if a second filesystem ever
  actually shows up and needs to coexist with the first, rather than
  replace it; deferred until then since it's meaningfully more code
  (cross-mount path resolution, boundary conflicts) for a capability
  nothing needs yet.
