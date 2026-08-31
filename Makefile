# grub-mkrescue is named grub2-mkrescue on Fedora/RHEL and openSUSE.
# Resolved here rather than documented as a "symlink it yourself" step,
# so `make iso` just works on those distributions.
# Seconds GRUB waits on its menu before booting the default entry. 0
# (the default) draws no menu at all and boots instantly, which is what
# every automated path wants -- `make run MENU=1` asks for the menu, and
# GRUB_TIMEOUT= still overrides both.
GRUB_TIMEOUT ?= $(if $(MENU),5,0)

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

# ccache in front of the compiler when it is installed, and plain gcc
# when it is not -- so a checkout without it builds identically.
#
# It earns its keep on the GATE, not on an ordinary edit: preflight.sh
# and `make verify` both start with `make clean`, so every run is a full
# rebuild of a tree that mostly did not change. Nothing else here is
# affected -- ccache hashes the preprocessed source and the flags, so a
# CFLAGS change correctly misses the cache (which matters, because the
# .d files do NOT track flags; see CLAUDE.md).
CCACHE := $(shell command -v ccache 2>/dev/null)
CC = $(CCACHE) gcc
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
# The C library's PUBLIC headers, ring 3 only, and FIRST on the include
# path ahead of API_INCLUDES -- which it has to be, because both
# directories contain a `string.h` and an app asking for <string.h>
# means the C library's. The toolkit's is still reachable as
# <kstring.h>; see that file for why it exists.
#
# It is a separate variable rather than another -I on USERLAND_CFLAGS so
# that the shared-source rule can take it back OUT -- see the
# build/userland/shared/ rules near the bottom of this file.
LIBC_INCLUDES   = -Iuserland/include
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
# -fpie -mcmodel=small, NOT -mcmodel=large: PIE code reaches every
# global RIP-relatively, so the 2 GiB constraint is on the image's SPAN,
# not its placement -- the binaries still link at VMM_USER_BASE
# (512GiB) and nothing in the address map moved (dynlink Stage 1).
# The one thing that could not survive the switch was the *ABS* TLS
# geometry symbols; they are data now (userland/rt/link.ld's QUADs).
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
USERLAND_CFLAGS = -std=gnu11 -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fpie \
                   -mno-direct-extern-access \
                   -mno-red-zone -mcmodel=small -ftls-model=local-exec \
                   -Wall -Wextra -Wframe-larger-than=2048 -O2 -g -c $(LIBC_INCLUDES) $(API_INCLUDES) -Iuserland \
                   -ffunction-sections -fdata-sections -MMD -MP \
                   -fno-tree-loop-distribute-patterns
# -ftls-model=local-exec: a `__thread` variable is reached as a fixed
# offset from %fs and nothing else. Right for EXECUTABLES; objects
# destined for a shared library (dynlink Stage 3) compile separately
# with initial-exec, because local-exec assumes the block the linker
# itself laid out. See userland/rt/tls.c.
#
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
#           (crt0.asm, sys.c = libsys, stack_chk.c, sigtramp.c,
#           link.ld)
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
#   gui/apps/terminal -> uterm      so it doesn't collide with the
#                                   kernel-space Terminal in the Start
#                                   menu while both exist
#   gui/demos/gfxdemo -> shapes     the demo's user-facing name
#   tests/echo        -> echo_test  a syscall exercise, not echo(1)
#
# KEYED BY PATH UNDER userland/, NOT BY BASENAME, and that is not
# decoration. It was `SEED_NAME_echo` until /bin/echo was written --
# whereupon the rename meant for the syscall exercise in tests/ captured
# the real echo(1) in bin/ as well, and seeded it to /bin/echo_test.
# Nothing failed: the build was green, `ls /bin` looked plausible, and
# the only symptom was `echo` reporting "Unknown command" at a prompt
# where every other new program worked. A rename table addressed less
# specifically than the thing it renames will eventually rename
# something else.
SEED_NAME_gui/apps/terminal = uterm
SEED_NAME_gui/demos/gfxdemo = shapes
SEED_NAME_tests/echo        = echo_test

# The on-disk name for one ELF path: its override if it has one, else
# its own basename. The override is looked up by the ELF's path under
# build/userland/ ("tests/echo"), so a name is only ever renamed where
# it was meant to be.
seed_name = $(or $(SEED_NAME_$(basename $(subst $(BUILD)/userland/,,$(1)))),$(basename $(notdir $(1))))

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

# The USB ID database, on exactly the same terms as pci.ids above --
# same directory, same seed step, same path a Linux distribution uses,
# and the same licence choice. Read at runtime by /bin/lsusb to turn
# 0627:0001 into "Adomax Technology Co., Ltd". NOT MIT -- upstream
# (https://www.linux-usb.org/usb-ids.html) offers it under either GPL v2
# or later or the 3-clause BSD Licence, and this repo redistributes it
# under the BSD option, see LICENSE. Refreshing it is a deliberate
# commit: download a new copy from https://www.linux-usb.org/usb.ids
# over data/usb.ids.
USB_IDS = data/usb.ids

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

.PHONY: all clean clean-disk iso run debug help version seed test verify

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

all: version $(KERNEL) $(USERLAND_ELVES) $(LDSO) $(DYNLIBS) $(LIBC_SO)

help:
	@echo "toy-os -- available targets:"
	@echo "  all            Build kernel.bin and the userland test ELFs (default)"
	@echo "  iso            Build toy-os.iso, a bootable GRUB ISO (implies all)"
	@echo "  run            Boot toy-os in QEMU with an SDL window (the DISK, once"
	@echo "                 it carries GRUB; BOOT=cd for the ISO -- implies iso)"
	@echo "  usb-image      Build toyos-usb.img -- compact, self-booting, for dd to a USB stick"
	@echo "  live-iso       Build toy-os-live.iso -- carries a filesystem image, boots with NO disk"
	@echo "  demo-iso       Build toy-os-demo.iso -- boots straight into a scripted tour"
	@echo ""
	@echo " Any of the three ISO targets can BAKE IN boot flags, so they need not be"
	@echo " typed into the GRUB menu every boot -- docs/boot-flags.md lists every word:"
	@echo "   make iso KCMDLINE=\"video=1920x1080 nokaslr\""
	@echo "   video= goes up to 3840x2160 and works on EVERY VGA= below now (std"
	@echo "   included, through the bochs driver). A WINDOW still caps at 1920x1080."
	@echo ""
	@echo " THERE IS ONE RUN TARGET. Everything that used to be its own run-* target is a"
	@echo " variable on it, so any COMBINATION works without a target per combination:"
	@echo "   make run KVM=1            KVM instead of TCG emulation (needs /dev/kvm;"
	@echo "                             port-I/O-heavy paths can get SLOWER, so do not"
	@echo "                             compare throughput numbers against plain run)"
	@echo "   make run VIRTIO=1         virtio for EVERY device class below -- disk,"
	@echo "                             GPU and input at once. The modern machine."
	@echo ""
	@echo "   Each class also picks its own, and a per-class value overrides VIRTIO=1:"
	@echo "     DISK=ide|virtio|ahci    virtio removes the IDE controller ENTIRELY, so"
	@echo "                             the filesystem mounts only if virtio works;"
	@echo "                             ahci is a SATA drive on an ICH9 HBA"
	@echo "     VGA=std|virtio|vmware   virtio is the virtio-gpu driver, which nothing"
	@echo "                             else in the build exercises; vmware has a"
	@echo "                             HARDWARE cursor, which screendump cannot"
	@echo "                             capture -- do not use it for screenshot tests."
	@echo "                             ALL THREE CAN SET MODES now (std through the"
	@echo "                             bochs DISPI driver), so video= works on any"
	@echo "                             of them -- see the KCMDLINE line above"
	@echo "     INPUT=ps2|virtio        virtio attaches keyboard/mouse/tablet BESIDE"
	@echo "                             PS/2, so the input core has two sources"
	@echo "     USB=none|xhci|xhci+mouse|xhci+hub an xHCI controller and USB HID"
	@echo "                             devices; xhci+hub puts them behind a hub."
	@echo "                             NOTE: attaching usb-kbd takes the keyboard"
	@echo "                             AWAY from PS/2 -- QEMU routes keys to it"
	@echo "   Which driver actually claimed the display: type lsdev at the serial"
	@echo "   debug console. lspci only says the device is on the bus."
	@echo ""
	@echo "   make run AUDIO=1|usb|both PC speaker + a sound card (AUDIODEV=alsa etc.)"
	@echo "   make run WINDOW=full      start full-screen -- no decorations, so a guest"
	@echo "                             mode as big as the monitor is pixel-exact AND fits"
	@echo "   make run WINDOW=fit       a resizable window the guest is SCALED into (gtk)."
	@echo "                             The only one that helps when the guest mode is"
	@echo "                             BIGGER than the monitor; it blurs the font"
	@echo "   make run NOGRAPHIC=1      serial only, no window -- use this over SSH"
	@echo "   make run MENU=1           show the GRUB boot menu (5s) instead of booting"
	@echo "   make run MEM=512          a smaller machine"
	@echo "   make run LIVE=1           the live ISO, with NO disk attached (implies live-iso)"
	@echo "   make run DEMO=1           the scripted tour, no disk (implies demo-iso)"
	@echo "   make run NODISK=1         the ordinary ISO with no disk attached either"
	@echo "   make run BOOT=cd          boot the ISO instead of the disk (default: the"
	@echo "                             disk, when disk.img carries GRUB -- BOOT=disk"
	@echo "                             forces it, see tools/install_grub.py)"
	@echo "   make run KVM=1 VIRTIO=1   ...or any mix; a per-class value such as"
	@echo "                             VGA=std overrides what VIRTIO=1 chose"
	@echo ""
	@echo "  debug          Boot toy-os frozen (QEMU's -s -S) for real GDB"
	@echo "                 debugging -- attach with: gdb build/kernel.bin -ex"
	@echo "                 'target remote localhost:1234', then continue"
	@echo "  test           Run the in-kernel test suite (ktest) and exit non-zero on failure"
	@echo "  verify         Full pre-delivery check: clean build + iso + boot test + ktest"
	@echo "  clean          Remove build outputs (build/, ELFs, the ISOs and toyos-usb.img) -- leaves disk.img alone"
	@echo "  clean-disk     Wipe disk.img -- the filesystem AND the bootloader on it;"
	@echo "                 the next make iso rebuilds both. Use with care"
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
# sys.o carries the syscall wrappers (userland/sys.c), stack_chk.o the
# canary symbols GCC emits references to, sigtramp.o the two
# instructions a signal handler returns through, and tls.o the
# thread-local storage crt0 installs before main(). All five are linked
# into every userland ELF, which is what lets a program be nothing but
# its own main().
#
# sigtramp.o is kept honest by --gc-sections rather than by anything
# here: nothing references __sigrestore unless a program installs a
# handler, so a program that does not use signals does not carry it.
USERLAND_RT = $(BUILD)/userland/rt/crt0.o $(BUILD)/userland/rt/sys.o \
              $(BUILD)/userland/rt/stack_chk.o $(BUILD)/userland/rt/sigtramp.o \
              $(BUILD)/userland/rt/tls.o

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
               $(BUILD)/userland/shared/ansi.o \
               $(BUILD)/userland/shared/klineedit.o \
               $(BUILD)/userland/shared/completion.o \
               $(BUILD)/userland/shared/ttf.o \
               $(BUILD)/userland/shared/klineedit_cases.o
