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
#include "keyboard.h"  // keyboard_getchar, keyboard_try_getchar, KEY_* codes
#include "mouse.h"     // mouse_init, mouse_get_state, mouse_set_bounds
#include "timer.h"     // pit_ticks, rtc_read
#include "tz.h"        // rtc_read_local, timezone selection (see kernel/core/tz.c)
#include "font_config.h" // font size persistence (see kernel/core/font_config.c)
#include "fs.h"        // the filesystem (backend-agnostic API -- see fs.h's top comment)
#include "klog.h"      // klog_dump -- the kernel's in-memory log, what `dmesg` reads (see klog.c)
#include "multiboot.h" // multiboot_print_meminfo
#include "pmm.h"       // pmm_total_frames/pmm_free_frames -- physical frame allocator stats
#include "power.h"     // system_reboot
#include "pci.h"       // PCI config-space enumeration -- pci_init/pci_device_at/pci_class_name (see pci.c)
#include "ring3_test.h" // ring3_test_run: paging/GDT/ring-3 isolation demo
#include "elf_test.h"   // elf_test_run: loads and runs a real ELF64 binary in ring 3
#include "syscall_test.h" // syscall_test_run: real syscall round-trip -- runs a process and RETURNS
#include "write_test.h"   // write_test_run: real write syscall -- process prints its own output
#include "ptr_test.h"     // ptr_test_run: proves write's pointer validation actually rejects bad pointers
#include "gui_test.h"     // gui_test_run: modal ring-3 process draws directly to the real screen
#include "scheduler.h"    // scheduler_demo_run: preemptive round-robin scheduler demo (M16)
#include "echo_test.h"    // echo_test_run: interactive ring-3 program using SYS_READ_KEY + SYS_SBRK
#include "win_test.h"     // win_test_run: ring-3 process with its own private, kernel-composited window
#include "file_test.h"    // file_test_run: ring-3 process round-trips a real file through SYS_OPEN/READ/WRITE/CLOSE
#include "newsyscalls_test.h" // newsyscalls_test_run: ring-3 process exercises SYS_UNLINK/SYS_LISTDIR/SYS_GETTIME/SYS_YIELD
#include "crash_test.h"    // crash_test_run: ring-3 process deliberately faults, proving the kernel recovers instead of halting
#include "socket_test.h"   // socket_test_run: ring-3 process exercises SYS_SOCKET/SYS_SEND/SYS_RECV (fd/syscall surface, no transport yet)
#include "version.h"   // TOYOS_VERSION -- build number, shown by the shell's `about` and the GUI About window
#include "string.h"    // k_strlen, k_strcmp, etc -- freestanding, no libc

#endif
