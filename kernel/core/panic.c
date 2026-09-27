// panic_finish() -- what every kernel panic does once it has reported.
// See kernel/panic.h for the contract and panic_store.h for the record.
#include "panic.h"
#include "panic_store.h"
#include "multiboot.h"
#include "knum.h"
#include "klog.h"
#include "kfmt.h"
#include "vga.h"
#include "serial.h"   // serial_flush()
#include "clocksource.h"
#include "io.h"
#include "kdebug.h"

int panic_parse_secs(const char *value, int *out) {
    int64_t v;
    if (!value || !k_parse_i64(value, &v)) return 0;
    if (v > 3600) v = 3600;
    if (v < -1) v = -1;
    *out = (int)v;
    return 1;
}

static int panic_secs(void) {
    char v[16];
    int secs = PANIC_DEFAULT_SECS;
    if (multiboot_cmdline_value("panic=", v, sizeof v)) panic_parse_secs(v, &secs);
    return secs;
}

// A key PRESSED on the PS/2 keyboard -- a make code, so not the release
// of the Enter that ran the command which panicked, not a controller
// reply (0xFA/0xFE), not the mouse sharing the port, and not COM1, where
// a test harness's resync bytes would end a countdown nobody is reading.
// Polled, because interrupts are off for good.
static int key_waiting(void) {
    uint8_t st = inb(0x64);
    if (st == 0xFF || !(st & 0x01)) return 0;
    uint8_t b = inb(0x60);
    if (st & 0x20) return 0;
    return b < 0x80 && b != 0x00;
}

// Whatever was already buffered when the panic hit is not a key pressed
// in answer to the countdown.
static void drain_8042(void) {
    for (int i = 0; i < 64; i++) {
        uint8_t st = inb(0x64);
        if (st == 0xFF || !(st & 0x01)) return;
        inb(0x60);
    }
}

static void wait_100ms(void) {
    if (clocksource_deadline_capable()) { clocksource_delay_ms(100); return; }
    // No clock that runs with interrupts off (only before the TSC or PM
    // timer registers): an I/O-port write is about a microsecond.
    for (int i = 0; i < 100000; i++) outb(0x80, 0);
}

void panic_finish(void) {
    __asm__ volatile ("cli");
    // A fault INSIDE the panic path lands here again; do not try to save
    // or count a second time, just get the machine back.
    static int entered;
    if (entered++) power_reset_hardware();
    kdebug_panic();

    int secs = panic_secs();
    if (secs > 0)
        klog_printf(KLOG_CRIT "panic: restarting in %d s (panic=0 on the GRUB line halts instead)\n", secs);
    else if (secs == 0)
        klog_write(KLOG_CRIT "panic: halted (panic=0)\n");
    panic_store_save();
    serial_flush();

    if (secs == 0) {
        vga_present_force();
        for (;;) __asm__ volatile ("hlt");
    }
    drain_8042();
    for (int left = secs * 10; left > 0; left--) {
        if (left % 10 == 0) {
            vga_printf("\rRestarting in %d s -- press a key to restart now. ", left / 10);
            vga_present_force();
        }
        if (key_waiting()) break;
        wait_100ms();
    }
    vga_write("\nRestarting.\n");
    vga_present_force();
    serial_flush();
    power_reset_hardware();
}