LIBUAPP      = $(BUILD)/userland/libuapp.a

# libc.a -- the C LIBRARY, a second archive beside the toolkit.
#
# The split is by AUDIENCE, the same call userland/include/ made for the
# headers: a /bin program links the C library and nothing else, while a
# GUI app links the C library plus Toykit. One archive would have made
# the libc's audience "toy-os apps" rather than "any C program", which
# is the opposite of what docs/libc-design.md is aiming at -- and it is
# cheap to separate now and awkward once every program depends on the
# merged shape.
#
# The shared objects in here are the C library's implementation:
# string.c and knum.c are what <string.h>'s inlines call, kfmt.c is the
# formatter behind snprintf() and printf(), heap_core.c is malloc, and
# caltime.c is the calendar arithmetic behind <time.h>. They sit in
# libc.a rather than libuapp.a for the same audience reason.
#
# LINK ORDER MATTERS AND IT IS libuapp THEN libc. Toykit calls strlen()
# and snprintf(); the C library calls nothing in Toykit. A linker
# resolves an archive against what is still undefined at the moment it
# reaches it, so the dependency has to come first -- reversing these two
# fails at link time with undefined k_* references from widgets, which
# is at least a loud failure rather than a quiet one.
LIBC_SRCS = $(shell find userland/libc -name '*.c' 2>/dev/null | sort)
# .S as well as .c: setjmp/longjmp cannot be written in C at all -- it
# is eight registers and a stack pointer -- and is the only assembly in
# the C library. GNU `.S` rather than the kernel's NASM `.asm` because
# everything under userland/ is built through gcc, which runs the
# preprocessor and the assembler itself.
LIBC_ASM  = $(shell find userland/libc -name '*.S' 2>/dev/null | sort)
# kpath.o joined this list when Image Viewer needed to join a directory
# and a filename: k_path_join()/_basename()/_dirname() are the kernel's,
# they are covered by KTESTs, and kpath.c includes nothing kernel-only --
# so ring 3 links the same implementation rather than growing a fourth
# hand-rolled copy of "concatenate with exactly one slash", which is the
# duplication CLAUDE.md names by example.
LIBC_OBJS = $(patsubst userland/%.c,$(BUILD)/userland/%.o,$(LIBC_SRCS)) \
               $(patsubst userland/%.S,$(BUILD)/userland/%.o,$(LIBC_ASM)) \
               $(BUILD)/userland/shared/string.o \
               $(BUILD)/userland/shared/knum.o \
               $(BUILD)/userland/shared/kpath.o \
               $(BUILD)/userland/shared/kfmt.o \
               $(BUILD)/userland/shared/heap_core.o \
               $(BUILD)/userland/shared/caltime.o \
               $(BUILD)/userland/shared/ksignal.o \
               $(BUILD)/userland/shared/kfmt_cases.o
LIBC         = $(BUILD)/userland/libc.a

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

$(BUILD)/userland/%.o: userland/%.S | version
	@mkdir -p $(dir $@)
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(LIBC): $(LIBC_OBJS)
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
                        wm/wm_debug wm/wm_tray wm/wm_taskbar wm/wm_watchdog \
                        wm/desktop wm/start_menu wm/context_menu wm/calendar_popup \
                        wm/volume_popup wm/wm_overlay \
                        wm/confirm_dialog wm/file_picker wm/cursor_theme \
                        wm/gui_apps wm/wm_log wm/wm_fs wm/wm_conf \
                        wm/wm_hwcursor
# icon_cache is NOT in that list any more: it moved to userland/lib/ when
# the toolkit's sidebar needed icons too, so it comes from libuapp.a like
# every other shared piece. The archive is linked into every userland ELF
# and --gc-sections drops it from the ones that never call icon_get().

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

# --- DOOM ------------------------------------------------------------
#
# userland/ports/doom/ is doomgeneric, vendored byte for byte (see its
# README). userland/backends/doom/ is OURS: the five DG_* platform
# the key translation, kept outside the vendored directory so the
# boundary between third-party and written-here is a directory boundary.
#
# WILDCARDED, unlike EXTRA_OBJS_toywm above, and the difference is who
# owns the file list. The WM's fourteen units are ours, so a stray .c
# dropped in there should fail to link rather than be absorbed silently.
# This list is UPSTREAM's -- it is exactly what doomgeneric's own
# Makefile compiles -- so curating it here by hand would mean a second
# copy of somebody else's build to keep in step, and the failure mode of
# getting it wrong is a link error either way.
DOOM_PORT_SRCS = $(shell find userland/ports/doom -name '*.c' 2>/dev/null | sort)
DOOM_PORT_OBJS = $(patsubst userland/%.c,%,$(DOOM_PORT_SRCS))
# OUR backend is discovered the same way, so adding a piece of it (the
# sound module, the OPL driver) is a .c file and nothing else -- the
# rule the rest of userland/ already follows.
DOOM_BACKEND_SRCS = $(shell find userland/backends/doom -name '*.c' 2>/dev/null | sort)
DOOM_BACKEND_OBJS = $(patsubst userland/%.c,%,$(DOOM_BACKEND_SRCS))
EXTRA_OBJS_doom = $(DOOM_BACKEND_OBJS) $(DOOM_PORT_OBJS)

# VENDORED CODE IS COMPILED WITH ITS WARNINGS OFF, on purpose.
#
# 36,000 lines of 1990s C does not pass -Wall -Wextra, and this project's
# rule for the directory is that nobody may "fix" it to match local
# conventions -- so a warning here is noise nobody is permitted to act
# on, and noise that would bury a real one from our own code.
#
# **EXCEPT THE FRAME-SIZE WARNING, WHICH IS KEPT AND RAISED.** That one
# is not style: a frame larger than UADDR_STACK_GROW_GAP (64 KiB) leaps
# the growable region and dies on a stack that was willing to grow for
# it (kernel/include/kernel/uaddr.h). 16 KiB leaves a wide margin under
# that and still names anything alarming. It cannot simply be dropped to
# -w, because -w would silence this too.
DOOM_CFLAGS = $(subst -Wframe-larger-than=2048,-Wframe-larger-than=16384,\
                 $(subst -Wextra,,$(subst -Wall,-w,$(USERLAND_CFLAGS)))) \
               -Iuserland/ports/doom -Iuserland/ports/doom/opl \
               -Iuserland/backends/doom/compat \
               -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 -DFEATURE_SOUND

