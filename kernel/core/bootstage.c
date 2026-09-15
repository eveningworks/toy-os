// Which subsystems are up. See kernel/include/kernel/bootstage.h.
#include "bootstage.h"
#include "vga.h"
#include "klog.h"
#include "kfmt.h"

static uint32_t g_up;

static const char *sub_name(uint32_t sub) {
    switch (sub) {
        case BOOT_SUB_PCI:  return "pci_init()";
        case BOOT_SUB_PMM:  return "pmm_init()";
        default:            return "an unknown subsystem";
    }
}

void boot_subsystem_up(uint32_t sub) { g_up |= sub; }

int boot_subsystem_is_up(uint32_t sub) { return (g_up & sub) == sub; }

void boot_require(uint32_t sub, const char *caller) {
    if ((g_up & sub) == sub) return;

    // Same shape as __stack_chk_fail(): say it on screen AND on the
    // serial log, then halt. The screen may not exist yet this early --
    // a display driver is a plausible caller of exactly this -- which is
    // why the serial line is the one that carries the diagnosis.
    vga_set_color(VGA_WHITE, VGA_RED);
    vga_write("\n*** KERNEL PANIC: boot order ***\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    klog_printf(KLOG_CRIT "PANIC: %s ran before %s -- see kernel_main() and"
                " kernel/include/kernel/bootstage.h\n",
                caller ? caller : "(unknown caller)", sub_name(sub));

    for (;;) __asm__ volatile ("cli; hlt");
}
