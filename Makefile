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
# and kernel/include/version.h's generation (tools/gen_version.sh) for
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
# kernel/core/stack_protector.c) instead of the default `tls` guard --
# GCC's default reads the canary via %fs:0x28, and this kernel never
# sets up a per-CPU/per-thread FS/GS base (no TLS infrastructure
# exists at all, see docs/decisions.md), so the TLS-based default
# would dereference an unconfigured segment. `strong` (not plain
# `-fstack-protector`, and not `-all`) instruments any function with a
# local array or a local whose address is taken -- the same middle
# ground GCC itself recommends over the two extremes.
CFLAGS = -std=gnu11 -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie \
         -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
         -Wall -Wextra -O2 -g -c -Ikernel/include -Iapps -MMD -MP

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
# multi-gigabyte files (see kernel/drivers/tfs.c's FS_DISK_TOTAL_BYTES,
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
USERLAND_CFLAGS = -std=gnu11 -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie \
                   -mno-red-zone -mcmodel=large -mno-mmx -mno-sse -mno-sse2 \
                   -Wall -Wextra -O2 -g -c -Ikernel/include -MMD -MP
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

# Seed directory for tools/tfs2_writer.py's `sync` command -- see the
# `seed` target below and docs/decisions.md. Not committed as a
# generic directory: SEED_DIR/sync/bin/* are build-generated copies of
# each SEED_BINARIES entry, staged fresh by the `seed` target's own
# recipe every build, not tracked source files.
SEED_DIR = seed

