# grub-mkrescue is named grub2-mkrescue on Fedora/RHEL and openSUSE.
# Resolved here rather than documented as a "symlink it yourself" step,
# so `make iso` just works on those distributions.
# Seconds GRUB waits on its menu before booting the default entry. 0
# (the default) draws no menu at all and boots instantly, which is what
# every automated path wants -- `make run-menu` overrides it.
GRUB_TIMEOUT ?= 0

# Extra words appended to the kernel's GRUB command line, baked into the
# ISO at build time. Empty by default, so every automated path (the boot
# smoke test, ktest, CI, gui_regress) boots exactly as before.
#
#     make iso KCMDLINE="video=1920x1080"
#     make live-iso KCMDLINE="video=1600x900 nokaslr"
#
# This exists so a boot flag can be tried without pressing `e` in the
# GRUB menu and retyping it every boot. Every word the kernel looks for
# is listed in docs/boot-flags.md; nothing here validates them, because
# the kernel matches by substring and an unknown word is simply ignored.
KCMDLINE ?=

GRUB_MKRESCUE := $(shell command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null)

CC = gcc
LD = ld
AR = ar
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
         -Wall -Wextra -Wframe-larger-than=1024 -O2 -g -c $(KERNEL_INCLUDES) -Iapps -MMD -MP

# apps/ gets a looser frame budget than kernel/ on purpose. The tight
# 1024 above bounds what runs on a PER-PROCESS kernel stack -- i.e. what
# a syscall from ring 3 can reach -- and that is kernel/ only. apps/ (the
# shell, the WM) runs in the kernel CONTEXT, on its own stack, and three
# of its functions are legitimately 1.4-1.8 KiB of local buffers at the
# top of their call chains (gui_apps_load, shell_main, cmd_parttable).
# 2048 still catches the realistic mistake, which is a new multi-KiB
# buffer declared as a local.
APPS_CFLAGS = $(subst -Wframe-larger-than=1024,-Wframe-larger-than=2048,\
              $(subst -Ikernel/include/kernel,,$(CFLAGS)))

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

# userland programs (see the "Process isolation" section of README.md)
# -- plain freestanding binaries, no
# kernel-specific flags like -mcmodel=kernel needed since these run in
# ordinary ring-3 user space, not as part of the kernel image.
# -mcmodel=large IS needed though: these link at VMM_USER_BASE
# (512GiB), and the default code model can't reach a global (e.g. a
# string literal) from that address with a 32-bit relocation -- the
# linker fails with "relocation truncated to fit" without this.
# Same stack-canary flags as CFLAGS above, and the same reasoning --
# see that comment. Every userland ELF needs userland/rt/stack_chk.c's
# __stack_chk_guard/__stack_chk_fail linked in (see USERLAND_RT below)
# since GCC emits implicit references to both from any protected
# function in any userland .c file.
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
                   -Wall -Wextra -Wframe-larger-than=2048 -O2 -g -c $(API_INCLUDES) -Iuserland \
                   -ffunction-sections -fdata-sections -MMD -MP \
                   -fno-tree-loop-distribute-patterns
