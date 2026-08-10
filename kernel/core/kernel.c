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
#include "pci.h"
#include "fs.h"
#include "tz.h"
#include "font_config.h"
#include "apps.h"
#include "scheduler.h"
#include <stdint.h>

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

    pci_init(); // brute-force config-space scan -- see pci.h's top comment
    klog_write("toy-os: PCI bus enumerated\n");

    fs_init();
    fs_mkdir("/etc"); // config-file convention (see tz.c) -- a no-op if it already exists
    tz_init(); // loads the persisted timezone choice, if any -- needs fs_init()/"/etc" first
    font_config_init(); // loads the persisted font size, if any -- see kernel/core/font_config.c
    vga_reflow(); // apply it to the console's cell layout (no-op if nothing was persisted)

    scheduler_init();
    klog_write("toy-os: scheduler initialized (disarmed; see `schedtest`)\n");

    apps_start();

    for (;;) __asm__ volatile ("hlt");
}