# More specific than the generic userland rule below it, so make prefers
# it: a pattern rule with a shorter stem wins.
# midifile.c reaches SDL_SwapBE16/32 through i_swap.h, which upstream
# Chocolate Doom routes to SDL's endian header and doomgeneric's copy
# does not. FORCE-INCLUDED rather than patched, because i_swap.h is
# vendored -- and scoped to the one object that needs it rather than
# pushed through all eighty.
$(BUILD)/userland/ports/doom/midifile.o: DOOM_CFLAGS += -include SDL.h

$(BUILD)/userland/ports/doom/%.o: userland/ports/doom/%.c | version
	@mkdir -p $(dir $@)
	$(CC) $(DOOM_CFLAGS) $< -o $@

# OUR backend needs doomgeneric's headers (doomgeneric.h, doomkeys.h) but
# is ours, so it keeps every warning. Scoped to this one object with a
# target-specific variable, exactly as calculator.o gets -Iapps, so no
# other userland program gains the ability to include doom's headers.
# A PATTERN-specific variable, so every file of the backend gets it and
# adding one needs no Makefile edit. FEATURE_SOUND is defined HERE rather
# than in doomfeatures.h because that header is vendored: upstream ships
# it with the flag commented out, and editing it would be a divergence
# somebody has to carry forever (see that directory's README).
$(BUILD)/userland/backends/doom/%.o: USERLAND_CFLAGS += -Iuserland/ports/doom \
                                     -Iuserland/ports/doom/opl \
                                     -Iuserland/backends/doom/compat \
                                     -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 \
                                     -DFEATURE_SOUND

# THE ONE VENDORED PROGRAM. cjson_test is ours; userland/ports/cjson/ is
# upstream's source byte for byte (see its README), and it is linked in
# per-binary rather than added to an archive so that nothing else can
# accidentally depend on third-party code. docs/libc-design.md's Stage 6
# is what it is for: a program nobody working on this repo wrote,
# compiled against this C library.
# The File Manager is six translation units, and `userland/fm/` is
# outside USERLAND_PROGRAM_DIRS for the same reason `userland/wm/` is:
# those directories turn every .c into its own ELF, which is right for a
# program and wrong for one program's parts. Listed rather than
# wildcarded, again as toywm is -- a stray .c dropped in there should
# fail to link with an undefined symbol, not be absorbed silently.
EXTRA_OBJS_files = fm/fm_view fm/fm_jobs fm/fm_tree fm/fm_thumbs fm/fm_modal

EXTRA_OBJS_cjson_test = ports/cjson/cJSON
EXTRA_OBJS_cjson_bench = ports/cjson/cJSON

# The extras for one binary, as real object paths.
uextra = $(patsubst %,$(BUILD)/userland/%.o,$(EXTRA_OBJS_$(notdir $(1))))

# .SECONDEXPANSION lets the prerequisite list reference the stem: `$$*`
# survives make's first expansion (when the rule is read, and the stem
# isn't known yet) and is expanded a second time per target, once it is.
# Without it there is no way for one pattern rule to depend on a
# per-target variable, which is the whole point here.
.SECONDEXPANSION:

# --- the shared C library (dynlink Stage 3) --------------------------
#
# The SAME sources as libc.a, compiled a second time with -fpic into
# build/userland-pic/ (the geom.c compiled-twice pattern, one axis
# over) -- minus pthread.c, whose `__thread g_self` is TLS a library
# may not carry here: it lands in LIBC_NONSHARED instead, glibc's own
# libc_nonshared.a shape, statically linked into every DYNAMIC program
# that references it. errno needs no such trick -- g_errno lives in
# rt/sys.o, which is static in every binary, and libc.so reaches it
# through the exe's exported __errno_location.
LIBC_PIC_CFLAGS = $(subst -fpie,-fpic,$(USERLAND_CFLAGS))
LIBC_PIC_SHARED_CFLAGS = $(subst -fpie,-fpic,$(SHARED_CFLAGS))

LIBC_PIC_OBJS = $(patsubst userland/%.c,$(BUILD)/userland-pic/%.o,$(filter-out userland/libc/pthread.c,$(LIBC_SRCS))) \
                $(patsubst userland/%.S,$(BUILD)/userland-pic/%.o,$(LIBC_ASM)) \
                $(BUILD)/userland-pic/shared/string.o \
                $(BUILD)/userland-pic/shared/knum.o \
                $(BUILD)/userland-pic/shared/kpath.o \
                $(BUILD)/userland-pic/shared/kfmt.o \
                $(BUILD)/userland-pic/shared/heap_core.o \
                $(BUILD)/userland-pic/shared/caltime.o \
                $(BUILD)/userland-pic/shared/ksignal.o \
                $(BUILD)/userland-pic/shared/kfmt_cases.o

LIBC_NONSHARED = $(BUILD)/userland/libc_nonshared.a

$(BUILD)/userland-pic/%.o: userland/%.c | version
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_PIC_CFLAGS) $< -o $@

$(BUILD)/userland-pic/%.o: userland/%.S | version
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_PIC_CFLAGS) $< -o $@

$(BUILD)/userland-pic/shared/%.o: kernel/lib/%.c | version
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_PIC_SHARED_CFLAGS) -Iapps $< -o $@

$(BUILD)/userland-pic/shared/%.o: apps/%.c | version
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_PIC_SHARED_CFLAGS) -Iapps $< -o $@

LIBC_SO = $(BUILD)/lib/libc.so
$(LIBC_SO): $(LIBC_PIC_OBJS)
	@mkdir -p $(dir $@)
	$(LD) -shared --hash-style=sysv -z max-page-size=4096 -soname libc.so -o $@ $(LIBC_PIC_OBJS)

$(LIBC_NONSHARED): $(BUILD)/userland/libc/pthread.o
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

# One link line for every DYNAMIC executable. --gc-sections is safe
# here ONLY because --export-dynamic makes exported symbols gc roots
# (measured: every one of libc.so's imports survives) -- binding is
# eager, so a dropped sys_* would kill the program at LAUNCH, not at
# the missing call. No -n: it would force a static link.
DYN_LINK = $(LD) --gc-sections -T userland/rt/link-dyn.ld -nostdlib \
           --dynamic-linker=/lib/ld-toy.so --export-dynamic \
           --hash-style=sysv -z nocopyreloc

# The Stage-3 proof: a binary whose WHOLE libc is /lib/libc.so -- no
# libc.a on this line at all.
$(BUILD)/userland/tests/dynlibc_test.elf: $(BUILD)/userland/tests/dynlibc_test.o $(USERLAND_RT) userland/rt/link-dyn.ld $(LIBUAPP) $(LIBC_NONSHARED) $(LIBC_SO) $(LDSO)
	$(DYN_LINK) -o $@ $(BUILD)/userland/rt/crt0.o $< $(BUILD)/userland/rt/sys.o $(BUILD)/userland/rt/stack_chk.o $(BUILD)/userland/rt/sigtramp.o $(BUILD)/userland/rt/tls.o $(LIBUAPP) $(LIBC_NONSHARED) $(LIBC_SO)

# EVERY /bin AND GUI PROGRAM LINKS libc.so (the user's 2026-08-28
# call, docs/decisions.md): both are only ever started through
# SYS_SPAWN -- a bare name at either shell spawns -- so nothing loses
# the legacy `run`, which refuses dynamic by design. /tests stays
# static because usertest_run drives it THROUGH `run` for the exit
# codes. These two patterns beat the generic static rule below by
# stem length (make picks the most specific match).
#
# TWO EXCEPTIONS, static on purpose:
#   init   -- pid 1; the machine must reach a shell with /lib broken
#             or missing, and init is what starts every service.
#   (toywm is outside USERLAND_PROGRAM_DIRS and stays static by
#    construction -- same reasoning: the desktop is what a rescue
#    happens on.)
$(BUILD)/userland/bin/%.elf: $(BUILD)/userland/bin/%.o $(USERLAND_RT) userland/rt/link-dyn.ld $(LIBUAPP) $(LIBC_NONSHARED) $(LIBC_SO) $(LDSO) $$(call uextra,$$*)
	$(DYN_LINK) -o $@ $(BUILD)/userland/rt/crt0.o $< $(call uextra,$*) $(BUILD)/userland/rt/sys.o $(BUILD)/userland/rt/stack_chk.o $(BUILD)/userland/rt/sigtramp.o $(BUILD)/userland/rt/tls.o $(LIBUAPP) $(LIBC_NONSHARED) $(LIBC_SO)

