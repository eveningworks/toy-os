# grub-mkrescue is named grub2-mkrescue on Fedora/RHEL and openSUSE.
# Resolved here rather than documented as a "symlink it yourself" step,
# so `make iso` just works on those distributions.
# Seconds GRUB waits on its menu before booting the default entry. 0
# (the default) draws no menu at all and boots instantly, which is what
# every automated path wants -- `make run-menu` overrides it.
GRUB_TIMEOUT ?= 0

GRUB_MKRESCUE := $(shell command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null)

CC = gcc
LD = ld
ASM = nasm

# kernel/include holds the driver/core headers + kapi.h (the boundary
# apps are supposed to stick to). apps/ is also on the include path so
# core code can reach apps.h to call apps_start(), and so apps can
# include each other's headers if that's ever needed.
# -MMD -MP: emit a .d dependency file alongside each .o (see the
# -include line near the bottom of this file) so changing a header
# rebuilds every .o that includes it, not just files whose own .c
# changed -- see that -include line's comment for the full reasoning
# and kernel/include/api/version.h's generation (tools/gen_version.sh) for
# the one subtlety this tracking requires upstream of it.
# -g: DWARF debug info, for `make debug` (see that target below) --
# GDB can already attach to QEMU's own built-in gdbstub with zero
# kernel-side code (see docs/decisions.md for why an in-kernel GDB
# remote-serial-protocol stub is unnecessary: QEMU emulates the CPU
# directly, so real breakpoints/single-step/register-memory inspection
# work regardless of what the guest OS does), but without -g GDB only
# ever sees raw addresses -- no function names, no source lines. Kept
# at -O2 (not dropped to -Og/-O0) deliberately -- same binary as
# always, just now carrying symbols; some locals may show "optimized
# out" in GDB, a tradeoff accepted in favor of not needing a second
# build config to keep in sync.
# -fstack-protector-strong + -mstack-protector-guard=global: stack
# canaries (Milestone 2, docs/roadmap.md), explicitly off before now.
# `global` (a plain extern uintptr_t __stack_chk_guard, see
# kernel/lib/stack_protector.c) instead of the default `tls` guard --
# GCC's default reads the canary via %fs:0x28, and this kernel never
# sets up a per-CPU/per-thread FS/GS base (no TLS infrastructure
# exists at all, see docs/decisions.md), so the TLS-based default
# would dereference an unconfigured segment. `strong` (not plain
# `-fstack-protector`, and not `-all`) instruments any function with a
# local array or a local whose address is taken -- the same middle
# ground GCC itself recommends over the two extremes.
# ---- header surfaces (see kernel/include/README.md) ----
#
# Three include directories, three audiences, enforced by which -I flags
# each thing is compiled with rather than by convention:
#
#   include/api/     what apps/ may use -- kapi.h and everything it
#                    aggregates. Anything in here is a promise.
#   include/abi/     the kernel<->userland contract (syscall numbers,
#                    the ELF entry contract). Shared by the kernel and
#                    the freestanding userland/ programs, nobody else.
#   include/kernel/  kernel internals -- paging, the syscall
#                    implementation, driver-private headers, the
#                    filesystem backend vtable. NOT on apps/'s or
#                    userland/'s include path, so reaching for one is a
#                    compile error rather than a code-review catch.
#
# This is the boundary CLAUDE.md has always described; before the split
# every header sat in one flat directory and nothing enforced it.
API_INCLUDES    = -Ikernel/include/api -Ikernel/include/abi
KERNEL_INCLUDES = $(API_INCLUDES) -Ikernel/include/kernel

CFLAGS = -std=gnu11 -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie \
         -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
         -Wall -Wextra -O2 -g -c $(KERNEL_INCLUDES) -Iapps -MMD -MP

APPS_CFLAGS = $(subst -Ikernel/include/kernel,,$(CFLAGS))

LDFLAGS = -n -T linker.ld -nostdlib

ASMFLAGS = -f elf64

BUILD = build
KERNEL = $(BUILD)/kernel.bin
ISO = toy-os.iso

# A separate, genuinely writable raw disk image for kernel/drivers/ata.c
# + fs.c's persistent filesystem -- NOT part of the ISO above, which
# GRUB/QEMU only ever mount as a read-only CD-ROM (El Torito). Created
# once and then left alone by every other target (including `clean` --
# see below) so files written during one `make run` are still there the
# next time, exactly the point of it existing.
#
# 9GiB, sparse -- grown from the original 1MiB when TFS2's on-disk
# format switched from one fixed-size inline data blob per file to a
# real block allocator + indirect pointers, specifically to support
# multi-gigabyte files (see kernel/fs/tfs.c's FS_DISK_TOTAL_BYTES,
# which MUST match this). `truncate` makes a sparse file -- the actual
# bytes on YOUR disk only grow as toy-os actually writes into the
# image, not upfront, so creating this doesn't eat 9GiB of real space
# by itself (most filesystems support sparse files; if yours doesn't,
# this will actually allocate the full 9GiB up front).
#
# IMPORTANT: this rule only runs if disk.img doesn't already exist (see
# below) -- an existing 1MiB disk.img from before this change will NOT
# be auto-grown by `make`. TFS2's on-disk format also changed
# incompatibly at the same time (see tfs.c's top-of-file warning), so
# there's no upgrade path for an old image anyway: run `make clean-disk`
# once (wipes disk.img -- existing saved files are lost either way) to
# get a fresh, correctly-sized one.
DISK_IMG = disk.img

