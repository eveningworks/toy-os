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
#include <stdint.h>

// Zero-padded 2-digit decimal, for the RTC boot-time log line below --
// klog_write_dec() (klog.h) deliberately doesn't pad (matches
// vga_write_dec()'s no-padding behavior), so a real clock display needs
// its own small helper the same way klog_write_timestamp() (klog.c)
// does internally for the "[secs.hh]" prefix.
static void klog_write_dec2(uint8_t n) {
    klog_putc((char)('0' + (n / 10) % 10));
    klog_putc((char)('0' + (n % 10)));
}

void kernel_main(uint64_t multiboot_info_addr) {
    serial_init();
    klog_write("toy-os: kernel_main reached, initializing...\n");

    multiboot_set_info(multiboot_info_addr);

    vga_init();
    vga_write("toy-os booting...\n");

    gdt_init();
    klog_write("toy-os: GDT/TSS initialized\n");

    idt_init();
    klog_write("toy-os: IDT/PIC/PIT initialized, interrupts enabled\n");

    pmm_init();
    klog_write("toy-os: physical frame allocator initialized\n");
    pmm_selftest(); // exercises pmm_alloc_contiguous()/pmm_free_contiguous() -- see pmm.h

    heap_init(); // kmalloc()/kfree() -- built on pmm, needs it initialized first
    klog_write("toy-os: kernel heap initialized\n");
    heap_selftest();

    json_selftest(); // parse/accessor/write/round-trip check -- only needs kmalloc, not fs_init() yet

    pci_init(); // brute-force config-space scan -- see pci.h's top comment
    klog_write("toy-os: PCI bus enumerated\n");

    fs_init();
    fs_mkdir("/etc"); // config-file convention (see tz.c) -- a no-op if it already exists
    tz_init(); // loads the persisted timezone choice, if any -- needs fs_init()/"/etc" first
    font_config_init(); // loads the persisted font size, if any -- see kernel/core/font_config.c
    keyboard_config_init(); // loads the persisted keyboard layout, if any -- see kernel/core/keyboard_config.c
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
    klog_write("toy-os: scheduler initialized (disarmed; see `schedtest`)\n");

    apps_start();

    for (;;) __asm__ volatile ("hlt");
}
