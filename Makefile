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
CFLAGS = -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
         -Wall -Wextra -O2 -c -Ikernel/include -Iapps -MMD -MP

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
# next time, exactly the point of it existing. 1MiB comfortably fits the
# current on-disk layout (1 + 16*5 = 81 sectors, ~40KB -- see fs.c) with
# room to spare.
DISK_IMG = disk.img

# userland test programs (see userland/README or kernel's "Process
# isolation" README section) -- plain freestanding binaries, no
# kernel-specific flags like -mcmodel=kernel needed since these run in
# ordinary ring-3 user space, not as part of the kernel image.
# -mcmodel=large IS needed though: these link at VMM_USER_BASE
# (512GiB), and the default code model can't reach a global (e.g. a
# string literal) from that address with a 32-bit relocation -- the
# linker fails with "relocation truncated to fit" without this.
USERLAND_CFLAGS = -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                   -mno-red-zone -mcmodel=large -mno-mmx -mno-sse -mno-sse2 \
                   -Wall -Wextra -O2 -c -Ikernel/include -MMD -MP
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
C_SOURCES = $(CORE_C) $(DRIVERS_C) $(APPS_C) $(WM_C)

CORE_OBJ    = $(patsubst kernel/core/%.c,    $(BUILD)/core/%.o,    $(CORE_C))
DRIVERS_OBJ = $(patsubst kernel/drivers/%.c, $(BUILD)/drivers/%.o, $(DRIVERS_C))
APPS_OBJ    = $(patsubst apps/%.c,           $(BUILD)/apps/%.o,    $(APPS_C))
WM_OBJ      = $(patsubst apps/wm/%.c,        $(BUILD)/apps/wm/%.o, $(WM_C))
C_OBJECTS   = $(CORE_OBJ) $(DRIVERS_OBJ) $(APPS_OBJ) $(WM_OBJ)

ASM_OBJECTS = $(BUILD)/core/boot.o $(BUILD)/core/isr.o $(BUILD)/core/context_switch.o

.PHONY: all clean clean-disk iso run run-nographic help version

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

all: version $(KERNEL) $(HELLO_ELF) $(EXIT_TEST_ELF) $(WRITE_TEST_ELF) $(WRITE_BAD_TEST_ELF) $(GUI_TEST_ELF) $(COUNTER_A_ELF) $(COUNTER_B_ELF) $(ECHO_ELF) $(WIN_TEST_ELF) $(FILE_TEST_ELF) $(NEWSYSCALLS_TEST_ELF) $(CRASH_TEST_ELF) $(SOCKET_TEST_ELF)

help:
	@echo "toy-os -- available targets:"
	@echo "  all            Build kernel.bin and the userland test ELFs (default)"
	@echo "  iso            Build toy-os.iso, a bootable GRUB ISO (implies all)"
	@echo "  run            Boot toy-os.iso in QEMU with a GTK window (implies iso)"
	@echo "  run-nographic  Boot toy-os.iso in QEMU with no display, serial only (implies iso)"
	@echo "  clean          Remove build outputs (build/, ELFs, toy-os.iso) -- leaves disk.img alone"
	@echo "  clean-disk     Wipe disk.img, the persistent filesystem -- use with care"
	@echo "  version        Regenerate kernel/include/version.h (runs automatically as part of all/iso)"
	@echo "  help           Show this message"

$(BUILD)/core $(BUILD)/drivers $(BUILD)/apps $(BUILD)/apps/wm:
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

$(BUILD)/userland:
	mkdir -p $@

$(BUILD)/userland/hello.o: userland/hello.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(HELLO_ELF): $(BUILD)/userland/hello.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/hello.o

$(BUILD)/userland/exit_test.o: userland/exit_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(EXIT_TEST_ELF): $(BUILD)/userland/exit_test.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/exit_test.o

$(BUILD)/userland/write_test.o: userland/write_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(WRITE_TEST_ELF): $(BUILD)/userland/write_test.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/write_test.o

$(BUILD)/userland/write_bad_test.o: userland/write_bad_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(WRITE_BAD_TEST_ELF): $(BUILD)/userland/write_bad_test.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/write_bad_test.o

$(BUILD)/userland/gui_test.o: userland/gui_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(GUI_TEST_ELF): $(BUILD)/userland/gui_test.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/gui_test.o

$(BUILD)/userland/counter_a.o: userland/counter_a.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(COUNTER_A_ELF): $(BUILD)/userland/counter_a.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/counter_a.o