# userland test programs (see userland/README or kernel's "Process
# isolation" README section) -- plain freestanding binaries, no
# kernel-specific flags like -mcmodel=kernel needed since these run in
# ordinary ring-3 user space, not as part of the kernel image.
# -mcmodel=large IS needed though: these link at VMM_USER_BASE
# (512GiB), and the default code model can't reach a global (e.g. a
# string literal) from that address with a 32-bit relocation -- the
# linker fails with "relocation truncated to fit" without this.
# Same stack-canary flags as CFLAGS above, and the same reasoning --
# see that comment. Every userland ELF needs userland/stack_chk.c's
# __stack_chk_guard/__stack_chk_fail linked in now (see SEED_BINARIES
# below and each $(FOO_ELF) rule) since GCC emits implicit references
# to both from any protected function in any userland .c file.
# NOTE the deliberate asymmetry with CFLAGS above: userland ELFs are
# built WITHOUT -mno-mmx/-mno-sse/-mno-sse2, so ring-3 code gets real
# hardware floating point and SSE. The kernel (and apps/, which is ring
# 0 here) keeps them, and that split is the whole design -- Linux builds
# its own kernel with these same flags and brackets the rare kernel-side
# SIMD in kernel_fpu_begin()/kernel_fpu_end(); Windows requires
# KeSaveExtendedProcessorState() for the same reason. See
# kernel/include/kernel/fpu.h for why copying that split matters here:
# with SSE on, GCC emits XMM in ordinary code (struct copies, inlined
# memcpy), so an FP-enabled kernel would need an FXSAVE on every
# interrupt vector rather than only where the scheduler swaps processes.
USERLAND_CFLAGS = -std=gnu11 -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie \
                   -mno-red-zone -mcmodel=large \
                   -Wall -Wextra -O2 -g -c $(API_INCLUDES) -MMD -MP
HELLO_ELF = userland/hello.elf
EXIT_TEST_ELF = userland/exit_test.elf
WRITE_TEST_ELF = userland/write_test.elf
WRITE_BAD_TEST_ELF = userland/write_bad_test.elf
GUI_TEST_ELF = userland/gui_test.elf
COUNTER_A_ELF = userland/counter_a.elf
COUNTER_B_ELF = userland/counter_b.elf
ECHO_ELF = userland/echo.elf
WIN_TEST_ELF = userland/win_test.elf
FILE_TEST_ELF = userland/file_test.elf
NEWSYSCALLS_TEST_ELF = userland/newsyscalls_test.elf
CRASH_TEST_ELF = userland/crash_test.elf
SOCKET_TEST_ELF = userland/socket_test.elf
LSPCI_ELF = userland/lspci.elf
LS_ELF = userland/ls.elf
STACK_SMASH_TEST_ELF = userland/stack_smash_test.elf
NX_TEST_ELF = userland/nx_test.elf
FPU_TEST_ELF = userland/fpu_test.elf
FPU_RACE_ELF = userland/fpu_race.elf
LSCPU_ELF = userland/lscpu.elf
SPIN_TEST_ELF = userland/spin_test.elf
EVENT_TEST_ELF = userland/event_test.elf
WINCLIENT_ELF = userland/winclient.elf
UICLIENT_ELF = userland/uiclient.elf
CALCULATOR_ELF = userland/calculator.elf
NOTEPAD_ELF = userland/notepad.elf
PIPE_TEST_ELF = userland/pipe_test.elf
UTERM_ELF = userland/terminal.elf
GFXDEMO_ELF = userland/gfxdemo.elf

