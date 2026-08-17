#include "power.h"
#include "io.h"
#include "vga.h"
#include "klog.h"
#include "ata_cache.h"

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
    klog_write("power: poweroff requested (QEMU/Bochs ACPI PM1a_CNT port trick)\n");

    // See power.h's top comment: this is QEMU/Bochs's well-known ACPI
    // shortcut, not a real parsed-from-the-guest's-own-tables ACPI
    // shutdown. Written unconditionally -- there's no way to probe
    // "will this port write actually be honored" ahead of time, so the
    // halt-loop fallback below is what makes this safe to call
    // regardless of which VM/hardware it's running on.
    outw(0x604, 0x2000);

    // Still here -- the port write didn't take (real hardware, or an
    // emulator that doesn't implement this legacy shortcut). Same
    // "always end up in a safe, inert state" contract system_reboot()
    // has for its own fallback path.
    vga_write("\nSystem halted -- it is now safe to close this window.\n");
    klog_write("power: poweroff port write had no effect, halting instead\n");
    for (;;) __asm__ volatile ("hlt");
}