# Source layout:
#   kernel/core/boot.asm, isr.asm  -- boot + interrupt stubs (assembly)
#   kernel/core/*.c                -- hardware bring-up, IDT/PIC, kernel_main
#   kernel/drivers/*.c             -- device drivers (console, gfx, keyboard, mouse, fs)
#   kernel/include/*.h             -- all headers, incl. kapi.h (the apps-facing API)
#   apps/*.c                       -- programs (shell, gui, ...) + the app registry
CORE_C    = $(wildcard kernel/core/*.c)
DRIVERS_C = $(wildcard kernel/drivers/*.c)
APPS_C    = $(wildcard apps/*.c)
# apps/wm/ holds the window manager split across a few files (wm.c,
# wm_input.c, wm_render.c -- see apps/wm/wm.c's top comment); its own
# wildcard since APPS_C's is non-recursive and won't see into subdirs.
WM_C      = $(wildcard apps/wm/*.c)
# apps/ui/ holds the retained-widget-object library (ui_button.c,
# ui_button_group.c, ui_textbox.c -- see apps/ui/ui.h's top comment);
# same reasoning as WM_C above -- APPS_C's wildcard is non-recursive.
UI_C      = $(wildcard apps/ui/*.c)
C_SOURCES = $(CORE_C) $(DRIVERS_C) $(APPS_C) $(WM_C) $(UI_C)

CORE_OBJ    = $(patsubst kernel/core/%.c,    $(BUILD)/core/%.o,    $(CORE_C))
DRIVERS_OBJ = $(patsubst kernel/drivers/%.c, $(BUILD)/drivers/%.o, $(DRIVERS_C))
APPS_OBJ    = $(patsubst apps/%.c,           $(BUILD)/apps/%.o,    $(APPS_C))
WM_OBJ      = $(patsubst apps/wm/%.c,        $(BUILD)/apps/wm/%.o, $(WM_C))
UI_OBJ      = $(patsubst apps/ui/%.c,        $(BUILD)/apps/ui/%.o, $(UI_C))
C_OBJECTS   = $(CORE_OBJ) $(DRIVERS_OBJ) $(APPS_OBJ) $(WM_OBJ) $(UI_OBJ)

ASM_OBJECTS = $(BUILD)/core/boot.o $(BUILD)/core/isr.o $(BUILD)/core/context_switch.o

.PHONY: all clean clean-disk iso run run-audio run-nographic debug help version seed

# Regenerates kernel/include/version.h from VERSION (see
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

all: version $(KERNEL) $(HELLO_ELF) $(EXIT_TEST_ELF) $(WRITE_TEST_ELF) $(WRITE_BAD_TEST_ELF) $(GUI_TEST_ELF) $(COUNTER_A_ELF) $(COUNTER_B_ELF) $(ECHO_ELF) $(WIN_TEST_ELF) $(FILE_TEST_ELF) $(NEWSYSCALLS_TEST_ELF) $(CRASH_TEST_ELF) $(SOCKET_TEST_ELF) $(LSPCI_ELF) $(LS_ELF) $(STACK_SMASH_TEST_ELF) $(NX_TEST_ELF)

help:
	@echo "toy-os -- available targets:"
	@echo "  all            Build kernel.bin and the userland test ELFs (default)"
	@echo "  iso            Build toy-os.iso, a bootable GRUB ISO (implies all)"
	@echo "  run            Boot toy-os.iso in QEMU with an SDL window (implies iso)"
	@echo "  run-audio      Same as run, plus a PulseAudio backend so the PC speaker"
	@echo "                 (beep) is actually audible -- see the Makefile for how to"
	@echo "                 swap the backend if you're not on PulseAudio"
	@echo "  run-nographic  Boot toy-os.iso in QEMU with no display, serial only (implies iso)"
	@echo "  debug          Boot toy-os.iso frozen (QEMU's -s -S) for real GDB"
	@echo "                 debugging -- attach with: gdb build/kernel.bin -ex"
	@echo "                 'target remote localhost:1234', then continue"
	@echo "  clean          Remove build outputs (build/, ELFs, toy-os.iso) -- leaves disk.img alone"
	@echo "  clean-disk     Wipe disk.img, the persistent filesystem -- use with care"
	@echo "  version        Regenerate kernel/include/version.h (runs automatically as part of all/iso)"
	@echo "  help           Show this message"

$(BUILD)/core $(BUILD)/drivers $(BUILD)/apps $(BUILD)/apps/wm $(BUILD)/apps/ui:
	mkdir -p $@

$(BUILD)/core/boot.o: kernel/core/boot.asm | $(BUILD)/core
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD)/core/isr.o: kernel/core/isr.asm | $(BUILD)/core
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD)/core/context_switch.o: kernel/core/context_switch.asm | $(BUILD)/core
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD)/core/%.o: kernel/core/%.c | $(BUILD)/core
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/drivers/%.o: kernel/drivers/%.c | $(BUILD)/drivers
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/apps/%.o: apps/%.c | $(BUILD)/apps
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/apps/wm/%.o: apps/wm/%.c | $(BUILD)/apps/wm
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/apps/ui/%.o: apps/ui/%.c | $(BUILD)/apps/ui
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userland:
	mkdir -p $@

# __stack_chk_guard/__stack_chk_fail -- linked into every userland ELF
# below (see USERLAND_CFLAGS's comment above).
$(BUILD)/userland/stack_chk.o: userland/stack_chk.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(BUILD)/userland/hello.o: userland/hello.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(HELLO_ELF): $(BUILD)/userland/hello.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/hello.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/exit_test.o: userland/exit_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(EXIT_TEST_ELF): $(BUILD)/userland/exit_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/exit_test.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/write_test.o: userland/write_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(WRITE_TEST_ELF): $(BUILD)/userland/write_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/write_test.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/write_bad_test.o: userland/write_bad_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(WRITE_BAD_TEST_ELF): $(BUILD)/userland/write_bad_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/write_bad_test.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/gui_test.o: userland/gui_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(GUI_TEST_ELF): $(BUILD)/userland/gui_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/gui_test.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/counter_a.o: userland/counter_a.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(COUNTER_A_ELF): $(BUILD)/userland/counter_a.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/counter_a.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/counter_b.o: userland/counter_b.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(COUNTER_B_ELF): $(BUILD)/userland/counter_b.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/counter_b.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/echo.o: userland/echo.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(ECHO_ELF): $(BUILD)/userland/echo.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/echo.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/win_test.o: userland/win_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(WIN_TEST_ELF): $(BUILD)/userland/win_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/win_test.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/file_test.o: userland/file_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(FILE_TEST_ELF): $(BUILD)/userland/file_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/file_test.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/newsyscalls_test.o: userland/newsyscalls_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(NEWSYSCALLS_TEST_ELF): $(BUILD)/userland/newsyscalls_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/newsyscalls_test.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/crash_test.o: userland/crash_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(CRASH_TEST_ELF): $(BUILD)/userland/crash_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/crash_test.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/socket_test.o: userland/socket_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(SOCKET_TEST_ELF): $(BUILD)/userland/socket_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/socket_test.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/lspci.o: userland/lspci.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(LSPCI_ELF): $(BUILD)/userland/lspci.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/lspci.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/ls.o: userland/ls.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(LS_ELF): $(BUILD)/userland/ls.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/ls.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/stack_smash_test.o: userland/stack_smash_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(STACK_SMASH_TEST_ELF): $(BUILD)/userland/stack_smash_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/stack_smash_test.o $(BUILD)/userland/stack_chk.o

$(BUILD)/userland/nx_test.o: userland/nx_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(NX_TEST_ELF): $(BUILD)/userland/nx_test.o $(BUILD)/userland/stack_chk.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/nx_test.o $(BUILD)/userland/stack_chk.o

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
SEED_BINARIES = \
	$(LSPCI_ELF):lspci \
	$(LS_ELF):ls \
	$(HELLO_ELF):hello \
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
	$(NX_TEST_ELF):nx_test

# Seeds $(DISK_IMG) with every SEED_BINARIES entry, plus the /etc/kbs/*
# keyboard-layout data files, via tools/tfs2_writer.py's `sync` (see
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
seed: $(DISK_IMG) $(LSPCI_ELF) $(LS_ELF) $(HELLO_ELF) $(EXIT_TEST_ELF) $(WRITE_TEST_ELF) $(WRITE_BAD_TEST_ELF) $(GUI_TEST_ELF) $(COUNTER_A_ELF) $(COUNTER_B_ELF) $(ECHO_ELF) $(WIN_TEST_ELF) $(FILE_TEST_ELF) $(NEWSYSCALLS_TEST_ELF) $(CRASH_TEST_ELF) $(SOCKET_TEST_ELF) $(STACK_SMASH_TEST_ELF) $(NX_TEST_ELF)
	mkdir -p $(SEED_DIR)/sync/bin
	$(foreach pair,$(SEED_BINARIES),cp $(word 1,$(subst :, ,$(pair))) $(SEED_DIR)/sync/bin/$(word 2,$(subst :, ,$(pair)));)
	@if command -v xkbcli >/dev/null 2>&1; then \
		python3 tools/gen_kbs.py us --write; \
		python3 tools/gen_kbs.py se --write; \
	else \
		echo "seed: xkbcli not found -- skipping /etc/kbs regeneration (apt-get install libxkbcommon-tools to enable; kernel falls back to compiled-in US regardless)"; \
	fi
	python3 tools/tfs2_writer.py sync $(DISK_IMG) $(SEED_DIR)

iso: version $(KERNEL) $(HELLO_ELF) $(EXIT_TEST_ELF) $(WRITE_TEST_ELF) $(WRITE_BAD_TEST_ELF) $(GUI_TEST_ELF) $(COUNTER_A_ELF) $(COUNTER_B_ELF) $(ECHO_ELF) $(WIN_TEST_ELF) $(FILE_TEST_ELF) $(NEWSYSCALLS_TEST_ELF) $(CRASH_TEST_ELF) $(SOCKET_TEST_ELF) $(LSPCI_ELF) $(LS_ELF) $(STACK_SMASH_TEST_ELF) $(NX_TEST_ELF) seed
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/kernel.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso

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
run: iso $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -vga std -display sdl,grab-mod=rctrl -m 256

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
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -vga std -display sdl,grab-mod=rctrl -m 256 -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

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
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -vga std -display sdl,grab-mod=rctrl -m 256 -s -S

run-nographic: iso $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -display none -m 256

clean:
	rm -rf $(BUILD) $(ISO) iso/boot/kernel.bin $(HELLO_ELF) $(EXIT_TEST_ELF) $(WRITE_TEST_ELF) $(WRITE_BAD_TEST_ELF) $(GUI_TEST_ELF) $(COUNTER_A_ELF) $(COUNTER_B_ELF) $(ECHO_ELF) $(WIN_TEST_ELF) $(FILE_TEST_ELF) $(NEWSYSCALLS_TEST_ELF) $(CRASH_TEST_ELF) $(SOCKET_TEST_ELF) $(LSPCI_ELF) $(LS_ELF) $(STACK_SMASH_TEST_ELF) $(NX_TEST_ELF) $(SEED_DIR)/sync
	# Deliberately NOT touching $(DISK_IMG) here -- see its comment above.
	# Use `make clean-disk` to explicitly wipe the persistent filesystem.

clean-disk:
	rm -f $(DISK_IMG)