# Which userland ELFs get seeded onto disk.img's /bin, and under what
# name. The mapping is explicit because it isn't always mechanical --
# echo.c is seeded as `echo_test`.
#
# MUST be defined above `all:`: make expands a rule's prerequisite list
# when it READS the rule, not when it runs it, so USERLAND_ELVES below
# would expand to nothing if this lived further down the file (which is
# exactly what happened while writing this -- `make all` silently built
# no ELFs at all and still exited 0).
# Real user-facing programs -> /bin. Kept deliberately short: /bin is
# what a person sees when they type `ls /bin`, and what PATH offers
# first. See docs/filesystem-layout.md.
# Real user-facing programs -> /bin. The four ring-3 GUI apps here
# (calculator, notepad, uterm, shapes) sat in /tests until the Start
# menu learned to launch them: every client written during the ring-3
# migration landed in /tests because that is where the first one went,
# and nobody moved them once they stopped being experiments. A program
# offered in the Start menu is user-facing by definition, and /tests is
# explicitly "not things a user of the OS wants offered to them" --
# see docs/filesystem-layout.md, including the note on cleaning up the
# stale copies a move leaves behind on existing images.
SEED_PROGRAMS = \
	$(LSPCI_ELF):lspci \
	$(LSCPU_ELF):lscpu \
	$(LS_ELF):ls \
	$(HELLO_ELF):hello \
	$(CALCULATOR_ELF):calculator \
	$(NOTEPAD_ELF):notepad \
	$(UTERM_ELF):uterm \
	$(GFXDEMO_ELF):shapes

# Test/demo binaries -> /tests. These are exercises of one kernel
# mechanism each (a deliberate fault, a syscall round-trip, a window),
# not things a user of the OS wants offered to them. They used to sit in
# /bin alongside the real programs, where they outnumbered them 14 to 3.
# /tests is deliberately NOT an FHS directory -- see
# docs/filesystem-layout.md for why that exception was made rather than
# using /usr/libexec.
SEED_TESTS = \
	$(EXIT_TEST_ELF):exit_test \
	$(WRITE_TEST_ELF):write_test \
	$(WRITE_BAD_TEST_ELF):write_bad_test \
	$(GUI_TEST_ELF):gui_test \
	$(COUNTER_A_ELF):counter_a \
	$(COUNTER_B_ELF):counter_b \
	$(ECHO_ELF):echo_test \
	$(WIN_TEST_ELF):win_test \
	$(FILE_TEST_ELF):file_test \
	$(NEWSYSCALLS_TEST_ELF):newsyscalls_test \
	$(CRASH_TEST_ELF):crash_test \
	$(SOCKET_TEST_ELF):socket_test \
	$(STACK_SMASH_TEST_ELF):stack_smash_test \
	$(NX_TEST_ELF):nx_test \
	$(FPU_TEST_ELF):fpu_test \
	$(FPU_RACE_ELF):fpu_race \
	$(SPIN_TEST_ELF):spin_test \
	$(PIPE_TEST_ELF):pipe_test \
	$(EVENT_TEST_ELF):event_test \
	$(WINCLIENT_ELF):winclient \
	$(UICLIENT_ELF):uiclient

# Both lists together -- only USERLAND_ELVES below needs the union, so
# it's derived rather than maintained as a third list.
SEED_BINARIES = $(SEED_PROGRAMS) $(SEED_TESTS)

# Every ELF named in SEED_BINARIES, without the :diskname suffix -- what
# `all` and `iso` actually have to build. Derived rather than listed
# again, so the two can't drift.
USERLAND_ELVES = $(foreach pair,$(SEED_BINARIES),$(firstword $(subst :, ,$(pair))))

# Seed directory for the writer tools' `sync` (reached through
# tools/seed_disk.py, which probes the image's format) -- see the
# `seed` target below and docs/decisions.md. Not committed as a
# generic directory: SEED_DIR/sync/bin/* are build-generated copies of
# each SEED_BINARIES entry, staged fresh by the `seed` target's own
# recipe every build, not tracked source files.
SEED_DIR = seed

# The PCI ID Database (pci-ids.ucw.cz), bundled rather than fetched or
# read from the build host, so a build is reproducible and works
# offline. Read at runtime by /bin/lspci to turn 8086:7010 into "Intel
# Corporation 82371SB PIIX3 IDE"; the `seed` target below stages it to
# /usr/share/hwdata/pci.ids on the disk image, the same path Linux
# distributions use. NOT MIT -- redistributed under its 3-clause BSD
# option, see LICENSE. Refreshing it is a deliberate commit: download a
# new copy from https://pci-ids.ucw.cz/v2.2/pci.ids over data/pci.ids.
PCI_IDS = data/pci.ids

# Source discovery is RECURSIVE and automatic: every .c under kernel/
# or apps/ is compiled, and every .asm under kernel/ is assembled, with
# build/ mirroring the source tree. Adding a directory needs no Makefile
# edit at all.
#
# It used to be one hand-written wildcard + pattern rule + mkdir target
# per directory, which is a tax paid every time the tree grows -- both
# apps/wm/ and apps/ui/ needed that treatment when they appeared, and
# the reshape into kernel/arch, kernel/mm, kernel/proc, kernel/fs and
# kernel/lib would have needed five more. `find` costs one subprocess
# per build and removes the whole category.
#
# The one thing to know: a .c file anywhere under kernel/ or apps/ is
# now IN the kernel image. There's no "scratch file in the source tree"
# that the build ignores -- put throwaway code somewhere else.
C_SOURCES   = $(shell find kernel apps -name '*.c' | sort)
ASM_SOURCES = $(shell find kernel -name '*.asm' | sort)