$(BUILD)/userland/gui/%.elf: $(BUILD)/userland/gui/%.o $(USERLAND_RT) userland/rt/link-dyn.ld $(LIBUAPP) $(LIBC_NONSHARED) $(LIBC_SO) $(LDSO) $$(call uextra,$$*)
	$(DYN_LINK) -o $@ $(BUILD)/userland/rt/crt0.o $< $(call uextra,$*) $(BUILD)/userland/rt/sys.o $(BUILD)/userland/rt/stack_chk.o $(BUILD)/userland/rt/sigtramp.o $(BUILD)/userland/rt/tls.o $(LIBUAPP) $(LIBC_NONSHARED) $(LIBC_SO)

$(BUILD)/userland/bin/init.elf: $(BUILD)/userland/bin/init.o $(USERLAND_RT) userland/rt/link.ld $(LIBUAPP) $(LIBC)
	$(LD) -n --gc-sections -T userland/rt/link.ld -nostdlib -o $@ $(BUILD)/userland/rt/crt0.o $< $(BUILD)/userland/rt/sys.o $(BUILD)/userland/rt/stack_chk.o $(BUILD)/userland/rt/sigtramp.o $(BUILD)/userland/rt/tls.o $(LIBUAPP) $(LIBC)

# --- dynamic linking (dynlink Stage 2, docs/dynlink-design.md) -------
#
# /lib/ld-toy.so is a fixed-base ET_EXEC at ELF_LDSO_BASE (its link.ld
# and kernel/include/kernel/elf.h must agree), freestanding except for
# stack_chk.o -- which is freestanding too. It is NOT a program in
# USERLAND_PROGRAM_DIRS: it seeds to /lib, not /bin.
#
# A shared library compiles -fpic (not the executables' -fpie -- fpie
# objects may not enter a shared object) and links with
# --hash-style=sysv (the loader's symbol lookup is sysv-hash only) and
# -z max-page-size=4096 (the loader maps segments file-backed, and a
# 2 MiB-aligned .so's offsets are not page-congruent under 4 KiB
# pages -- ld-toy refuses such a file by name).
LDSO    = $(BUILD)/lib/ld-toy.so
DYNLIBS = $(BUILD)/lib/libhello.so

$(BUILD)/userland/dynlib/%.o: USERLAND_CFLAGS := $(subst -fpie,-fpic,$(USERLAND_CFLAGS))

$(LDSO): $(BUILD)/userland/ldso/entry.o $(BUILD)/userland/ldso/ldso.o $(BUILD)/userland/rt/stack_chk.o userland/ldso/link.ld
	@mkdir -p $(dir $@)
	$(LD) -n --gc-sections -T userland/ldso/link.ld -nostdlib -o $@ $(BUILD)/userland/ldso/entry.o $(BUILD)/userland/ldso/ldso.o $(BUILD)/userland/rt/stack_chk.o

$(BUILD)/lib/libhello.so: $(BUILD)/userland/dynlib/hello_dl.o
	@mkdir -p $(dir $@)
	$(LD) -shared --hash-style=sysv -z max-page-size=4096 -soname libhello.so -o $@ $<

# The one test with its own link line: a DYNAMIC executable.
# link-dyn.ld adds the .interp/.dynamic/GOT/PLT homes the static script
# has no segments for; --export-dynamic is what lets the library call
# back into the program (the shape a shared libc needs for the exe's
# __errno_location); --hash-style=sysv because the loader resolves
# against the EXECUTABLE's hash table too; -z nocopyreloc because ld
# otherwise turns a lib-data reference into an R_X86_64_COPY the
# loader deliberately does not implement (the GOT indirection -fpie
# already pays for is strictly better).
$(BUILD)/userland/tests/dyn_test.elf: $(BUILD)/userland/tests/dyn_test.o $(USERLAND_RT) userland/rt/link-dyn.ld $(LIBUAPP) $(LIBC) $(DYNLIBS) $(LDSO)
	$(LD) --gc-sections -T userland/rt/link-dyn.ld -nostdlib --dynamic-linker=/lib/ld-toy.so --export-dynamic --hash-style=sysv -z nocopyreloc -o $@ $(BUILD)/userland/rt/crt0.o $< $(BUILD)/userland/rt/sys.o $(BUILD)/userland/rt/stack_chk.o $(BUILD)/userland/rt/sigtramp.o $(BUILD)/userland/rt/tls.o $(BUILD)/lib/libhello.so $(LIBUAPP) $(LIBC)

# --gc-sections drops every section nothing reaches, which is what
# makes linking against one archive cheap: `hello` references nothing in
# libuapp.a and gains nothing from it. The archive goes LAST -- a
# linker resolves archive members against the undefined symbols it has
# accumulated so far, so an archive placed before its callers
# contributes nothing and the link fails with undefined references.
$(BUILD)/userland/%.elf: $(BUILD)/userland/%.o $(USERLAND_RT) userland/rt/link.ld $(LIBUAPP) $(LIBC) $$(call uextra,$$*)
	$(LD) -n --gc-sections -T userland/rt/link.ld -nostdlib -o $@ $(BUILD)/userland/rt/crt0.o $< $(call uextra,$*) $(BUILD)/userland/rt/sys.o $(BUILD)/userland/rt/stack_chk.o $(BUILD)/userland/rt/sigtramp.o $(BUILD)/userland/rt/tls.o $(LIBUAPP) $(LIBC)

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
# AND THE C LIBRARY IS TAKEN BACK OFF THE INCLUDE PATH HERE. A file on
# this list is compiled into BOTH rings, so it may only use the toolkit
# -- kernel/lib/klineedit.c, kfmt.c and heap_core.c all include
# "string.h", and with -Iuserland/include in front that quoted include
# would silently resolve to the C library's header in the ring-3 build
# and the toolkit's in the kernel build: the same source line meaning
# two different files depending on which pass compiled it. Removing the
# flag makes "freestanding, toolkit only" something the build ENFORCES
# rather than something the comment above asks for.
SHARED_CFLAGS = $(subst $(LIBC_INCLUDES),,$(USERLAND_CFLAGS))

$(BUILD)/userland/shared/%.o: kernel/lib/%.c | version
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CFLAGS) -Iapps $< -o $@

