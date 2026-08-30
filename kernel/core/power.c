#include "power.h"
#include "io.h"
#include "vga.h"
#include "klog.h"
#include "ata_cache.h"
#include "acpi.h"

// FLUSH FIRST. With a write-back cache under the disk (ata_cache.h) a
// write that returned success may still be sitting in RAM, so both of
// these would otherwise discard it -- and the user's last action before
// a shutdown is exactly the one they would notice missing. Both paths
// end the machine, so this is the last chance either gets; it is here
// rather than at the call sites so a future caller cannot forget it.
static void flush_before_stopping(const char *what) {
    if (!atac_flush()) {
        klog_write("power: DISK FLUSH FAILED before ");
        klog_write(what);
        klog_write(" -- some writes were NOT saved\n");
    }
}

void system_reboot(void) {
    flush_before_stopping("reboot");

    // ACPI first, the 8042 pulse second. That order rather than the
    // other way round because the reset register is what the firmware
    // asked for, and a machine with no PS/2 controller at all still has
    // one -- but the pulse works on everything this project runs on
    // today, so it stays as the fallback rather than being replaced.
    acpi_reset();

    uint8_t status;
    do {
        status = inb(0x64);
        if (status & 1) inb(0x60);
    } while (status & 2);
    outb(0x64, 0xFE);

    for (;;) __asm__ volatile ("hlt"); // in case the reset didn't take
}

void system_poweroff(void) {
    flush_before_stopping("power off");
    klog_write("power: poweroff requested\n");

    // The real thing: the sleep type from this machine's own `_S5_`,
    // written to the port its own FADT names. Returns only when there
    // was nothing to try -- no ACPI, no FADT, or a `_S5_` this kernel
    // would not decode.
    acpi_poweroff();

    // QEMU/Bochs's well-known shortcut, kept as a fallback for the one
    // case ACPI cannot cover: a guest whose tables did not survive the
    // walk above but whose chipset still answers this port.
    klog_write("power: falling back to the QEMU/Bochs PM1a_CNT port trick\n");
    outw(0x604, 0x2000);

    // Still here -- neither path stopped the machine. Same "always end
    // up in a safe, inert state" contract system_reboot() has for its
    // own fallback path.
    vga_write("\nSystem halted -- it is now safe to close this window.\n");
    klog_write("power: poweroff port write had no effect, halting instead\n");
    for (;;) __asm__ volatile ("hlt");
}