C_OBJECTS   = $(patsubst %.c,   $(BUILD)/%.o, $(C_SOURCES))
ASM_OBJECTS = $(patsubst %.asm, $(BUILD)/%.o, $(ASM_SOURCES))

.PHONY: all clean clean-disk iso run run-menu run-audio run-kvm run-nographic debug help version seed test verify

# Regenerates kernel/include/api/version.h from VERSION (see
# tools/gen_version.sh) -- listed first so it always runs before
# anything gets compiled. Always considered out of date (.PHONY), so
# every `make all`/`make iso` re-embeds whatever VERSION currently
# holds -- but this step never changes VERSION itself; see
# tools/set_version.sh for the separate, deliberate step that does
# (starting a new dev round or cutting a release, not on every build
# or even every change -- see docs/decisions.md).
#
# Used to force-delete build/apps/about.o and build/apps/shell.o here,
# because this Makefile didn't track header dependencies at all (no
# -MMD/-MP) and version.h changing alone would never otherwise trigger
# anything to recompile. Now that -MMD/-MP is on (see CFLAGS above and
# the -include line near the bottom of this file), that's handled
# generically -- any .o whose .c (transitively) includes version.h
# rebuilds automatically once it actually changes. gen_version.sh is
# deliberately idempotent (only touches version.h's mtime when
# VERSION's value actually changed) specifically so this doesn't
# regress into "every file that includes kapi.h rebuilds on every
# single build" -- see that script's top comment.
version:
	@sh tools/gen_version.sh

all: version $(KERNEL) $(USERLAND_ELVES)

help:
	@echo "toy-os -- available targets:"
	@echo "  all            Build kernel.bin and the userland test ELFs (default)"
	@echo "  iso            Build toy-os.iso, a bootable GRUB ISO (implies all)"
	@echo "  run            Boot toy-os.iso in QEMU with an SDL window (implies iso)"
	@echo "  run-menu       Same, but with the GRUB boot menu visible (5s timeout)"
	@echo "  run-audio      Same as run, plus a PulseAudio backend so the PC speaker"
	@echo "                 (beep) is actually audible -- see the Makefile for how to"
	@echo "                 swap the backend if you're not on PulseAudio"
	@echo "  run-kvm        Same as run, but KVM-accelerated instead of TCG emulation --"
	@echo "                 needs /dev/kvm; port-I/O-heavy paths can get slower, so"
	@echo "                 don't compare its throughput numbers against run's"
	@echo "  run-nographic  Boot toy-os.iso in QEMU with no display, serial only (implies iso)"
	@echo "  debug          Boot toy-os.iso frozen (QEMU's -s -S) for real GDB"
	@echo "                 debugging -- attach with: gdb build/kernel.bin -ex"
	@echo "                 'target remote localhost:1234', then continue"
	@echo "  test           Run the in-kernel test suite (ktest) and exit non-zero on failure"
	@echo "  verify         Full pre-delivery check: clean build + iso + boot test + ktest"
	@echo "  clean          Remove build outputs (build/, ELFs, toy-os.iso) -- leaves disk.img alone"
	@echo "  clean-disk     Wipe disk.img, the persistent filesystem -- use with care"
	@echo "  version        Regenerate kernel/include/api/version.h (runs automatically as part of all/iso)"
	@echo "  help           Show this message"

# Two generic rules, mirroring the source tree into build/. The mkdir
# is per-target (the object's own directory) rather than a set of
# order-only prerequisites naming every directory, so a new source
# directory needs nothing here either.
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(if $(filter apps/%,$<),$(APPS_CFLAGS),$(CFLAGS)) $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

# Userland ELFs: two generic rules instead of three hand-written lines
# per binary (17 binaries = ~55 lines before this). A new /bin program is
# now a .c file plus one SEED_BINARIES entry -- and that entry is needed
# only because the on-disk name isn't always the file name (echo.c is
# seeded as `echo_test`).
$(BUILD)/userland/%.o: userland/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USERLAND_CFLAGS) $< -o $@

# stack_chk.o carries __stack_chk_guard/__stack_chk_fail and is linked
# into every userland ELF -- see USERLAND_CFLAGS's comment above.
# .SECONDARY keeps the per-binary .o files: without it make treats them
# as intermediates of the pattern-rule chain and deletes them after
# linking, so every build recompiles all 17 userland programs.
.SECONDARY:

# The C runtime every ring-3 program now starts through. crt0.o must be
# FIRST on the link line so _start lands at the lowest address in .text
# -- userland/link.ld has no ENTRY() override, and the loader jumps to
# the ELF header's e_entry, which ld takes from the _start symbol; being
# first also keeps the entry point where a disassembly expects it.
#
# sys.o carries the syscall wrappers (userland/sys.c) and stack_chk.o
# the canary symbols GCC emits references to. All three are linked into
# every userland ELF, which is what lets a program be nothing but its
# own main().
$(BUILD)/userland/crt0.o: userland/crt0.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