$(BUILD)/userland/shared/%.o: apps/%.c | version
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CFLAGS) -Iapps $< -o $@

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
# EXTRAS=1 fetches the optional, differently-licensed material before
# seeding -- today that is the Doom shareware IWAD. OFF by default, so no
# ordinary build reaches the network and no ordinary image carries
# anything but ours. LICENSE=agree answers the licence prompt, which a
# build with no terminal cannot answer and is REFUSED rather than hung on
# (the mtools trap, from a different direction). tools/fetch_extras.py
# holds the registry and the reasoning.
EXTRAS ?=
LICENSE ?=
seed: $(DISK_IMG) $(USERLAND_ELVES) $(KERNEL) $(LDSO) $(DYNLIBS) $(LIBC_SO)
	$(if $(EXTRAS),TOYOS_LICENSE=$(LICENSE) python3 tools/fetch_extras.py,@true)
	mkdir -p $(SEED_DIR)/sync/bin $(SEED_DIR)/sync/tests
	# The dynamic loader and the shared libraries -- /lib is theirs
	# (docs/filesystem-layout.md).
	mkdir -p $(SEED_DIR)/sync/lib
	cp $(LDSO) $(DYNLIBS) $(LIBC_SO) $(SEED_DIR)/sync/lib/
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
	cp $(USB_IDS) $(SEED_DIR)/sync/usr/share/hwdata/usb.ids
	# TWO TEXT FILES THAT SHIP, and they are deliberately not one.
	#
	# /usr/share/doc/toy-os.txt is a DOCUMENT -- prose about this system,
	# meant to be read. /tests/sample.txt is a FIXTURE: every line names
	# its own number, and the content is hostile on purpose (a
	# 400-column line, an exactly-80 one, trailing spaces, a tab, and a
	# last line with no newline). Splitting them is what lets the
	# fixture be awkward without making the document worse to read, and
	# what lets the document be edited without breaking a test's line
	# numbers. Both exist because anything that pages, scrolls or edits
	# needed a real subject and had to manufacture one -- and the usual
	# way, repeating a line, produces content whose movement is
	# pixel-identical. See docs/filesystem-layout.md.
	mkdir -p $(SEED_DIR)/sync/usr/share/doc
	cp data/usr/share/doc/toy-os.txt $(SEED_DIR)/sync/usr/share/doc/toy-os.txt
	cp data/tests/sample.txt $(SEED_DIR)/sync/tests/sample.txt
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
	# Runtime-loadable fonts -- one .ttf per face (see
	# docs/filesystem-layout.md and kernel/drivers/font_face.c). The face
	# NAME is the filename without the extension, so adding a face is
	# dropping a file into data/fonts/ and nothing else. Tracked (not
	# copied from the host's installed fonts) deliberately: a build-time
	# dependency on a system font is the same failure that shipped images
	# with no keyboard layouts at all for months, silently. The LICENSE-*
	# files travel with them, which the fonts' own licenses require.
	mkdir -p $(SEED_DIR)/sync/usr/share/fonts
	cp data/fonts/*.ttf data/fonts/LICENSE-*.txt $(SEED_DIR)/sync/usr/share/fonts/
	# The Doom IWAD, IF ONE HAS BEEN FETCHED. Conditional because it is
	# the one seeded file that is deliberately NOT in the repository --
	# `tools/fetch_wad.py` puts it in data/doom/, and a clean checkout
	# has no such directory (see docs/decisions.md for the licensing
	# reason). A missing WAD is not a build failure: DOOM installs
	# either way and says in its own window that it could not find one.
	@if ls data/doom/*.wad >/dev/null 2>&1; then 	    mkdir -p $(SEED_DIR)/sync/usr/share/doom; 	    cp data/doom/*.wad $(SEED_DIR)/sync/usr/share/doom/; 	fi
	# What this image carries that is NOT ours, written by
	# tools/fetch_extras.py and present only after an EXTRAS build. It
	# rides INSIDE the image because that is the only note still attached
	# to it when somebody decides whether to publish it.
	@if [ -f data/usr/share/licenses/extras.txt ]; then 	    mkdir -p $(SEED_DIR)/sync/usr/share/licenses; 	    cp data/usr/share/licenses/extras.txt $(SEED_DIR)/sync/usr/share/licenses/; 	fi
	# Wallpapers -- one JPEG per background, named by filename without
	# the extension exactly as a font face is (see
	# docs/filesystem-layout.md). Generated by tools/gen_imgdata.py into
	# data/wallpapers/ and tracked, so a checkout with no Pillow still
	# has backgrounds; staged here for the same reason everything above
	# is, since sync/ is deleted by `make clean`. Adding one is dropping
	# a file into data/wallpapers/ and nothing else -- the desktop lists
	# the directory rather than holding a list.
	mkdir -p $(SEED_DIR)/sync/usr/share/wallpapers
	cp data/wallpapers/*.jpg $(SEED_DIR)/sync/usr/share/wallpapers/
	# Sounds -- one WAV per effect, named by filename without the
	# extension (the same rule as a wallpaper or a font face). Generated
	# by tools/gen_audio.py into data/usr/share/sounds/ and tracked, so a
	# checkout has sound without regenerating anything. Every file is in
	# a DIFFERENT format on purpose -- see that script's header.
	mkdir -p $(SEED_DIR)/sync/usr/share/sounds
	cp data/usr/share/sounds/*.wav $(SEED_DIR)/sync/usr/share/sounds/
	# The audio FIXTURE, beside /tests/sample.txt and for the same
	# reason: a steady 1 kHz tone at 44.1 kHz is what tools/audio_test.py
	# measures on the host, and the shipped sounds are musical rather
	# than measurable.
	cp data/tests/sine1k.wav $(SEED_DIR)/sync/tests/sine1k.wav
	# ...and the same tone as an MP3, so the one host-side oracle that
	# measures FREQUENCY covers the decoder as well as the WAV path.
	cp data/tests/sine1k.mp3 $(SEED_DIR)/sync/tests/sine1k.mp3
	# Music -- one MP3 per track, named by filename without the extension,
	# the same rule as a sound or a wallpaper. Generated by
	# tools/gen_music.py and tracked, so a checkout has something to play.
	mkdir -p $(SEED_DIR)/sync/usr/share/music
	cp data/usr/share/music/*.mp3 $(SEED_DIR)/sync/usr/share/music/
	# Application icons -- one QOI per icon NAME, which is what a
	# .desktop entry's Icon= key names (freedesktop's rule, and the same
	# filename-is-the-name rule fonts and cursor themes follow here).
	# Generated by tools/gen_icons.py into data/icons/ and tracked, for
	# the reason every other staged directory is: sync/ is deleted by
	# `make clean`, so a file written straight into it exists on exactly
	# one machine.
	mkdir -p $(SEED_DIR)/sync/usr/share/icons
	cp data/icons/*.qoi $(SEED_DIR)/sync/usr/share/icons/
	# Service descriptors -- what init starts, one file per service (see
	# docs/init-design.md and data/etc/services.d/README.md). Tracked
	# under data/ and staged here for the same reason pci.ids and the
	# cursor themes are: sync/ is deleted by `make clean`.
	# Every file but the README, so adding a service is dropping a file
	# here -- the same rule data/wm/desktop/ follows.
	mkdir -p $(SEED_DIR)/sync/etc/services.d
	@for f in data/etc/services.d/*; do \
	    if [ "$$(basename $$f)" != "README.md" ]; then \
	        cp $$f $(SEED_DIR)/sync/etc/services.d/; fi; \
	done
	# AVAILABLE services, which init never reads: /usr/share/services is
	# the descriptor a `service enable` copies into /etc/services.d. The
	# split is systemd's /lib vs /etc, and it is what lets a service ship
	# turned OFF rather than not ship at all.
	mkdir -p $(SEED_DIR)/sync/usr/share/services
	@for f in data/usr/share/services/*; do \
	    if [ "$$(basename $$f)" != "README.md" ]; then \
	        cp $$f $(SEED_DIR)/sync/usr/share/services/; fi; \
	done
	# The human-facing text for each setting -- descriptions, choice
	# display names and presentation hints, one file per setting (see
	# data/etc/settings.d/README.md). Same rule again: every file but
	# the README, so adding text for a setting is dropping a file here.
	# Absent is a supported state -- a setting with no file falls back to
	# its compiled-in label -- which is exactly why these are seeded
	# rather than compiled in.
	mkdir -p $(SEED_DIR)/sync/etc/settings.d
	@for f in data/etc/settings.d/*; do \
	    if [ "$$(basename $$f)" != "README.md" ]; then \
	        cp $$f $(SEED_DIR)/sync/etc/settings.d/; fi; \
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
	# THE KERNEL GOES ON THE DISK TOO. GRUB cannot read TFS3, so
	# /boot is a FAT32 partition in front of it and GRUB's core.img
	# is embedded in a BIOS boot partition in front of THAT -- see
	# tools/install_grub.py. Same grub.cfg as the ISO's, same paths
	# inside it, so the two media cannot drift. `--optional` is what
	# lets a disk.img built before this layout existed keep working:
	# it has nowhere to install to, so nothing is installed and every
	# launcher falls back to booting the ISO.
	sed -e 's/@GRUB_TIMEOUT@/$(GRUB_TIMEOUT)/' -e 's|@KCMDLINE@|$(KCMDLINE)|' grub.cfg > $(BUILD)/grub-disk.cfg
	python3 tools/install_grub.py $(DISK_IMG) --kernel $(KERNEL) \
	    --grub-cfg $(BUILD)/grub-disk.cfg --optional
	@touch $(BUILD)/.bootdisk
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

# DERIVED FROM THE SEED TREE, not a constant -- and the constant it
# replaces had already gone stale. It was 24 MiB; the seed tree reached
# ~33 MiB at some point nobody noticed, because the live image is built
# by an on-demand target and its failure ("image is out of free blocks")
# only reaches whoever types `make live-iso`. A number in a Makefile
# that some other directory has to stay smaller than is the
# pointer-somebody-must-maintain shape this repo keeps deleting.
#
# The tree, plus a quarter for TFS3's metadata and 4 KiB block
# granularity, plus 8 MiB of room to work in and to cover the partition
# table's own overhead (the 1 MiB alignment gap at the front and GPT's
# backup structures at the back). Deferred (`=`), so it is measured
# when the recipe runs -- which is after `seed` has populated the tree.
LIVE_IMG_MB = $(shell echo $$(( $$(du -sm --apparent-size $(SEED_DIR)/sync 2>/dev/null | cut -f1) * 5 / 4 + 8 )))

# PARTITIONED, like every other volume this OS mounts. The kernel takes
# a root from a partition and refuses a whole-disk volume (kernel/fs/
# vfs.c, docs/rootfs-design.md), and a RAM image is not an exception to
# that -- it is mounted through the same block layer and the same probe.
# An exemption would live in the one function whose whole job is to have
# one rule.
#
# One DATA partition and no boot partition: the ISO's GRUB loads the
# kernel here, so there is nothing for a BIOS boot or ESP partition to
# hold. `--print-volume` is how the trim afterwards finds the volume
# rather than assuming an offset.
$(LIVE_IMG): $(USERLAND_ELVES) seed
	@mkdir -p $(BUILD)
	rm -f $(LIVE_IMG)
	truncate -s $$(( $(LIVE_IMG_MB) * 1024 * 1024 )) $(LIVE_IMG)
	python3 tools/seed_disk.py $(LIVE_IMG) $(SEED_DIR) --layout rest
	V=`python3 tools/mkpart_test.py $(LIVE_IMG) --print-volume`; \
	python3 tools/tfs3_writer.py trim $(LIVE_IMG) \
	    --at-lba $${V%% *} --sectors $${V##* }

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

# A COMPACT, SELF-BOOTING IMAGE, for writing to a USB stick.
#
# WHY NOT JUST dd disk.img. That image is 9 GB because it is a
# development scratch disk and sparseness makes the size free -- but a
# stick is written byte for byte, so dd'ing it means 9 GB over USB for
# ~50 MB of content. This is the same thing at a size that writes in
# seconds. USB_SIZE overrides it; the root filesystem takes whatever is
# left after the 1 MiB BIOS boot partition and the 64 MiB ESP.
#
# BIOS (i386-pc) ONLY, like disk.img, because tools/install_grub.py
# installs one loader -- so a machine booting this must have CSM/legacy
# boot enabled. That is not a limitation of the stick; it is the same
# one every `make run BOOT=disk` has, which is also why this path is the
# most exercised one in the repo.
USB_IMG  = toyos-usb.img
USB_SIZE = 512M

usb-image: version $(KERNEL) $(USERLAND_ELVES) seed
	rm -f $(USB_IMG)
	truncate -s $(USB_SIZE) $(USB_IMG)
	python3 tools/seed_disk.py $(USB_IMG) $(SEED_DIR)
	sed -e 's/@GRUB_TIMEOUT@/$(GRUB_TIMEOUT)/' -e 's|@KCMDLINE@|$(KCMDLINE)|' \
	    grub.cfg > $(BUILD)/grub-usb.cfg
	python3 tools/install_grub.py $(USB_IMG) --kernel $(KERNEL) \
	    --grub-cfg $(BUILD)/grub-usb.cfg
	@echo ""
	@echo "  $(USB_IMG) is ready and boots itself (BIOS/CSM, not UEFI)."
	@echo "  Find the stick with 'lsblk' and CHECK THE SIZE, then:"
	@echo "      sudo dd if=$(USB_IMG) of=/dev/sdX bs=4M status=progress conv=fsync"
	@echo "  /dev/sdX is the WHOLE DEVICE, not a partition (no digit)."
	@echo ""

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

# -vga std: explicit (matches QEMU's own default, but pinned here so the
#   higher 1280x720 mode boot.asm requests isn't at the mercy of a
#   per-host/per-distro QEMU default changing underneath us).
# -display sdl,grab-mod=rctrl: shows the framebuffer at its native
#   resolution, one real pixel per emulated pixel. (This comment said
#   `gtk,zoom-to-fit=off` long after the recipe had moved to sdl; the
#   reasoning was the same either way -- scaling would blur the
#   deliberately sharp, nearest-neighbour font -- but sdl cannot scale
#   at all, which is what the WINDOW axis below exists for.)
#   grab-mod=rctrl sets the key that captures/releases the mouse once
#   it's grabbed (right Ctrl) -- only meaningful now that -device
#   usb-mouse (below) makes the pointer relative and therefore actually
#   need grabbing.
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
# --- ONE QEMU RUN RECIPE, AND ONE TARGET ----------------------------
#
# There is exactly one way to boot this OS interactively -- `make run`
# -- and everything that used to be its own target is a VARIABLE on it.
#
# It was twelve near-identical qemu-system-x86_64 lines, which grew by
# MULTIPLICATION rather than addition: adding `run-virtio` immediately
# forced `run-virtio-kvm`, and `run-virtio-audio` and `run-vmware-kvm`
# did not exist only because nobody had asked. Collapsing the recipe
# fixed the duplication and left the NAMES behind as thin aliases --
# which turned out to be the same problem one level up, because a name
# per combination multiplies just as fast as a recipe per combination.
# So the aliases are gone too, and the axes are all there is:
#
#   make run                        the default: TCG, IDE disk, a window
#   make run KVM=1                  KVM instead of TCG emulation
#   make run VIRTIO=1               virtio for disk, GPU and input at once
#   make run DISK=virtio            just the disk, no IDE controller at all
#   make run VGA=virtio             just the GPU -- the virtio-gpu driver
#   make run VGA=vmware             the adapter with a hardware cursor
#   make run INPUT=virtio           just input: virtio keyboard/mouse/tablet
#   make run AUDIO=1                PC speaker wired to PulseAudio
#   make run WINDOW=full            full-screen, so a monitor-sized guest fits
#   make run WINDOW=fit             a resizable window the guest is scaled into
#   make run NOGRAPHIC=1            serial only, no window (over SSH)
#   make run MENU=1                 show GRUB's menu instead of booting
#   make run MEM=512                a smaller machine
#   make run LIVE=1                 the live ISO, with NO disk attached
#   make run DEMO=1                 the scripted tour, with no disk
#   make run KVM=1 VIRTIO=1         ...and any mix; a per-class value like
#                                   VGA=std overrides what VIRTIO=1 chose
#
# `make debug` is the one remaining relative, and it is not a
# combination of the above: it freezes the CPU for a debugger.
#
# This is the same shape CLAUDE.md legislates for C -- "a dispatch chain
# over ~20 branches should be a table", which tools/check_dispatch.py
# enforces on .c files and cannot see in a Makefile.
#
# EVERY DEFINITION HERE IS DEFERRED (`=`, never `:=`) AND USES $(if ...)
# RATHER THAN ifeq. That is load-bearing: `ifeq` is evaluated once when
# the Makefile is read, so a target-specific `run-kvm: KVM=1` would be
# invisible to it. $(if) expands when the recipe runs, which is when
# the target-specific value exists.
COMMA := ,

MEM ?= 2048

# WHICH ISO, and what has to be built first. LIVE and DEMO are ordinary
# axes like the rest, but they are the two that change the PREREQUISITE
# as well as the command line -- which works because a command-line
# variable is set before the Makefile is parsed, so $(if) expands
# correctly even in a prerequisite list.
QEMU_ISO     = $(if $(DEMO),$(DEMO_ISO),$(if $(LIVE),$(LIVE_ISO),$(ISO)))
RUN_PREREQ   = $(if $(DEMO),demo-iso,$(if $(LIVE),live-iso,iso $(DISK_IMG)))
QEMU_ACCEL   = $(if $(KVM),-enable-kvm -cpu host,)

# WHAT THE FRAMEBUFFER APPEARS IN, and whether it may be SCALED.
#
# `-display sdl` cannot scale or place its window -- its only suboptions
# are gl/grab-mod/show-cursor/window-close -- so the window is exactly
# the guest's framebuffer plus decorations. Ask for a mode the size of
# the monitor (`make iso KCMDLINE="video=1920x1080"`) and it cannot fit
# on that monitor, which is what this axis is for:
#
#   sdl    the default, unchanged: one real pixel per emulated pixel
#   full   the same backend, started full-screen. No decorations, so a
#          guest mode the size of the monitor is pixel-exact AND fits
#   fit    gtk with zoom-to-fit: freely resizable, and the guest is
#          SCALED into whatever size the window is. The only one of the
#          three that helps when the guest mode is BIGGER than the
#          monitor, and the only one that blurs the font -- which is why
#          it is not the default
#
# NOGRAPHIC=1 still wins over all three: no window at all.
#
# An unknown value is REFUSED rather than falling back to the default.
# `fit` and `full` are this Makefile's names rather than QEMU's, so a
# typo cannot be caught downstream the way `VGA=vitrio` is -- it would
# silently give a plain sdl window and look like the flag doing nothing.
WINDOW ?= sdl
# $(strip) because the continuations below put whitespace inside the
# expansion, which a shell tolerates and `make -n run` should not print.
#
# `fit` carries no grab-mod: that is an sdl suboption, and gtk has no
# equivalent (its own list is clipboard/full-screen/gl/grab-on-hover/
# show-tabs/show-cursor/window-close/show-menubar/zoom-to-fit), so
# releasing the pointer there is GTK's own Ctrl+Alt rather than rctrl.
QEMU_DISPLAY = $(strip $(if $(NOGRAPHIC),none,\
                 $(if $(filter fit,$(WINDOW)),gtk$(COMMA)zoom-to-fit=on,\
                   $(if $(filter sdl full,$(WINDOW)),sdl$(COMMA)grab-mod=rctrl,\
                     $(error WINDOW=$(WINDOW) is not one of: sdl, full, fit)))))
# Separate from QEMU_DISPLAY because -full-screen is a GLOBAL flag, not
# a suboption of the backend -- it works with either one.
QEMU_FULLSCREEN = $(if $(NOGRAPHIC),,$(if $(filter full,$(WINDOW)),-full-screen,))

# --- the three DEVICE-CLASS axes, and the one switch over all of them --
#
# Each class picks an implementation by NAME rather than by a boolean,
# because a boolean cannot express a third one and this machine is going
# to grow them: NVMe is a roadmap item, and `NVME=1` sitting beside a
# `VIRTIO=1` would immediately raise "what does setting both mean?".
# A named value has no such question, and `DISK=ide` is the explicit
# spelling of today's default rather than a thing you can only get by
# leaving something unset.
#
# **VIRTIO=1 SETS THE DEFAULT FOR ALL THREE**, so it is the "give me the
# modern machine" switch and not a disk flag. It USED to mean the disk
# alone, which read as a general statement and was not one -- a
# `make run KVM=1 VIRTIO=1` still booted `-vga std`, so the virtio GPU
# driver was never exercised by the invocation everyone reached for.
# Per-class values still win over it, so `VIRTIO=1 VGA=std` is a legal
# and meaningful thing to ask for.
DISK_KIND  = $(if $(DISK),$(DISK),$(if $(VIRTIO),virtio,ide))
QEMU_VGA   = $(if $(VGA),$(VGA),$(if $(VIRTIO),virtio,std))
INPUT_KIND = $(if $(INPUT),$(INPUT),$(if $(VIRTIO),virtio,ps2))

# The disk. `DISK=virtio` also removes the IDE controller entirely, and
# that is the point of the mode rather than a side effect -- ata_init()
# then finds nothing, so the filesystem mounts only if the whole virtio
# path works.
QEMU_DISK_IDE    = -drive file=$(DISK_IMG),format=raw,if=ide,discard=unmap
QEMU_DISK_VIRTIO = -drive file=$(DISK_IMG),format=raw,if=none,id=vblk,discard=unmap \
                   -device virtio-blk-pci,drive=vblk,disable-legacy=on
# DISK=ahci is a SATA drive behind an ICH9 host bus adapter -- what a
# machine built this decade presents, and what kernel/drivers/ahci.c
# drives. The legacy IDE controller is still on the bus (this is an
# i440fx machine) and has nothing attached to it, which is what makes
# `noahci` on the boot line a real fallback test rather than a
# no-disk boot.
QEMU_DISK_AHCI   = -device ich9-ahci,id=ahci \
                   -drive file=$(DISK_IMG),format=raw,if=none,id=sata0,discard=unmap \
                   -device ide-hd,drive=sata0,bus=ahci.0
# LIVE and DEMO attach NO -drive at all, and that is the whole point of
# them rather than an optimisation: pointing the live ISO at disk.img
# would let the ordinary disk path run and prove nothing about the live
# one.
QEMU_DISK = $(if $(NODISK)$(LIVE)$(DEMO),,\
              $(if $(filter virtio,$(DISK_KIND)),$(QEMU_DISK_VIRTIO),\
                $(if $(filter ahci,$(DISK_KIND)),$(QEMU_DISK_AHCI),$(QEMU_DISK_IDE))))

# Input. PS/2 by default (the 8042 the console has always used);
# `INPUT=virtio` attaches the three virtio-input devices BESIDE it, so
# the input core has two sources registered and the shared-IRQ path is
# exercised -- see kernel/drivers/virtio/virtio_input.c and
# tools/virtio_input_test.py, which is the only thing that attaches them
# in the suite.
QEMU_INPUT = $(if $(filter virtio,$(INPUT_KIND)),\
               -device virtio-keyboard-pci -device virtio-mouse-pci \
               -device virtio-tablet-pci,)

# USB. A BUS axis rather than a value of INPUT=, because USB is not an
# input class -- mass storage is the next thing to arrive on it, and an
# `INPUT=usb` would have to be renamed the day it does. Named rather
# than a boolean for the reason the other axes are: `USB=xhci+mouse` is
# a third value, and it has to be separate because attaching a usb-mouse
# changes where QMP pointer events go the moment the guest driver polls
# that endpoint.
#
# ATTACHING THIS TAKES THE KEYBOARD AWAY FROM PS/2. QEMU activates a
# keyboard handler when usb-kbd appears and routes keystrokes to it, so
# a guest whose USB driver is not working receives NOTHING -- measured,
# not assumed (see tools/usb_test.py, whose control this property is).
# That is exactly why it is off by default: adding it to an existing
# test would silently kill that test's keyboard input.
#
# Devices carry ids so a test can aim `input-send-event` at one by name.
USB_KIND = $(if $(USB),$(USB),none)
# Every comma inside a device string has to be $(COMMA): make splits
# $(if)'s arguments on commas, so a literal one silently truncates the
# device list -- which is how `usb-kbd` disappeared entirely the first
# time this was written, leaving a controller with nothing plugged in.
QEMU_USB = $(if $(filter xhci xhci+mouse xhci+hub,$(USB_KIND)),\
             -device qemu-xhci$(COMMA)id=xhci,)\
           $(if $(filter xhci xhci+mouse,$(USB_KIND)),\
             -device usb-kbd$(COMMA)id=usbkbd$(COMMA)bus=xhci.0,)\
           $(if $(filter xhci+mouse,$(USB_KIND)),\
             -device usb-mouse$(COMMA)id=usbmouse$(COMMA)bus=xhci.0,)\
           $(if $(filter xhci+hub,$(USB_KIND)),\
             -device usb-hub$(COMMA)id=usbhub$(COMMA)port=1$(COMMA)bus=xhci.0 \
             -device usb-kbd$(COMMA)id=usbkbd$(COMMA)bus=xhci.0$(COMMA)port=1.1 \
             -device usb-mouse$(COMMA)id=usbmouse$(COMMA)bus=xhci.0$(COMMA)port=1.2,)

# Networking. A NIC and a user-mode (SLIRP) network behind it.
#
# `NET=e1000` IS WHAT QEMU ALREADY DID IMPLICITLY -- the default pc
# machine attaches an 8086:100E with user networking when no -net/-netdev
# option is given, which is why the e1000 driver needs no flag and why
# every guest in the suite has had an unclaimed NIC on the bus for as
# long as this project has existed. Naming it changes nothing about the
# machine and makes the other three values expressible.
#
# `NET=both` is the multi-NIC configuration, and it is the shape no other
# test here boots -- the same reason tools/multidisk_test.py exists. Both
# cards are leased an address by /bin/dhcp at boot, each from its own
# SLIRP network.
#
# `NET=quiet` IS A SEGMENT WITH NOBODY ON IT -- a socket netdev listening
# for a peer that never connects. It is the one network SLIRP cannot be,
# because SLIRP always answers DHCP, and it is therefore the only way to
# SEE the link-local fallback (RFC 3927) that /bin/dhcp reaches for when
# nothing offers a lease. `NET=none` is a different thing: no card at all.
#
# On a SLIRP network the guest is leased 10.0.2.15, the gateway and DNS
# are 10.0.2.2 and 10.0.2.3; SLIRP answers ICMP to the gateway itself,
# which is what `ping 10.0.2.2` proves. Commas are $(COMMA) for the
# reason QEMU_USB documents above.
NET_KIND = $(if $(NET),$(NET),e1000)
QEMU_NET = $(if $(filter none,$(NET_KIND)),-nic none,\
             $(if $(filter quiet,$(NET_KIND)),\
               -netdev socket$(COMMA)id=n0$(COMMA)listen=127.0.0.1:14899 -device e1000$(COMMA)netdev=n0,\
             $(if $(filter e1000 both,$(NET_KIND)),\
               -netdev user$(COMMA)id=n0 -device e1000$(COMMA)netdev=n0,)\
             $(if $(filter virtio both,$(NET_KIND)),\
               -netdev user$(COMMA)id=n1 -device virtio-net-pci$(COMMA)netdev=n1$(COMMA)disable-legacy=on,)))

# QEMU has had no default audio backend since 5.x, and -machine
# pcspk-audiodev is what routes the emulated i8254 speaker to it.
# Without both, `beep` runs correctly and is simply silent. `pa` is what
# this was confirmed with; AUDIODEV= overrides it for an alsa/coreaudio
# host (`qemu-system-x86_64 -audiodev help` lists them).
AUDIODEV ?= pa
# A NAME rather than a boolean, for the reason DISK/VGA/INPUT are:
# `AUDIO=usb` is a third value. `AUDIO=1` still means the AC'97, so
# every existing invocation is unchanged.
AUDIO_KIND = $(if $(filter usb both,$(AUDIO)),$(AUDIO),$(if $(AUDIO),ac97,none))
# USB audio needs a controller. Derived rather than made the caller's
# problem -- and skipped when USB= already attached one, since a second
# `-device qemu-xhci,id=xhci` is a duplicate-id error, not a second bus.
# $(strip): the value carries the WHITESPACE that lines this up, and an
# expansion that is only spaces is still non-empty to $(if) -- make
# strips a condition's literal whitespace, never its expansion's.
# Without it the controller is attached twice and QEMU refuses the
# duplicate id.
AUDIO_XHCI = $(strip $(if $(filter usb both,$(AUDIO_KIND)),$(if $(filter xhci xhci+mouse xhci+hub,$(USB_KIND)),,yes),))
QEMU_AUDIO = $(if $(filter-out none,$(AUDIO_KIND)),               -audiodev $(AUDIODEV)$(COMMA)id=snd0 -machine pcspk-audiodev=snd0                $(if $(AUDIO_XHCI),-device qemu-xhci$(COMMA)id=xhci,)               $(if $(filter ac97 both,$(AUDIO_KIND)),                 -device AC97$(COMMA)audiodev=snd0,)               $(if $(filter usb both,$(AUDIO_KIND)),                 -device usb-audio$(COMMA)id=usbaud$(COMMA)bus=xhci.0$(COMMA)audiodev=snd0,),)

QEMU_EXTRA =

# WHICH MEDIUM BOOTS, and why this is derived rather than fixed.
#
# The disk is the boot medium now: disk.img carries GRUB and the kernel
# (tools/install_grub.py), so `-boot order=c` boots the machine the way
# a real one boots. The ISO is still a boot medium -- it is what the
# live and demo images ARE, and what a release ships -- so this is a
# choice rather than a replacement.
#
# It is DERIVED because an image built before the boot partition existed
# has no GRUB on it and must still work: is_bootable() asks the image,
# and `BOOT=disk`/`BOOT=cd` overrides the answer. Getting this wrong in
# the CD direction costs a slower boot; getting it wrong in the DISK
# direction hangs with no output at all, because SeaBIOS checks only for
# 0x55AA at LBA 0 and a protective MBR has one -- it then jumps into 446
# bytes of table as if they were boot code. That is why "it has a
# partition table" is exactly the wrong test, and why the predicate
# looks for GRUB's own stamp instead.
BOOT ?= auto
DISK_BOOTABLE = $(shell python3 tools/install_grub.py $(DISK_IMG) --check >/dev/null 2>&1 && echo 1)
BOOT_MEDIUM = $(if $(NODISK)$(LIVE)$(DEMO),cd,\
                $(if $(filter disk,$(BOOT)),disk,\
                  $(if $(filter cd,$(BOOT)),cd,\
                    $(if $(DISK_BOOTABLE),disk,cd))))
QEMU_BOOT = $(if $(filter disk,$(BOOT_MEDIUM)),-boot order=c,-boot order=d -cdrom $(QEMU_ISO))

QEMU_RUN = qemu-system-x86_64 $(QEMU_BOOT) $(QEMU_ACCEL) $(QEMU_DISK) \
	  $(QEMU_INPUT) $(QEMU_USB) \
	  -serial stdio -vga $(QEMU_VGA) -display $(QEMU_DISPLAY) $(QEMU_FULLSCREEN) -m $(MEM) \
	  $(QEMU_AUDIO) $(QEMU_NET) $(QEMU_EXTRA)

run: $(RUN_PREREQ)
	$(QEMU_RUN)

# WHAT EACH AXIS IS FOR, since the targets that used to carry this are
# gone and the knowledge is not obvious from a variable name.
#
# VGA -- WHICH ADAPTER, and therefore WHAT SCREEN SIZE IS REACHABLE.
#   All three have a modesetting driver now, so `video=<W>x<H>` baked in
#   with KCMDLINE (docs/boot-flags.md) is honoured whichever you pick:
#   `std` through kernel/drivers/display/bochs.c (the Bochs DISPI
#   register window, which QEMU's stdvga implements), `vmware` through
#   vmsvga.c, `virtio` through the virtio-gpu driver. Before bochs
#   existed `std` had none, so the flag was inert on the default -- and
#   the default is what every headless test boots.
#
#   A guest bigger than 1920x1080 gets a bigger DESKTOP and not a bigger
#   WINDOW: WIN_CLIENT_MAX_W/H (kernel/include/abi/win_proto.h) caps a
#   client's buffer at 1080p, because the window server allocates those
#   pixels contiguously. See docs/roadmap.md's growable client buffers.
#
# VGA=vmware -- the VMware SVGA II adapter, which has a HARDWARE MOUSE
#   CURSOR: kernel/drivers/vmsvga.c detects it, takes the display over
#   from GRUB's VBE mode and hands the cursor to the adapter, so the WM
#   stops drawing one. The default `std` has no cursor hardware at all
#   (plain VGA's only cursor is the text-mode underline). NOT the
#   default for two reasons: it is emulator-only (a real machine needs a
#   real GPU driver), and a hardware cursor is composited by the display
#   frontend rather than living in the framebuffer -- so QEMU's
#   `screendump` does NOT capture it, and every screenshot-based test
#   would stop seeing the pointer. Leave it off for anything you intend
#   to screenshot.
#
# VIRTIO=1 -- virtio for EVERY device class at once: DISK, VGA and INPUT.
#   A per-class value still wins over it, so `VIRTIO=1 VGA=std` is a
#   legal and meaningful thing to ask for.
#
#   **IT USED TO MEAN THE DISK ALONE**, and that was the whole problem:
#   the name reads as a general statement and was not one, so the
#   `make run KVM=1 VIRTIO=1` everybody reached for still booted
#   `-vga std` and never once exercised the virtio-gpu driver. A flag
#   whose name is broader than its effect is a flag people will misread,
#   and they did.
#
#   Each class picks its implementation BY NAME rather than by a boolean
#   (DISK=ide|virtio, VGA=std|virtio|vmware, INPUT=ps2|virtio), because a
#   boolean cannot express a third one and this machine is going to grow
#   them -- NVMe is a roadmap item, and an `NVME=1` sitting beside a
#   `VIRTIO=1` would immediately raise "what does setting both mean?".
#
# DISK=virtio -- the disk on virtio-blk and NO IDE controller at all,
#   which is the point rather than a detail: ata_init() then finds nothing, so
#   the filesystem mounts only if the whole virtio path works (PCI
#   capability walk, 64-bit BAR, feature negotiation, the virtqueue, the
#   block adapter). `dmesg` should say "block: virtio-blk active" and
#   `df` should report a persistent TFS3. Not the default because ATA
#   needs to stay the exercised path for an interactive run.
#   disable-legacy=on asks for a MODERN device (1af4:1042); drop it, or
#   use `-drive file=...,if=virtio`, for a TRANSITIONAL one (1af4:1001)
#   -- the driver handles both, and transitional is QEMU's default, so
#   it is worth exercising too.
#
# KVM=1 -- hardware virtualization instead of TCG software emulation;
#   guest instructions run natively. Needs /dev/kvm readable (usually
#   membership of the `kvm` group), which is why plain `make run` stays
#   the portable default. `-cpu host` is what makes it worth having:
#   without it QEMU masks the guest down to a conservative model.
#   NOT a uniform speedup -- compute-bound guest code gets much faster,
#   but every port-I/O instruction becomes a hardware VM exit costing
#   ~a microsecond where TCG services one in-process in tens of
#   nanoseconds, so the PIO disk path and other inb/outb-heavy loops can
#   get SLOWER. Any throughput figure in the git history should say
#   which of the two it came from; they are not comparable.
#
# AUDIO=1 -- a PulseAudio backend wired to the PC speaker
#   (kernel/drivers/speaker.c's `beep`). QEMU has had no default audio
#   backend since 5.x and -machine pcspk-audiodev=<id> is what routes
#   the emulated i8254 speaker to it; without both, `beep` runs
#   correctly (the PIT/port-0x61 programming completes fine) and is
#   simply silent. `pa` is what this was confirmed with -- set
#   AUDIODEV=alsa/coreaudio for a host that has something else
#   (`qemu-system-x86_64 -audiodev help` lists them).
#
# MENU=1 -- GRUB's menu, with 5 seconds to choose. It rebuilds the ISO,
#   because the timeout is baked into grub.cfg at ISO build time.

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
debug: QEMU_EXTRA=-s -S
debug: run

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
	rm -rf $(BUILD) $(ISO) $(LIVE_ISO) $(DEMO_ISO) $(USB_IMG) iso/boot/kernel.bin iso/boot/live.img iso-live iso-demo $(SEED_DIR)/sync
	# Deliberately NOT touching $(DISK_IMG) here -- see its comment above.
	# Use `make clean-disk` to explicitly wipe the persistent filesystem.

clean-disk:
	rm -f $(DISK_IMG)
