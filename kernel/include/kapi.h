#ifndef KAPI_H
#define KAPI_H

// This is the ONLY header apps/*.c should include to talk to the kernel.
// It just aggregates the driver headers that make up the current API
// surface -- if a driver gets rewritten or split up internally, this file
// (and only this file) needs updating, not every app.
//
// Rule of thumb: if you're writing code in apps/, #include "kapi.h" and
// nothing under kernel/drivers/ or kernel/core/ directly.

#include "vga.h"       // console: vga_write, vga_putc, vga_clear, vga_set_color...
#include "gfx.h"       // framebuffer graphics primitives
#include "keyboard.h"  // keyboard_getchar, keyboard_try_getchar, KEY_* codes, IS_PRINTABLE_KEY, Nordic layout
#include "keyboard_config.h" // keyboard layout persistence (see kernel/core/keyboard_config.c)
#include "mouse.h"     // mouse_init, mouse_get_state, mouse_set_bounds
#include "timer.h"     // pit_ticks, rtc_read
#include "tz.h"        // rtc_read_local, timezone selection (see kernel/core/tz.c)
#include "font_config.h" // font size persistence (see kernel/core/font_config.c)
#include "fs.h"        // the filesystem (backend-agnostic API -- see fs.h's top comment)
#include "json.h"      // heap-backed JSON parser/serializer -- see json.h's top comment (coexists with etc_config.h's flat name=value format)
#include "klog.h"      // klog_dump -- the kernel's in-memory log, what `dmesg` reads (see klog.c)
#include "multiboot.h" // multiboot_print_meminfo
#include "pmm.h"       // pmm_total_frames/pmm_free_frames -- physical frame allocator stats
#include "heap.h"      // kmalloc/kzalloc/kfree -- kernel-space heap (see heap.h's top comment)
#include "power.h"     // system_reboot
#include "pci.h"       // PCI config-space enumeration -- pci_init/pci_device_at/pci_class_name (see pci.c)
#include "ring3_test.h" // ring3_test_run: paging/GDT/ring-3 isolation demo (the one test that predates and doesn't use any ELF file at all)
#include "scheduler.h"    // scheduler_demo_run: preemptive round-robin scheduler demo (M16), spawns /bin/counter_a and /bin/counter_b from disk
#include "elf_run.h"       // elf_run_from_fs: loads and runs a real ELF64 binary straight from the persistent filesystem -- what every /bin binary now runs through via the shell's `run <name>` (see docs/decisions.md; this replaced a dozen near-identical per-binary GRUB-module test harnesses)
#include "version.h"   // TOYOS_VERSION -- build number, shown by the shell's `about` and the GUI About window
#include "string.h"    // k_strlen, k_strcmp, etc -- freestanding, no libc

#endif
