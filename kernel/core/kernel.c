// Kernel entry point. Does hardware bring-up only -- once interrupts and
// the drivers are up, it hands off to apps_start() and never looks at
// app-level code again. Compare with apps/apps.c, which knows nothing
// about hardware.
#include "vga.h"
#include "serial.h"
#include "klog.h"
#include "idt.h"
#include "gdt.h"
#include "multiboot.h"
#include "pmm.h"
#include "heap.h"
#include "pci.h"
#include "fs.h"
#include "json.h"
#include "tz.h"
#include "timer.h"
#include "font_config.h"
#include "keyboard_config.h"
#include "apps.h"
#include "scheduler.h"
#include "debug_console.h"
#include "knum.h"
#include <stdint.h>

// Zero-padded 2-digit decimal, for the RTC boot-time log line below.
// klog_write_dec() (klog.h) deliberately doesn't pad, so this used to
// be a hand-rolled two-character helper; knum.h has the padded
// converter now, and klog.c's own "[secs.hh]" prefix -- which this
// helper's comment used to point at as the other copy -- goes through
// the same one.
static void klog_write_dec2(uint8_t n) {
    char buf[8];
    k_utoa_pad(n % 100, buf, sizeof buf, 2);
    klog_write(buf);
}

void kernel_main(uint64_t multiboot_info_addr) {
    serial_init();
    klog_write("toy-os: kernel_main reached, initializing...\n");

    multiboot_set_info(multiboot_info_addr);

    vga_init();
    // Mirror the kernel log to the screen for the rest of boot, the way
    // a real kernel shows its init sequence. Switched off again just
    // before apps_start() below -- and the console's scrollback (see
    // vga.h) keeps these lines readable with PageUp once the shell is
    // up, which is the whole point: they used to go past far too fast
    // to read and only existed on the serial line afterwards.
    klog_set_console_echo(1);
    vga_write("toy-os booting...\n");

    gdt_init();
    klog_write("toy-os: GDT/TSS initialized\n");

    idt_init();
    klog_write("toy-os: IDT/PIC/PIT initialized, interrupts enabled\n");

    serial_irq_init(); // COM1 RX -- see serial.c for why this can't run inside serial_init() itself
    klog_write("toy-os: serial RX enabled (debug console on COM1, see docs/decisions.md)\n");

    pmm_init();
    klog_write("toy-os: physical frame allocator initialized\n");

    heap_init(); // kmalloc()/kfree() -- built on pmm, needs it initialized first
    klog_write("toy-os: kernel heap initialized\n");

    // No self-tests run here any more. pmm/heap/json/tfs each used to be
    // checked on every single boot -- see kernel/include/kernel/ktest.h
    // for why that stopped being a good idea and `ktest` for how to run
    // them now.

    pci_init(); // brute-force config-space scan -- see pci.h's top comment
    klog_write("toy-os: PCI bus enumerated\n");

    fs_init();
    fs_mkdir("/etc"); // config-file convention (see tz.c) -- a no-op if it already exists
    // /bin binaries (e.g. lspci) are no longer bootstrap-installed here
    // at boot time -- tools/tfs2_writer.py seeds them into disk.img at
    // BUILD time now (see the Makefile's `seed` step), so by the time
    // toy-os actually boots they're already on disk. See
    // docs/decisions.md for why this replaced the old GRUB-module/
    // BIN_BOOTSTRAP-table approach.
    tz_init(); // loads the persisted timezone choice, if any -- needs fs_init()/"/etc" first
    font_config_init(); // loads the persisted font size, if any -- see kernel/lib/font_config.c
    keyboard_config_init(); // loads the persisted keyboard layout, if any -- see kernel/lib/keyboard_config.c
    vga_reflow(); // apply it to the console's cell layout (no-op if nothing was persisted)

    // One-shot boot-time CMOS/RTC readout, logged for the same reason a
    // real kernel's dmesg has an "rtc_cmos ...: setting system clock"
    // line -- proves the hardware clock is readable and shows what it
    // says at boot. Deliberately NOT logged from rtc_read() itself
    // (timer.c) -- that function is called continuously by the taskbar
    // clock/tz code every time it redraws, so logging there would flood
    // the ring buffer; this is the one call site that only ever runs
    // once. Raw rtc_read(), not tz.h's rtc_read_local() -- this is
    // "what the hardware says," unadjusted for timezone/DST, matching
    // what a real kernel's own RTC probe logs before any timezone
    // config is even in the picture.
    struct rtc_time boot_time;
    rtc_read(&boot_time);
    klog_write("rtc: hardware clock reads ");
    klog_write_dec(boot_time.year); // already a full 4-digit year -- see rtc_read()'s "2000 + year" in timer.c
    klog_write("-");
    klog_write_dec2(boot_time.month);
    klog_write("-");
    klog_write_dec2(boot_time.day);
    klog_write(" ");
    klog_write_dec2(boot_time.hour);
    klog_write(":");
    klog_write_dec2(boot_time.minute);
    klog_write(":");
    klog_write_dec2(boot_time.second);
    klog_write(" UTC\n");

    scheduler_init();
    klog_write("toy-os: scheduler initialized (continuously armed; see `schedtest`)\n");

    debug_console_init(); // serial debug console (COM1) -- see docs/decisions.md; polled from keyboard_getchar()'s and wm_run()'s idle-wait loops

    // Boot is over; stop putting kernel log lines on top of whatever the
    // shell or the GUI draws. `dmesg`, the serial port and the console's
    // own scrollback all still have them.
    klog_set_console_echo(0);

    apps_start();

    for (;;) __asm__ volatile ("hlt");
}
