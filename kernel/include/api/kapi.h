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
#include "keyboard.h"  // keyboard_getchar, keyboard_try_getchar, KEY_* codes, IS_PRINTABLE_KEY, Nordic chars
#include "keyboard_layout.h" // keyboard_layout_load/current/translate -- data-driven /etc/kbs/<name> layouts (see kernel/lib/keyboard_layout.c)
#include "keyboard_config.h" // keyboard layout persistence (see kernel/lib/keyboard_config.c)
#include "mouse.h"     // mouse_init, mouse_get_state, mouse_set_bounds
#include "timer.h"     // pit_ticks, rtc_read
#include "tz.h"        // rtc_read_local, timezone selection (see kernel/lib/tz.c)
#include "font_config.h" // font size persistence (see kernel/lib/font_config.c)
#include "font_face.h"   // fonts loaded from /usr/share/fonts at runtime
#include "cursor_config.h" // console cursor-style persistence (see kernel/lib/cursor_config.c)
#include "etc_config.h" // shared /etc/*.conf name=value reader/writer (see kernel/lib/etc_config.c) -- for an app's own /etc/<name>.conf, not just the kernel-internal settings above that already wrap it
#include "fs.h"        // the filesystem (backend-agnostic API -- see fs.h's top comment)
#include "json.h"      // heap-backed JSON parser/serializer -- see json.h's top comment (coexists with etc_config.h's flat name=value format)
#include "klog.h"      // klog_dump -- the kernel's in-memory log, what `dmesg` reads (see klog.c)
#include "multiboot.h" // multiboot_print_meminfo
#include "pmm.h"       // pmm_total_frames/pmm_free_frames -- physical frame allocator stats
#include "heap.h"      // kmalloc/kzalloc/kfree -- kernel-space heap (see heap.h's top comment)
#include "power.h"     // system_reboot, system_poweroff
#include "speaker.h"   // speaker_beep -- PC speaker (PIT channel 2 + port 0x61), see kernel/drivers/speaker.c
#include "pci.h"       // PCI config-space enumeration -- pci_init/pci_device_at/pci_class_name (see pci.c)
#include "cpuinfo.h"   // CPU identity/features, and which of them this kernel enabled (see cpuid.c).
                        // NOT api/cpu_features.h -- that's the ~90-entry name table, included only by
                        // the two files that print it; see its own top comment.
#include "partition.h" // MBR/GPT partition table parsing -- partition_read_table (see kernel/drivers/partition.c)
#include "ata.h"       // ata_dma_nonblocking_selftest -- diagnostic only (the shell's `dmatest`); fs.h is the real disk-I/O surface apps should use otherwise
#include "ring3_test.h" // ring3_test_run: paging/GDT/ring-3 isolation demo (the one test that predates and doesn't use any ELF file at all)
#include "scheduler.h"    // scheduler_demo_run (M16 demo), scheduler_spawn/scheduler_poll (Milestone 1 phase 4b -- non-blocking process spawn, userland/wm/wm.c's window_start_process()/apps/terminal.c's async run/ls)
#include "elf_run.h"       // elf_run_from_fs: loads and runs a real ELF64 binary straight from the persistent filesystem -- what every /bin binary now runs through via the shell's `run <name>` (see docs/decisions.md; this replaced a dozen near-identical per-binary GRUB-module test harnesses)
#include "debug_console.h" // debug_console_poll -- userland/wm/wm.c's event loop rides this the same way keyboard_getchar() does, so the serial debug console stays responsive while the GUI desktop is up too (see docs/decisions.md)
#include "ktest_run.h" // ktest_run_all -- the shell's `ktest` command; writing tests needs kernel/ktest.h, see that header
#include "version.h"   // TOYOS_VERSION -- build number, shown by the shell's `about` and the GUI About window
#include "string.h"    // k_strlen, k_strcmp, k_strchr/strstr/strlcpy, char classes -- freestanding, no libc
#include "knum.h"      // numbers <-> strings: k_utoa/k_itoa/k_htoa, k_parse_* (see knum.h's top comment on the nine copies it replaced)
#include "kfmt.h"      // k_snprintf + vga_printf/klog_printf -- one bounded formatter instead of six calls per line
#include "kpath.h"     // path join/normalize/resolve/basename/dirname -- one implementation, shared by the shell and the Terminal
#include "klineedit.h" // readline-style line editing (buffer/cursor/kill ring/undo), shared by the shell and the GUI Terminal
#include "debugflags.h" // dbgflag_enabled/set/parse -- per-subsystem runtime debug-log toggles (see the shell's `debug` command)
#include "krandom.h"   // krandom_u64/krandom_bytes + krandom_quality -- the kernel's entropy source (RDSEED/RDRAND, TSC jitter otherwise). Deliberately NOT a CSPRNG; the quality enum is how a caller finds that out (see krandom.h)

#endif