# -Wframe-larger-than for RING 3, which had none while the kernel side
# has had one since kernel stacks got guard pages. The reason is the
# same and the mechanism is weaker here: a ring-3 stack has ONE 4 KiB
# guard page below it, and a function whose frame exceeds that can write
# past the guard without ever touching it -- the Stack Clash shape
# (CVE-2017-1000364), which is why Linux widened its guard gap to 256
# pages in 4.11. A warning names the offending function; a wider guard
# would only hide it. 2048 rather than the kernel's 1024 because a
# ring-3 stack is 16 KiB where a kernel one is shared with an interrupt
# frame; raise the guard instead if this ever becomes the constraint.
# That last flag stops GCC rewriting a hand-written copy loop into a
# call to memcpy(). It matters only in userland, and only since
# userland/lib/string.c started providing a real memcpy: the rewrite
# applied to k_memcpy's own loop would make memcpy() call k_memcpy()
# call memcpy() forever. It would LINK -- every symbol resolves -- and
# blow the stack at runtime with no obvious cause. The kernel needs no
# such flag, because it defines no memcpy at all, so there the same
# rewrite is a loud undefined reference instead. -ffreestanding does
# not promise this on its own; Linux passes the same flag for the same
# reason. NOTE this is a CFLAGS change, which the .d files do not
# track -- `make clean` after touching it (see CLAUDE.md).
# --- userland source layout ------------------------------------------
#
# userland/ is split by ROLE, and the split is load-bearing rather than
# cosmetic -- the build derives what to build, and where to seed it,
# from which directory a file is in:
#
#   rt/     the C runtime every ring-3 program starts through
#           (crt0.asm, sys.c = libsys, stack_chk.c, link.ld)
#   ui/     the GUI toolkit: ugfx, utheme, utext and the widgets.
#           Mirrors apps/ui/, so a widget's kernel-side and ring-3
#           versions sit at the same relative path while both exist.
#   lib/    userland libraries that aren't UI (tosh, the shell the
#           ring-3 Terminal links against)
#   gui/    windowed applications, in one subdirectory per CLASS:
#             gui/system/ -> /bin/wm/system   (About; the desktop's own)
#             gui/apps/   -> /bin/wm/apps     (Calculator, Notepad, Terminal)
#             gui/demos/  -> /bin/wm/demos    (Shapes, UI Demo)
#           The subdirectory is the destination, exactly as the top-level
#           one is -- so a new app is still a .c file and nothing else,
#           and WHICH class it belongs to is stated by where it lives
#           rather than in a table somewhere.
#   bin/    command-line programs  -> seeded to /bin
#   tests/  single-mechanism diagnostics -> seeded to /tests
#
# LIBRARY directories (rt, ui, lib) produce objects only. PROGRAM
# directories (gui, bin, tests) produce one ELF per .c file, with no
# list to maintain -- which is the point. /bin vs /tests is a real
# distinction docs/filesystem-layout.md enforces through
# tools/check_layout.py, and it used to be restated here as two
# hand-written lists plus 29 FOO_ELF variables that could drift from
# them. Now the directory IS the statement.
#
# Note tests/ holds windowed diagnostics too (winclient, uiclient): the
# directories name a DESTINATION, and those two exercise the windowing
# protocol rather than being programs a user wants offered.
USERLAND_PROGRAM_DIRS = gui bin tests

# Every program source, and the ELF it builds to. A new program is a .c
# file in one of those directories and nothing else -- the same
# reasoning as C_SOURCES's recursive discovery below.
USERLAND_PROGRAMS = $(shell find $(addprefix userland/,$(USERLAND_PROGRAM_DIRS)) -name '*.c' 2>/dev/null | sort)
USERLAND_ELVES    = $(patsubst userland/%.c,$(BUILD)/userland/%.elf,$(USERLAND_PROGRAMS))

# The handful of programs whose on-disk name isn't their file name --
# three exceptions spelled once each, instead of a name column on every
# entry of a 29-line list:
#   terminal -> uterm      so it doesn't collide with the kernel-space
#                          Terminal in the Start menu while both exist
#   gfxdemo  -> shapes     the demo's user-facing name
#   echo     -> echo_test  a syscall exercise, not a real echo(1)
SEED_NAME_terminal = uterm
SEED_NAME_gfxdemo  = shapes
SEED_NAME_echo     = echo_test

# The on-disk name for one ELF path: its override if it has one, else
# its own basename.
seed_name = $(or $(SEED_NAME_$(basename $(notdir $(1)))),$(basename $(notdir $(1))))