USERLAND_RT = $(BUILD)/userland/crt0.o $(BUILD)/userland/sys.o $(BUILD)/userland/stack_chk.o

userland/%.elf: $(BUILD)/userland/%.o $(USERLAND_RT) userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/crt0.o $< $(BUILD)/userland/sys.o $(BUILD)/userland/stack_chk.o

# Window clients that draw with the userland graphics runtime
# (userland/ugfx.c) link it in explicitly, via a rule that overrides the
# pattern above for just those binaries.
#
# Deliberately NOT added to the pattern rule's common objects the way
# stack_chk.o is: stack_chk.o is needed by every userland binary (GCC
# emits references to it from any protected function), whereas ugfx.o is
# wanted only by window clients -- and with no --gc-sections here, adding
# it globally would link the whole font-rendering path into programs
# like `hello` that never draw anything.
userland/uiclient.elf: $(BUILD)/userland/uiclient.o $(BUILD)/userland/ugfx.o $(BUILD)/userland/shared/geom.o $(BUILD)/userland/shared/fixed.o $(USERLAND_RT) userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/crt0.o $(BUILD)/userland/uiclient.o $(BUILD)/userland/ugfx.o $(BUILD)/userland/shared/geom.o $(BUILD)/userland/shared/fixed.o $(BUILD)/userland/sys.o $(BUILD)/userland/stack_chk.o

# Sources SHARED between the kernel image and userland ELFs, compiled a
# second time with USERLAND_CFLAGS into build/userland/shared/.
#
# This is not duplicated code -- it is the same .c file built for a
# different target. The kernel objects are -mcmodel=kernel and cannot be
# linked into a ring-3 ELF (which is -mcmodel=large and links at
# VMM_USER_BASE), so a second compile is the only way to share the
# SOURCE. That matters most for apps/calc_engine.c: the ring-3
# Calculator runs the identical arithmetic as the kernel-space one
# because there is exactly one engine, not two that have to be kept in
# step.
#
# Everything listed here must be freestanding -- kernel/lib/string.c and
# knum.c include only <stddef.h>/<stdint.h>, and calc_engine.c only
# those two headers. A file that reaches for kernel state does not
# belong on this path.
#
# -Iapps is needed for calc_engine.h and is scoped to this rule alone,
# so an ordinary userland program still cannot include apps/ headers.
$(BUILD)/userland/shared/%.o: kernel/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USERLAND_CFLAGS) -Iapps $< -o $@

$(BUILD)/userland/shared/%.o: apps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USERLAND_CFLAGS) -Iapps $< -o $@

# calculator.c is the one ordinary userland program that includes an
# apps/ header (calc_engine.h). Scoped to this object with a
# target-specific variable rather than added to the pattern rule above,
# so no OTHER userland program gains the ability to reach into apps/.
$(BUILD)/userland/calculator.o: USERLAND_CFLAGS += -Iapps

# The ported Calculator: its own code, the userland widget toolkit, and
# the shared arithmetic engine.
CALC_OBJS = $(BUILD)/userland/crt0.o \
            $(BUILD)/userland/calculator.o \
            $(BUILD)/userland/uui.o \
            $(BUILD)/userland/ugfx.o \
            $(BUILD)/userland/shared/geom.o \
            $(BUILD)/userland/shared/fixed.o \
            $(BUILD)/userland/shared/calc_engine.o \
            $(BUILD)/userland/shared/string.o \
            $(BUILD)/userland/shared/knum.o \
            $(BUILD)/userland/sys.o \
            $(BUILD)/userland/stack_chk.o

userland/calculator.elf: $(CALC_OBJS) userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(CALC_OBJS)

# Notepad: the userland widget toolkit plus the text buffer.
NOTEPAD_OBJS = $(BUILD)/userland/crt0.o \
               $(BUILD)/userland/notepad.o \
               $(BUILD)/userland/uui.o \
               $(BUILD)/userland/utext.o \
               $(BUILD)/userland/ugfx.o \
               $(BUILD)/userland/shared/geom.o \
               $(BUILD)/userland/shared/fixed.o \
               $(BUILD)/userland/sys.o \
               $(BUILD)/userland/stack_chk.o

userland/notepad.elf: $(NOTEPAD_OBJS) userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(NOTEPAD_OBJS)

# The ring-3 Terminal: the text widget, the shell it links against, and
# the runtime. Seeded as `uterm` so it doesn't collide with the
# kernel-space Terminal in the Start menu while both exist.
UTERM_OBJS = $(BUILD)/userland/crt0.o \
             $(BUILD)/userland/terminal.o \
             $(BUILD)/userland/ush.o \
             $(BUILD)/userland/uui.o \
             $(BUILD)/userland/utext.o \
             $(BUILD)/userland/ugfx.o \
             $(BUILD)/userland/shared/geom.o \
             $(BUILD)/userland/shared/fixed.o \
             $(BUILD)/userland/sys.o \
             $(BUILD)/userland/stack_chk.o