$(BUILD)/userland/counter_b.o: userland/counter_b.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(COUNTER_B_ELF): $(BUILD)/userland/counter_b.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/counter_b.o

$(BUILD)/userland/echo.o: userland/echo.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(ECHO_ELF): $(BUILD)/userland/echo.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/echo.o

$(BUILD)/userland/win_test.o: userland/win_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(WIN_TEST_ELF): $(BUILD)/userland/win_test.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/win_test.o

$(BUILD)/userland/file_test.o: userland/file_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(FILE_TEST_ELF): $(BUILD)/userland/file_test.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/file_test.o

$(BUILD)/userland/newsyscalls_test.o: userland/newsyscalls_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(NEWSYSCALLS_TEST_ELF): $(BUILD)/userland/newsyscalls_test.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/newsyscalls_test.o

$(BUILD)/userland/crash_test.o: userland/crash_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(CRASH_TEST_ELF): $(BUILD)/userland/crash_test.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/crash_test.o

$(BUILD)/userland/socket_test.o: userland/socket_test.c | $(BUILD)/userland
	$(CC) $(USERLAND_CFLAGS) $< -o $@

$(SOCKET_TEST_ELF): $(BUILD)/userland/socket_test.o userland/link.ld
	$(LD) -n -T userland/link.ld -nostdlib -o $@ $(BUILD)/userland/socket_test.o

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
-include $(wildcard $(BUILD)/core/*.d $(BUILD)/drivers/*.d $(BUILD)/apps/*.d $(BUILD)/apps/wm/*.d $(BUILD)/userland/*.d)

# Only created if it doesn't already exist -- see DISK_IMG's comment
# above for why this must never overwrite an existing image.
$(DISK_IMG):
	dd if=/dev/zero of=$@ bs=1M count=1 status=none

iso: version $(KERNEL) $(HELLO_ELF) $(EXIT_TEST_ELF) $(WRITE_TEST_ELF) $(WRITE_BAD_TEST_ELF) $(GUI_TEST_ELF) $(COUNTER_A_ELF) $(COUNTER_B_ELF) $(ECHO_ELF) $(WIN_TEST_ELF) $(FILE_TEST_ELF) $(NEWSYSCALLS_TEST_ELF) $(CRASH_TEST_ELF) $(SOCKET_TEST_ELF)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/kernel.bin
	cp $(HELLO_ELF) iso/boot/hello.elf
	cp $(EXIT_TEST_ELF) iso/boot/exit_test.elf
	cp $(WRITE_TEST_ELF) iso/boot/write_test.elf
	cp $(WRITE_BAD_TEST_ELF) iso/boot/write_bad_test.elf
	cp $(GUI_TEST_ELF) iso/boot/gui_test.elf
	cp $(COUNTER_A_ELF) iso/boot/counter_a.elf
	cp $(COUNTER_B_ELF) iso/boot/counter_b.elf
	cp $(ECHO_ELF) iso/boot/echo.elf
	cp $(WIN_TEST_ELF) iso/boot/win_test.elf
	cp $(FILE_TEST_ELF) iso/boot/file_test.elf
	cp $(NEWSYSCALLS_TEST_ELF) iso/boot/newsyscalls_test.elf
	cp $(CRASH_TEST_ELF) iso/boot/crash_test.elf
	cp $(SOCKET_TEST_ELF) iso/boot/socket_test.elf
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

run-nographic: iso $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -display none -m 256

clean:
	rm -rf $(BUILD) $(ISO) iso/boot/kernel.bin iso/boot/hello.elf iso/boot/exit_test.elf iso/boot/write_test.elf iso/boot/write_bad_test.elf iso/boot/gui_test.elf iso/boot/counter_a.elf iso/boot/counter_b.elf iso/boot/echo.elf iso/boot/win_test.elf iso/boot/file_test.elf iso/boot/newsyscalls_test.elf iso/boot/crash_test.elf iso/boot/socket_test.elf $(HELLO_ELF) $(EXIT_TEST_ELF) $(WRITE_TEST_ELF) $(WRITE_BAD_TEST_ELF) $(GUI_TEST_ELF) $(COUNTER_A_ELF) $(COUNTER_B_ELF) $(ECHO_ELF) $(WIN_TEST_ELF) $(FILE_TEST_ELF) $(NEWSYSCALLS_TEST_ELF) $(CRASH_TEST_ELF) $(SOCKET_TEST_ELF)
	# Deliberately NOT touching $(DISK_IMG) here -- see its comment above.
	# Use `make clean-disk` to explicitly wipe the persistent filesystem.

clean-disk:
	rm -f $(DISK_IMG)