# Seed directory for the writer tools' `sync` (reached through
# tools/seed_disk.py, which probes the image's format) -- see the
# `seed` target below and docs/decisions.md. Not committed as a
# generic directory: SEED_DIR/sync/bin/* are build-generated copies of
# each userland ELF, staged fresh by the `seed` target's own recipe
# every build, not tracked source files.
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
	@echo "  live-iso       Build toy-os-live.iso -- carries a filesystem image, boots with NO disk"
	@echo "  run-live       Boot that live ISO with no disk attached (implies live-iso)"
	@echo "  demo-iso       Build toy-os-demo.iso -- boots straight into a scripted tour"
	@echo "  run-demo       Boot that demo ISO (implies demo-iso)"
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
#
# `| version` is ORDER-ONLY and is what makes the generated headers
# (version.h, build_date.h) exist before anything includes them. It has
# to be here, on the compile rules, rather than on `all`: listing
# `version` first in `all:`s prerequisites orders nothing under `-j`,
# because make is free to build every prerequisite subtree at once. The
# failure is a `build_date.h: No such file or directory` on whichever
# object won the race -- apps/wm/desktop.c, the only file that includes
# it -- with gen_version.sh's own output appearing AFTER the error in
# the log. Order-only rather than a normal prerequisite because
# `version` is .PHONY and therefore always out of date: a normal one
# would rebuild every object on every build.
$(BUILD)/%.o: %.c | version
	@mkdir -p $(dir $@)
	$(CC) $(if $(filter apps/%,$<),$(APPS_CFLAGS),$(CFLAGS)) $< -o $@

$(BUILD)/%.o: %.asm | version
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

# Userland ELFs: two generic rules instead of three hand-written lines
# per binary (17 binaries = ~55 lines before this). A new program is now
# a .c file in userland/gui, userland/bin or userland/tests and nothing
# else -- the directory says both that it is a program and where it
# seeds to (see "userland source layout" above).
$(BUILD)/userland/%.o: userland/%.c | version
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
USERLAND_RT = $(BUILD)/userland/rt/crt0.o $(BUILD)/userland/rt/sys.o \
              $(BUILD)/userland/rt/stack_chk.o

# libuapp.a -- the toolkit, the userland libraries, and the sources
# shared with the kernel, as ONE archive every program links against.
#
# This replaced a per-binary object list (EXTRA_OBJS_<name>, which had
# itself replaced four hand-written FOO_OBJS blocks). The reason it is
# an archive rather than a longer list is what an archive DOES: the
# linker pulls in only the members a program actually references, so a
# program names nothing at all and still gets exactly what it uses.
# Adding a widget, or a whole new app, needs no Makefile edit.
#
# It works because of -ffunction-sections -fdata-sections above plus
# --gc-sections below: archive member granularity alone would still
# link the whole of (say) uwidgets.o for one checkbox, which is exactly
# what this removes. userland/rt/link.ld's `.text.*` wildcards are
# load-bearing for the same reason -- see the comment there.
#
# `shared/` is in here too (see the shared-source rule below), which is
# why the ring-3 Calculator no longer has to name calc_engine, string
# and knum: it references calc_* and the linker finds it.
LIBUAPP_SRCS = $(shell find userland/ui userland/lib -name '*.c' 2>/dev/null | sort)
LIBUAPP_OBJS = $(patsubst userland/%.c,$(BUILD)/userland/%.o,$(LIBUAPP_SRCS)) \
               $(BUILD)/userland/shared/geom.o \
               $(BUILD)/userland/shared/rubberband.o \
               $(BUILD)/userland/shared/icon_grid.o \
               $(BUILD)/userland/shared/etc_config.o \
               $(BUILD)/userland/shared/fixed.o \
               $(BUILD)/userland/shared/calc_engine.o \
               $(BUILD)/userland/shared/string.o \
               $(BUILD)/userland/shared/knum.o \
               $(BUILD)/userland/shared/kfmt.o \
               $(BUILD)/userland/shared/heap_core.o
LIBUAPP      = $(BUILD)/userland/libuapp.a

