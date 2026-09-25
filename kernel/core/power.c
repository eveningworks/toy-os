#include "power.h"
#include "io.h"
#include "vga.h"
#include "klog.h"
#include "ata_cache.h"
#include "fs.h"
#include "acpi.h"
#include "usb.h"      // usb_shutdown() -- quiesce the links before the reset
#include "panic.h"    // power_reset_hardware()

// COMMIT, THEN FLUSH -- and it must be fs_sync(), not atac_flush().
// With a write-back cache under the disk (ata_cache.h) a write that
// returned success may still be sitting in RAM, and the user's last
// action before a shutdown is exactly the one they would notice
// missing. Both paths end the machine, so this is the last chance
// either gets; it is here rather than at the call sites so a future
// caller cannot forget it.
//
// THE TRAP THIS EXISTS FOR: atac_flush() alone flushes the SECTOR
// CACHE, which is a layer BELOW the filesystem. `storage.sync =
// batched` (the default) keeps a TFS3 journal transaction open across
// writes, so blocks the transaction still owns have not reached the
// sector cache at all and a flush cannot save them -- fs_sync()'s own
// stage 0 is what commits them. Reboot within the writeback window and
// the write is lost; worse, fs_write() truncates first and THAT lands,
// so the file's PREVIOUS contents go too. Measured on 2026-09-19: a
// `config set` followed at once by `reboot` left a 0-byte
// /etc/storage.conf and a setting silently back at its default.
static void flush_before_stopping(const char *what) {
    if (!fs_sync(0)) {
        klog_write(KLOG_ERR "power: FILESYSTEM SYNC FAILED before ");
        klog_write(what);
        klog_write(" -- some writes were NOT saved\n");
    }
    if (!atac_flush()) {
        klog_write(KLOG_ERR "power: DISK FLUSH FAILED before ");
        klog_write(what);
        klog_write(" -- some writes were NOT saved\n");
    }
}

void system_reboot(void) {
    flush_before_stopping("reboot");
    // AFTER the flush, because a device may still be carrying the write
    // out, and BEFORE the reset, because the point is that the links go
    // down while something is still able to take them down. See
    // usb_shutdown().
    //
    // ITS LOG LINE IS NOT IN THE BOOT LOG, and that is this ordering's
    // one cost: everything klog_write()s from here on is emitted after
    // the last fs_sync() and dies with the reset. Serial has it; the
    // persistent log cannot. Do not go looking for it in
    // /var/log/boot/NNNN.log and conclude the call did not run.
    usb_shutdown();
    power_reset_hardware();
}

void power_reset_hardware(void) {
    // ACPI first, the 8042 pulse second. That order rather than the
    // other way round because the reset register is what the firmware
    // asked for, and a machine with no PS/2 controller at all still has
    // one -- but the pulse works on everything this project runs on
    // today, so it stays as the fallback rather than being replaced.
    acpi_reset();

    // BOUNDED: with no controller at all the port reads 0xFF, whose
    // "input buffer full" bit never clears.
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(0x64);
        if (status & 1) inb(0x60);
        if (!(status & 2)) break;
    }
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 10000000; i++) { }

    // A triple fault resets every x86 there is: no IDT, then a trap.
    static const struct { uint16_t limit; uint64_t base; } __attribute__((packed))
        no_idt = { 0, 0 };
    __asm__ volatile ("cli; lidt %0; int3" :: "m"(no_idt));
    for (;;) __asm__ volatile ("hlt");
}

void system_poweroff(void) {
    flush_before_stopping("power off");
    usb_shutdown();
    klog_write("power: poweroff requested\n");

    // The real thing: the sleep type from this machine's own `_S5_`,
    // written to the port its own FADT names. Returns only when there
    // was nothing to try -- no ACPI, no FADT, or a `_S5_` this kernel
    // would not decode.
    acpi_poweroff();

    // QEMU/Bochs's well-known shortcut, and ONLY for a machine whose
    // tables told us nothing. It used to run whenever acpi_poweroff()
    // declined, including after a real PM1a write that did not take --
    // and on real hardware 0x604 is not a known port but a LIVE one,
    // somewhere in a chipset's PM range. Writing SLP_EN with a sleep
    // type the firmware never named is the definition of guessing at
    // hardware, which this project's parsers are not allowed to do.
    if (acpi_poweroff_known()) {
        vga_write("\nSystem halted -- ACPI accepted the request and the "
                  "machine stayed up.\n");
        klog_write("power: ACPI named a poweroff path and it did not stop the "
                   "machine -- NOT guessing at another port\n");
        for (;;) __asm__ volatile ("hlt");
    }
    klog_write("power: no ACPI poweroff path -- trying the QEMU/Bochs port\n");
    outw(0x604, 0x2000);

    // Still here -- neither path stopped the machine. Same "always end
    // up in a safe, inert state" contract system_reboot() has for its
    // own fallback path.
    vga_write("\nSystem halted -- it is now safe to close this window.\n");
    klog_write("power: poweroff port write had no effect, halting instead\n");
    for (;;) __asm__ volatile ("hlt");
}