userland/terminal.elf: $(UTERM_OBJS) userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(UTERM_OBJS)

# The shapes demo: the widget toolkit plus the SHARED geometry module
# (kernel/lib/geom.c and fixed.c, compiled a second time for ring 3 --
# the same code the kernel's gfx_draw_line()/gfx_draw_ellipse() use).
GFXDEMO_OBJS = $(BUILD)/userland/crt0.o \
               $(BUILD)/userland/gfxdemo.o \
               $(BUILD)/userland/uui.o \
               $(BUILD)/userland/uwidgets.o \
               $(BUILD)/userland/ugfx.o \
               $(BUILD)/userland/shared/geom.o \
               $(BUILD)/userland/shared/fixed.o \
               $(BUILD)/userland/sys.o \
               $(BUILD)/userland/stack_chk.o

userland/gfxdemo.elf: $(GFXDEMO_OBJS) userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(GFXDEMO_OBJS)

$(KERNEL): $(ASM_OBJECTS) $(C_OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ASM_OBJECTS) $(C_OBJECTS)

# Pulls in every .d file -MMD/-MP generated alongside its .o (same
# directory, same basename, e.g. build/apps/notepad.d next to
# build/apps/notepad.o) -- each one is a make fragment listing that .o's
# full header dependency chain, so changing a shared header like
# widgets.h or gui_apps.h now correctly rebuilds every .o that includes
# it, not just the ones whose own .c file changed. This is what used to
# require "always make clean && make all before testing a GUI change"
# (see CLAUDE.md) -- a stale .o compiled against an old struct layout
# could silently sit next to freshly-rebuilt ones. Wrapped in `wildcard`
# so this is a no-op (no .d files exist yet) on a completely clean
# checkout, and `-include` (not `include`) so a missing/deleted .d file
# is silently ignored rather than a hard error -- both matter for
# `make clean` followed immediately by `make all` to still work.
-include $(wildcard $(BUILD)/core/*.d $(BUILD)/drivers/*.d $(BUILD)/apps/*.d $(BUILD)/apps/wm/*.d $(BUILD)/apps/ui/*.d $(BUILD)/userland/*.d)

# Only created if it doesn't already exist -- see DISK_IMG's comment
# above for why this must never overwrite an existing image.
$(DISK_IMG):
	truncate -s 9G $@

# Every /bin binary this Makefile seeds onto $(DISK_IMG) -- the ELF
# build product on the left, the /bin name it's seeded as on the right.
# Add a row here (and nowhere else) to get a new binary onto disk at
# build time; no grub.cfg/iso recipe changes needed anymore, unlike
# when these were GRUB modules (see docs/decisions.md).

# Seeds $(DISK_IMG) with every SEED_BINARIES entry, plus the /etc/kbs/*
# keyboard-layout data files, via tools/seed_disk.py (see
# docs/decisions.md) -- this is what gets each binary onto disk now,
# replacing both the old boot-time BIN_BOOTSTRAP/GRUB-module install
# (kernel/core/kernel.c, lspci only) and the older still per-binary
# GRUB-module test harnesses (kernel/core/*_test.c, all the rest --
# see docs/decisions.md for the full migration). Runs every `make iso`,
# not just when $(DISK_IMG) is first created: `sync`'s content-hash
# compare makes every call after the first a fast no-op unless
# something actually changed, so this always leaves disk.img current
# with whatever was just built. PHONY (not a real file target)
# specifically so it re-runs every time rather than being skipped once
# its prerequisites look up to date -- the "up to date" check IS the
# content-hash compare inside sync itself, not something make's own
# mtime logic should try to shortcut.
#
# The /etc/kbs/us and /etc/kbs/se files are regenerated here (not
# hand-maintained) via tools/gen_kbs.py -- see that script's top
# comment. It needs `xkbcli` (Debian/Ubuntu: `apt-get install
# libxkbcommon-tools`); if that's missing, this prints a warning and
# skips regenerating them rather than failing the build -- whatever's
# already in $(SEED_DIR)/sync/etc/kbs (nothing, on a machine that's
# never had xkbcli, including most CI runners) just doesn't get synced
# onto disk.img, and the kernel's own compiled-in US fallback
# (keyboard_layout.c) keeps the keyboard working regardless. Delete
# $(SEED_DIR)/sync/etc/kbs and re-run `make iso` to force a fresh
# regenerate once xkbcli is installed.
seed: $(DISK_IMG) $(USERLAND_ELVES)
	mkdir -p $(SEED_DIR)/sync/bin $(SEED_DIR)/sync/tests
	$(foreach pair,$(SEED_PROGRAMS),cp $(word 1,$(subst :, ,$(pair))) $(SEED_DIR)/sync/bin/$(word 2,$(subst :, ,$(pair)));)
	$(foreach pair,$(SEED_TESTS),cp $(word 1,$(subst :, ,$(pair))) $(SEED_DIR)/sync/tests/$(word 2,$(subst :, ,$(pair)));)
	# The PCI ID database, staged the same way the ELFs above are, and
	# for the same reason: $(SEED_DIR)/sync is a build-staging tree that
	# `make clean` deletes wholesale and .gitignore excludes, so nothing
	# hand-authored can live there. The tracked master copy is
	# data/pci.ids -- see that directory's entry in README.md, and
	# LICENSE's "Third-party data" section for what governs it.
	mkdir -p $(SEED_DIR)/sync/usr/share/hwdata
	cp $(PCI_IDS) $(SEED_DIR)/sync/usr/share/hwdata/pci.ids
	@if command -v xkbcli >/dev/null 2>&1; then \
		python3 tools/gen_kbs.py us --write; \
		python3 tools/gen_kbs.py se --write; \
	else \
		echo "seed: xkbcli not found -- skipping /etc/kbs regeneration (apt-get install libxkbcommon-tools to enable; kernel falls back to compiled-in US regardless)"; \
	fi
	python3 tools/seed_disk.py $(DISK_IMG) $(SEED_DIR)

iso: version $(KERNEL) $(USERLAND_ELVES) seed
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/kernel.bin
	sed 's/@GRUB_TIMEOUT@/$(GRUB_TIMEOUT)/' grub.cfg > iso/boot/grub/grub.cfg
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
		echo "make: grub-mkrescue not found (looked for grub-mkrescue and grub2-mkrescue)."; \
		echo "      Install GRUB's rescue tools + xorriso + mtools -- see README.md's"; \
		echo "      dependency table for your distribution's package names."; \
		exit 1; \
	fi
	$(GRUB_MKRESCUE) -o $(ISO) iso

# -vga std: explicit (matches QEMU's own default, but pinned here so the
#   higher 1280x720 mode boot.asm requests isn't at the mercy of a
#   per-host/per-distro QEMU default changing underneath us).
# -display gtk,zoom-to-fit=off,grab-mod=rctrl: shows the framebuffer at
#   its native resolution, one real pixel per emulated pixel --
#   zoom-to-fit would let GTK scale the window and blur/blend the
#   (deliberately sharp, nearest-neighbor-scaled) font. grab-mod=rctrl
#   sets the key that captures/releases the mouse once it's grabbed
#   (right Ctrl) -- only meaningful now that -device usb-mouse (below)
#   makes the pointer relative and therefore actually need grabbing.
# -usb -device usb-mouse: without this, recent QEMU defaults to an
#   absolute USB tablet pointer for the GTK display, which never needs
#   grabbing but also means host and guest cursor positions are just
#   two independent mappings onto the same window -- easy for the
#   cursor to wander outside the window edges and lose sync. A plain
#   relative mouse also matches what kernel/drivers/mouse.c actually
#   implements: it's a PS/2-protocol relative-mouse driver only, with
#   no concept of an absolute/tablet pointer, so this is the more
#   correct device for this guest regardless of the ergonomics.
# -drive ...,if=ide: attaches disk.img as the primary IDE bus's master
# drive -- exactly what kernel/drivers/ata.c's fixed 0x1F0 ports talk to
# (see ata.h for why legacy PIO IDE, not AHCI/virtio). This ends up on a
# DIFFERENT bus than -cdrom's ATAPI drive (QEMU's default piix3-ide
# puts -cdrom on the secondary bus), so ata.c's IDENTIFY never sees the
# boot CD and mistakes it for a plain disk.
# Boots with the GRUB menu visible (5s to choose), for when you want to
# see it. Rebuilds the ISO because the timeout is baked into grub.cfg at
# ISO build time -- so switching between `make run` and `make run-menu`
# re-runs grub-mkrescue, which is a couple of seconds.
run-menu:
	@$(MAKE) --no-print-directory GRUB_TIMEOUT=5 iso
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga std -display sdl,grab-mod=rctrl -m 256

run: iso $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga std -display sdl,grab-mod=rctrl -m 256

# Same as `run`, but on the VMware SVGA II adapter, which has a HARDWARE
# MOUSE CURSOR -- kernel/drivers/vmsvga.c detects it, takes the display
# over from GRUB's VBE mode and hands the cursor to the adapter, so the
# WM stops drawing one. `-vga std` above has no cursor hardware at all
# (plain VGA's only cursor is the text-mode underline), which is why
# this is a separate target rather than the default.
#
# Not the default for two reasons: it's emulator-only (a real machine
# needs a real GPU driver), and a hardware cursor is composited by the
# display frontend rather than living in the framebuffer -- so QEMU's
# `screendump` does NOT capture it, and every screenshot-based test
# would stop seeing the pointer. Use `run` for anything you intend to
# screenshot; use this to see the cursor the adapter draws.
run-vmware: iso $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga vmware -display sdl,grab-mod=rctrl -m 256

# Same as `run`, but with KVM hardware virtualization instead of QEMU's
# TCG software emulation -- guest instructions run natively on the host
# CPU. Needs /dev/kvm to be readable (usually membership of the `kvm`
# group, or a world-accessible node); `make run` remains the portable
# default precisely because that isn't guaranteed anywhere.
#
# `-cpu host` is what makes the acceleration worth having: without it
# QEMU still masks the guest down to a conservative CPU model. Safe for
# this kernel, which reads no CPUID feature bits and enables nothing
# beyond long mode + NX (see boot.asm).
#
# Worth knowing before comparing numbers across the two: this is NOT a
# uniform speedup. Compute-bound guest code gets much faster, but every
# port-I/O instruction becomes a hardware VM exit costing on the order
# of a microsecond, where TCG services one in-process for tens of
# nanoseconds -- so the PIO disk path and other `inb`/`outb`-heavy loops
# can get SLOWER here. Any throughput figure recorded in CHANGELOG.md
# should say which of the two it came from; they aren't comparable.
run-kvm: iso $(DISK_IMG)
	qemu-system-x86_64 -enable-kvm -cpu host -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga std -display sdl,grab-mod=rctrl -m 256

# Same as `run`, plus a PulseAudio backend wired to the PC speaker
# (kernel/drivers/speaker.c's `beep`, Milestone 19 -- see
# docs/roadmap.md) -- QEMU needs an explicit -audiodev backend to play
# anything at all (no default audio backend since QEMU 5.x), and
# -machine pcspk-audiodev=<id> is what actually routes the emulated
# i8254 PC speaker's output to it; without both, `beep` still runs
# correctly (the PIT/port-0x61 programming completes fine) but is
# silent. `pa` (PulseAudio) is what this was confirmed working with --
# swap it for whatever backend your host actually has
# (`qemu-system-x86_64 -audiodev help` lists what's available, e.g.
# `alsa` on plain ALSA-only Linux, `coreaudio` on macOS) if PulseAudio
# isn't it. Kept as a separate target rather than folding into `run`
# itself since the right backend is host-specific, not something safe
# to assume by default.
run-audio: iso $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga std -display sdl,grab-mod=rctrl -m 256 -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

# -s: shorthand for -gdb tcp::1234 -- QEMU's own built-in GDB remote
#   stub, exposed on the standard GDB-over-QEMU port. Emulates the CPU
#   directly, so it can already do real breakpoints/single-stepping/
#   register+memory inspection on toy-os with ZERO kernel-side GDB
#   protocol code -- see docs/decisions.md for why an in-kernel stub
#   isn't needed. -g in CFLAGS/USERLAND_CFLAGS above is what makes this
#   actually useful (DWARF symbols -- function names/source lines, not
#   just raw addresses).
# -S: freeze the CPU at reset instead of booting immediately, so it
#   doesn't race past GRUB/kernel_main before a debugger attaches.
# In another terminal once this is running: `gdb build/kernel.bin -ex
#   "target remote localhost:1234"`, then `continue` (or `break
#   kernel_main` first if you want to stop right at kernel entry).
debug: iso $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga std -display sdl,grab-mod=rctrl -m 256 -s -S

run-nographic: iso $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -display none -m 256

# Runs the in-kernel test suite and turns it into an exit code: boots
# headless, drives `ktest` over the serial debug console, exits non-zero
# if anything failed. See tools/ktest_run.py and
# kernel/include/kernel/ktest.h.
test: iso
	@python3 tools/ktest_run.py

# Everything: clean build, ISO, boot smoke test, and the in-kernel test
# suite. What to run before delivering a change -- tools/preflight.sh
# does exactly this plus a `git status` summary.
verify:
	@bash tools/preflight.sh

clean:
	rm -rf $(BUILD) $(ISO) iso/boot/kernel.bin $(HELLO_ELF) $(EXIT_TEST_ELF) $(WRITE_TEST_ELF) $(WRITE_BAD_TEST_ELF) $(GUI_TEST_ELF) $(COUNTER_A_ELF) $(COUNTER_B_ELF) $(ECHO_ELF) $(WIN_TEST_ELF) $(FILE_TEST_ELF) $(NEWSYSCALLS_TEST_ELF) $(CRASH_TEST_ELF) $(SOCKET_TEST_ELF) $(LSPCI_ELF) $(LS_ELF) $(STACK_SMASH_TEST_ELF) $(NX_TEST_ELF) $(FPU_TEST_ELF) $(FPU_RACE_ELF) $(LSCPU_ELF) $(SEED_DIR)/sync
	# Deliberately NOT touching $(DISK_IMG) here -- see its comment above.
	# Use `make clean-disk` to explicitly wipe the persistent filesystem.

clean-disk:
	rm -f $(DISK_IMG)