# The `rm -f` is load-bearing: `ar rcs` UPDATES an existing archive,
# adding and replacing members but never removing one whose source file
# has gone. Splitting uwidgets.c into per-widget files left uwidgets.o
# inside the archive, and it stayed there -- the link kept succeeding,
# because a linker pulls the first member that satisfies a symbol and
# only errors when two members it ALREADY pulled collide. So the build
# was quietly free to link the deleted file's code instead of its
# replacement, and said nothing until the two versions finally differed.
# Building the archive from scratch each time makes it a function of
# $(LIBUAPP_OBJS) rather than of every object that has ever existed.
$(LIBUAPP): $(LIBUAPP_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

# Kept as the escape hatch for an object that must be linked
# unconditionally rather than pulled from the archive on demand. Empty
# today for the single-file programs, and an empty list is the good
# outcome for those -- see $(LIBUAPP) above.
EXTRA_OBJS_uiclient   =
EXTRA_OBJS_calculator =
EXTRA_OBJS_notepad    =
EXTRA_OBJS_terminal   =
EXTRA_OBJS_gfxdemo    =

# ...and the one program that will genuinely need it: the ring-3 window
# manager (M41 stage 4b). `userland/wm/` is the port of `apps/wm/` and
# is deliberately NOT one of USERLAND_PROGRAM_DIRS -- those turn every
# .c into its own ELF, which is right for a program and wrong for the
# fourteen translation units of one.
#
# Listed rather than wildcarded on purpose: a stray .c dropped into
# userland/wm/ should fail to link with an undefined symbol, not get
# silently absorbed into the desktop.
EXTRA_OBJS_toywm      = wm/wm wm/wm_rawin wm/wm_render wm/wm_input wm/wm_client \
                        wm/wm_debug wm/wm_tray wm/wm_watchdog \
                        wm/desktop wm/start_menu wm/context_menu \
                        wm/confirm_dialog wm/file_picker wm/cursor_theme \
                        wm/gui_apps wm/wm_log wm/wm_fs wm/wm_conf

# --- the ring-3 WM is BUILT ON DEMAND, not by `make all` ---------------
#
# `make toywm`, and nothing else reaches it. The port is mid-flight
# (stage 4b): some of its call sites still name kernel functions that
# ring 3 has no path to, so the tree does not compile yet, and wiring an
# incomplete program into the default build would turn `make all`,
# `preflight.sh` and CI red for the duration of a migration every earlier
# stage was shaped to keep green.
#
# This is why main() lives in `userland/wm/` rather than in
# `userland/gui/system/`: that directory is auto-discovered, so a file
# there would be built by `all` whether or not it was ready.
#
# **Delete this target and move main.c into userland/gui/system/ the
# moment the port compiles** -- an on-demand target is a thing nobody
# runs, and a build nobody runs is a build that rots. Stage 4c does that
# and deletes apps/wm/ with it.
.PHONY: toywm
toywm: $(BUILD)/userland/wm/main.elf

# The extras for one binary, as real object paths.
uextra = $(patsubst %,$(BUILD)/userland/%.o,$(EXTRA_OBJS_$(notdir $(1))))

# .SECONDEXPANSION lets the prerequisite list reference the stem: `$$*`
# survives make's first expansion (when the rule is read, and the stem
# isn't known yet) and is expanded a second time per target, once it is.
# Without it there is no way for one pattern rule to depend on a
# per-target variable, which is the whole point here.
.SECONDEXPANSION:

# --gc-sections drops every section nothing reaches, which is what
# makes linking against one archive cheap: `hello` references nothing in
# libuapp.a and gains nothing from it. The archive goes LAST -- a
# linker resolves archive members against the undefined symbols it has
# accumulated so far, so an archive placed before its callers
# contributes nothing and the link fails with undefined references.
$(BUILD)/userland/%.elf: $(BUILD)/userland/%.o $(USERLAND_RT) userland/rt/link.ld $(LIBUAPP) $$(call uextra,$$*)
	$(LD) -n --gc-sections -T userland/rt/link.ld -nostdlib -o $@ $(BUILD)/userland/rt/crt0.o $< $(call uextra,$*) $(BUILD)/userland/rt/sys.o $(BUILD)/userland/rt/stack_chk.o $(LIBUAPP)

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
$(BUILD)/userland/shared/%.o: kernel/lib/%.c | version
	@mkdir -p $(dir $@)
	$(CC) $(USERLAND_CFLAGS) -Iapps $< -o $@

$(BUILD)/userland/shared/%.o: apps/%.c | version
	@mkdir -p $(dir $@)
	$(CC) $(USERLAND_CFLAGS) -Iapps $< -o $@

# calculator.c is the one ordinary userland program that includes an
# apps/ header (calc_engine.h). Scoped to this object with a
# target-specific variable rather than added to the pattern rule above,
# so no OTHER userland program gains the ability to reach into apps/.
$(BUILD)/userland/gui/apps/calculator.o: USERLAND_CFLAGS += -Iapps

# The kernel is linked TWICE, and the reason is kernel ASLR.
#
# Pass 1 links with --emit-relocs (-q), which keeps the relocations in
# the output. tools/genrelocs.py turns those into a table of every
# ABSOLUTE reference in the image, and pass 2 links that table in, so
# the kernel carries the list of words it must patch to run from a
# different address (kernel/arch/x86_64/reloc.c).
#
# This is not the usual two-pass chicken-and-egg, because linker.ld
# places .krelocs AFTER every section that can contain a fixup: adding
# the table cannot move any address the table records, so pass 1's
# addresses stay correct in pass 2. The --verify step re-derives the
# table from the FINAL image and fails the build if the two disagree,
# which is the only cheap way to notice that a section moved -- the
# expensive way is a kernel that does not boot.
KRELOCS_C = $(BUILD)/krelocs.c
KRELOCS_O = $(BUILD)/krelocs.o

# The panic's symbol table, generated the same way and for the same
# reason: linker.ld places .ksyms after every address it records, so
# pass 1's addresses stay correct in pass 2. See tools/gen_syms.py.
KSYMS_C = $(BUILD)/ksyms.c
KSYMS_O = $(BUILD)/ksyms.o

$(BUILD)/kernel.pass1.elf: $(ASM_OBJECTS) $(C_OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -q -o $@ $(ASM_OBJECTS) $(C_OBJECTS)

$(KRELOCS_C): $(BUILD)/kernel.pass1.elf tools/genrelocs.py
	python3 tools/genrelocs.py $< --out-c $@

$(KRELOCS_O): $(KRELOCS_C)
	$(CC) $(CFLAGS) -c $< -o $@

$(KSYMS_C): $(BUILD)/kernel.pass1.elf tools/gen_syms.py
	python3 tools/gen_syms.py $< --out-c $@

$(KSYMS_O): $(KSYMS_C)
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: $(KSYMS_C)

# $(KRELOCS_C) is named here as well as via $(KRELOCS_O) because the
# recipe below READS it. Without that, make classifies it as an
# intermediate file -- generated only on the way to the .o -- and
# deletes it once the .o is built, so the next build's --verify has
# nothing to read and fails with a FileNotFoundError on a path that
# looks obviously correct. .PRECIOUS keeps it for the same reason.
.PRECIOUS: $(KRELOCS_C)

$(KERNEL): $(ASM_OBJECTS) $(C_OBJECTS) $(KRELOCS_O) $(KRELOCS_C) $(KSYMS_O) $(KSYMS_C) linker.ld
	$(LD) $(LDFLAGS) -q -o $(BUILD)/kernel.pass2.elf $(ASM_OBJECTS) $(C_OBJECTS) $(KRELOCS_O) $(KSYMS_O)
	@python3 tools/genrelocs.py $(BUILD)/kernel.pass2.elf --verify $(KRELOCS_C)
	@python3 tools/gen_syms.py $(BUILD)/kernel.pass2.elf --verify $(KSYMS_C)
	objcopy --remove-section='.rela.*' $(BUILD)/kernel.pass2.elf $@

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
#
# RECURSIVE, via `find`, and deliberately not a hand-written list of
# directories: this line used to name six of them
# ($(BUILD)/core/*.d $(BUILD)/drivers/*.d ...), which stopped matching
# anything under kernel/ the day source discovery went recursive and
# objects moved from build/core/ to build/kernel/core/. Nothing failed
# -- make simply had no dependency information for 88 of the 161 .d
# files, so `touch kernel/include/kernel/process.h && make all` rebuilt
# NOTHING and left every kernel object stale against the new header.
# That is precisely the bug this line exists to prevent (see the
# paragraph above), silently reintroduced by a directory move. A
# `find` can't drift that way. tools/check_deps.py is the guard that
# proves it, and runs in preflight.sh and CI.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

# Only created if it doesn't already exist -- see DISK_IMG's comment
# above for why this must never overwrite an existing image.
$(DISK_IMG):
	truncate -s 9G $@

# Every /bin binary this Makefile seeds onto $(DISK_IMG) -- the ELF
# build product on the left, the /bin name it's seeded as on the right.
# Add a row here (and nowhere else) to get a new binary onto disk at
# build time; no grub.cfg/iso recipe changes needed anymore, unlike
# when these were GRUB modules (see docs/decisions.md).

# Seeds $(DISK_IMG) with every userland ELF, plus the /etc/kbs/*
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
	# Destination comes from the SOURCE DIRECTORY, not from a list:
	# build/userland/{gui,bin}/x.elf -> /bin/x, build/userland/tests/x.elf
	# -> /tests/x, with $(call seed_name,...) applying the three renames.
	# See "userland source layout" above for why this is derived.
	# gui/<class>/x.elf -> /bin/wm/<class>/x, preserving the class
	# directory. `dir` of the path relative to gui/ IS the class, so
	# adding gui/games/ later needs no edit here.
	mkdir -p $(SEED_DIR)/sync/bin/wm
	$(foreach e,$(filter $(BUILD)/userland/gui/%,$(USERLAND_ELVES)),\
	    mkdir -p $(SEED_DIR)/sync/bin/wm/$(patsubst %/,%,$(subst $(BUILD)/userland/gui/,,$(dir $(e)))) && \
	    cp $(e) $(SEED_DIR)/sync/bin/wm/$(patsubst %/,%,$(subst $(BUILD)/userland/gui/,,$(dir $(e))))/$(call seed_name,$(e));)
	$(foreach e,$(filter $(BUILD)/userland/bin/%,$(USERLAND_ELVES)),cp $(e) $(SEED_DIR)/sync/bin/$(call seed_name,$(e));)
	$(foreach e,$(filter $(BUILD)/userland/tests/%,$(USERLAND_ELVES)),cp $(e) $(SEED_DIR)/sync/tests/$(call seed_name,$(e));)
	# The PCI ID database, staged the same way the ELFs above are, and
	# for the same reason: $(SEED_DIR)/sync is a build-staging tree that
	# `make clean` deletes wholesale and .gitignore excludes, so nothing
	# hand-authored can live there. The tracked master copy is
	# data/pci.ids -- see that directory's entry in README.md, and
	# LICENSE's "Third-party data" section for what governs it.
	mkdir -p $(SEED_DIR)/sync/usr/share/hwdata
	cp $(PCI_IDS) $(SEED_DIR)/sync/usr/share/hwdata/pci.ids
	# Desktop entries -- what the Start menu and the desktop icons are
	# built FROM (see docs/filesystem-layout.md). Hand-authored and
	# tracked under data/wm/, staged here for the same reason pci.ids is:
	# sync/ is a build-staging tree `make clean` deletes wholesale.
	mkdir -p $(SEED_DIR)/sync/usr/wm/desktop $(SEED_DIR)/sync/usr/wm/startup
	cp data/wm/desktop/*.desktop $(SEED_DIR)/sync/usr/wm/desktop/
	# Cursor themes -- one directory per theme, one file per shape (see
	# docs/filesystem-layout.md). Generated by tools/gen_cursors.py into
	# data/cursors/ and staged here for exactly the reason above: a file
	# written straight into sync/ is gitignored and deleted by `make
	# clean`, so it works on the machine that made it and exists nowhere
	# else. That happened -- the themes shipped absent and the desktop
	# silently fell back to its built-in shapes.
	@for t in data/cursors/*/; do \
	    mkdir -p $(SEED_DIR)/sync/usr/share/cursors/$$(basename $$t); \
	    cp $$t* $(SEED_DIR)/sync/usr/share/cursors/$$(basename $$t)/; \
	done
	# The scripted tour. Seeded always -- it is inert unless `demo` is on
	# the kernel command line, and having it present means a live image
	# can be edited into a demo without a rebuild.
	cp data/wm/demo.script $(SEED_DIR)/sync/usr/wm/demo.script
	@if [ -n "$$(ls -A data/wm/startup 2>/dev/null)" ]; then \
	    cp data/wm/startup/* $(SEED_DIR)/sync/usr/wm/startup/; fi
	@if command -v xkbcli >/dev/null 2>&1; then \
		python3 tools/gen_kbs.py us --write; \
		python3 tools/gen_kbs.py se --write; \
	else \
		echo "seed: xkbcli not found -- skipping /etc/kbs regeneration (apt-get install libxkbcommon-tools to enable; kernel falls back to compiled-in US regardless)"; \
	fi
	python3 tools/seed_disk.py $(DISK_IMG) $(SEED_DIR)
	# A stamp saying the seed step RAN, which disk.img's own mtime
	# cannot: seeding is content-hash based, so a rebuild producing
	# byte-identical ELFs correctly rewrites nothing and leaves the
	# image untouched. tools/iso_guard.py compares the built ELFs
	# against this rather than against disk.img, so it does not cry
	# wolf on a no-op rebuild -- a guard that false-alarms is a guard
	# people switch off.
	@touch $(BUILD)/.seeded

# The LIVE ISO is a SEPARATE ARTIFACT, and that is the point.
#
# It was briefly part of `make iso`, which made every ISO 162 MiB and
# every boot pay for GRUB reading a 129 MiB module off an emulated
# CD-ROM before the kernel got a single instruction. Locally that took
# the boot smoke test from ~2s to 7s; on CI's slower runner it blew the
# 12s timeout and turned the build red. Measured, not guessed.
#
# So the live image ships the way a distribution ships one: as its own
# image you build when you want it (`make live-iso` -> toy-os-live.iso),
# leaving the ordinary ISO exactly as fast as it was. The two differ
# only in the module and the menu.
#
# The 129 MiB is TFS3's floor, not a choice -- see $(LIVE_IMG_MB) below
# -- and shrinking it is a roadmap item. Once it is small, this can
# fold back into the default ISO.
#
# The LIVE IMAGE: a small TFS3 filesystem carried inside the ISO and
# handed to the kernel by GRUB as a module, so the OS can boot with no
# disk at all (docs/live-cd-design.md). Built from the same seed tree
# disk.img is, by the same writer tool -- one format, one builder, and
# the live path is the disk path with a different device underneath.
#
# Sized for the seed tree plus room to work in -- which is possible at
# all only because TFS3's LAST block group may now be partial, the rule
# ext2/3/4 have always had. It was 129 MiB when a whole 128 MiB group
# was the floor, and that made the live ISO 162 MiB and cost GRUB seven
# seconds reading the module off an emulated CD-ROM before the kernel
# ran a single instruction.
LIVE_IMG    = $(BUILD)/live.img
LIVE_IMG_MB = 24

$(LIVE_IMG): $(USERLAND_ELVES) seed
	@mkdir -p $(BUILD)
	rm -f $(LIVE_IMG)
	python3 tools/tfs3_writer.py format $(LIVE_IMG) --size $$(( $(LIVE_IMG_MB) * 1024 * 1024 ))
	python3 tools/tfs3_writer.py sync $(LIVE_IMG) $(SEED_DIR)
	python3 tools/tfs3_writer.py trim $(LIVE_IMG)

iso: version $(KERNEL) $(USERLAND_ELVES) seed
	mkdir -p iso/boot/grub
	rm -f iso/boot/live.img
	cp $(KERNEL) iso/boot/kernel.bin
	sed -e 's/@GRUB_TIMEOUT@/$(GRUB_TIMEOUT)/' -e 's|@KCMDLINE@|$(KCMDLINE)|' grub.cfg > iso/boot/grub/grub.cfg
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
		echo "make: grub-mkrescue not found (looked for grub-mkrescue and grub2-mkrescue)."; \
		echo "      Install GRUB's rescue tools + xorriso + mtools -- see README.md's"; \
		echo "      dependency table for your distribution's package names."; \
		exit 1; \
	fi
	$(GRUB_MKRESCUE) -o $(ISO) iso

# The live ISO: same kernel, plus the filesystem image as a module and a
# menu that can force it. Built into its own staging tree so it cannot
# leave a stale module behind in the ordinary one.
LIVE_ISO = toy-os-live.iso

live-iso: version $(KERNEL) $(USERLAND_ELVES) seed $(LIVE_IMG)
	rm -rf iso-live
	mkdir -p iso-live/boot/grub
	cp $(KERNEL) iso-live/boot/kernel.bin
	cp $(LIVE_IMG) iso-live/boot/live.img
	sed -e 's/@GRUB_TIMEOUT@/$(GRUB_TIMEOUT)/' -e 's|@KCMDLINE@|$(KCMDLINE)|' grub-live.cfg > iso-live/boot/grub/grub.cfg
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
		echo "make: grub-mkrescue not found -- see README.md's dependency table."; \
		exit 1; \
	fi
	$(GRUB_MKRESCUE) -o $(LIVE_ISO) iso-live
	@echo "live-iso: $(LIVE_ISO) -- boots with no disk; see docs/live-cd-design.md"

# The DEMO ISO: the live ISO plus `demo` on the kernel command line, so
# it boots straight into the scripted tour with nobody touching a key.
# For showing the system on real hardware -- write it to a USB stick and
# boot it.
#
# Deliberately its own target and its own grub.cfg rather than a runtime
# toggle: a demo that can start itself by accident is a demo that starts
# during something else.
DEMO_ISO = toy-os-demo.iso

demo-iso: version $(KERNEL) $(USERLAND_ELVES) seed $(LIVE_IMG)
	rm -rf iso-demo
	mkdir -p iso-demo/boot/grub
	cp $(KERNEL) iso-demo/boot/kernel.bin
	cp $(LIVE_IMG) iso-demo/boot/live.img
	sed -e 's/@GRUB_TIMEOUT@/$(GRUB_TIMEOUT)/' -e 's|@KCMDLINE@|$(KCMDLINE)|' grub-demo.cfg > iso-demo/boot/grub/grub.cfg
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
		echo "make: grub-mkrescue not found -- see README.md's dependency table."; \
		exit 1; \
	fi
	$(GRUB_MKRESCUE) -o $(DEMO_ISO) iso-demo
	@echo "demo-iso: $(DEMO_ISO) -- boots the tour with no input; see data/wm/demo.script"

run-demo: demo-iso
	qemu-system-x86_64 -cdrom $(DEMO_ISO) -serial stdio -vga std -display sdl,grab-mod=rctrl -m 2048

# Boot the live ISO the way a user would: NO -drive at all. That is the
# whole point -- pointing it at disk.img would let the ordinary disk path
# run and prove nothing about the live one.
run-live: live-iso
	qemu-system-x86_64 -cdrom $(LIVE_ISO) -serial stdio -vga std -display sdl,grab-mod=rctrl -m 2048

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
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga std -display sdl,grab-mod=rctrl -m 2048

run: iso $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga std -display sdl,grab-mod=rctrl -m 2048

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
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga vmware -display sdl,grab-mod=rctrl -m 2048

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
# can get SLOWER here. Any throughput figure recorded in the git history
# should say which of the two it came from; they aren't comparable.
run-kvm: iso $(DISK_IMG)
	qemu-system-x86_64 -enable-kvm -cpu host -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga std -display sdl,grab-mod=rctrl -m 2048

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
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga std -display sdl,grab-mod=rctrl -m 2048 -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

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
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -vga std -display sdl,grab-mod=rctrl -m 2048 -s -S

run-nographic: iso $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap -serial stdio -display none -m 2048

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
	# One line, because userland ELFs land in $(BUILD) now like every
	# other build product. This used to name 21 $(FOO_ELF) variables by
	# hand -- they built into the source tree next to their .c files and
	# needed a .gitignore entry to stay out of the repo.
	rm -rf $(BUILD) $(ISO) $(LIVE_ISO) $(DEMO_ISO) iso/boot/kernel.bin iso/boot/live.img iso-live iso-demo $(SEED_DIR)/sync
	# Deliberately NOT touching $(DISK_IMG) here -- see its comment above.
	# Use `make clean-disk` to explicitly wipe the persistent filesystem.

clean-disk:
	rm -f $(DISK_IMG)
