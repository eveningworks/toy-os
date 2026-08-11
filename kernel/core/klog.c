// The kernel's in-memory log: every kernel-side diagnostic message
// (boot progress, syscall rejections, driver init, the ring3test/
// elftest/etc. diagnostic commands, ...) that used to go only to the
// physical COM1 serial port now also lands in a fixed-size ring buffer
// here, readable via klog_dump() -- what the shell's `dmesg` command
// (apps/shell.c) is built on.
//
// This is a thin decorator around serial.c, not a replacement for it:
// klog_write()/klog_putc() still forward every raw byte to
// serial_write()/serial_putc() unchanged, so the wire format
// tools/qmp_test.py's and tools/boot_smoke_test.py's serial.log
// capture depend on is completely unaffected -- they just ALSO buffer
// a timestamped copy. serial.c itself never changed; it's still just
// the raw UART hardware driver and doesn't know klog exists.
//
// Every one of the ~40 existing serial_write() call sites across the
// kernel was mechanically renamed to klog_write() as part of adding
// this (see CHANGELOG.md for the build), specifically so dmesg
// reflects real, complete kernel history from boot -- not just
// whatever new call sites get added going forward.
//
// Timestamps ("[secs.hh] ", e.g. "[7.03] ") are added to the RING
// BUFFER copy only, once per logical line -- never to the raw bytes
// sent out over the physical serial port. "Logical line" means "since
// the last '\n' this module sent, however many separate klog_write()/
// klog_putc() calls it took to get there" (tracked via at_line_start
// below), not "one timestamp per call" -- idt.c's panic path, for
// instance, builds one line ("PANIC: Divide Error\n") across three
// separate calls with no '\n' until the last one; without tracking
// this, each fragment would wrongly get its own timestamp stitched
// into the middle of one logical message. The unit is PIT ticks / 100
// for whole seconds, ticks % 100 for hundredths -- this kernel's PIT
// really does run at 100Hz (see timer.h), so hundredths is honest
// resolution, not fabricated microsecond precision the way real
// dmesg's timestamps often look on real hardware.
//
// Ring buffer, not a growing one: KLOG_BUF_SIZE bytes, oldest bytes
// silently overwritten once full -- same one-shot,
// no-history-beyond-N tradeoff a real kernel's dmesg ring buffer
// makes. A toy OS session producing more than 16KB of log output in
// one boot isn't a case worth handling specially.
#include "klog.h"
#include "serial.h"
#include "timer.h"

#define KLOG_BUF_SIZE 16384

static char klog_buf[KLOG_BUF_SIZE];
static uint32_t klog_head = 0;  // next write position, wraps mod KLOG_BUF_SIZE
static uint32_t klog_count = 0; // valid bytes currently buffered, caps at KLOG_BUF_SIZE
static int at_line_start = 1;   // true at boot and right after the last '\n' written

static void klog_buf_putc(char c) {
    klog_buf[klog_head] = c;
    klog_head = (klog_head + 1) % KLOG_BUF_SIZE;
    if (klog_count < KLOG_BUF_SIZE) klog_count++;
}

// Writes "[secs.hh] " straight into the ring buffer (not through
// klog_putc() -- this is buffer-only content, never sent to the
// physical port, and doesn't itself count as "starting a new line" for
// at_line_start purposes).
static void klog_write_timestamp(void) {
    uint64_t ticks = pit_ticks(); // 100 Hz -- see top comment
    uint32_t secs = (uint32_t)(ticks / 100);
    uint32_t hund = (uint32_t)(ticks % 100);

    klog_buf_putc('[');
    char tmp[10];
    int n = 0;
    if (secs == 0) {
        tmp[n++] = '0';
    } else {
        uint32_t v = secs;
        while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    }
    while (n > 0) klog_buf_putc(tmp[--n]);
    klog_buf_putc('.');
    klog_buf_putc((char)('0' + (hund / 10)));
    klog_buf_putc((char)('0' + (hund % 10)));
    klog_buf_putc(']');
    klog_buf_putc(' ');
}

void klog_putc(char c) {
    serial_putc(c); // raw wire byte, unchanged -- see top comment
    if (at_line_start) klog_write_timestamp();
    klog_buf_putc(c);
    at_line_start = (c == '\n');
}

void klog_write(const char *s) {
    while (*s) klog_putc(*s++);
}

void klog_write_dec(uint32_t n) {
    char tmp[11];
    int i = 0;
    if (n == 0) {
        klog_putc('0');
        return;
    }
    while (n > 0) {
        tmp[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0) klog_putc(tmp[--i]);
}

void klog_write_hex(uint64_t n) {
    klog_write("0x");
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (n >> (i * 4)) & 0xF;
        buf[15 - i] = nibble < 10 ? (char)('0' + nibble) : (char)('a' + nibble - 10);
    }
    buf[16] = '\0';
    int start = 0;
    while (start < 15 && buf[start] == '0') start++;
    klog_write(buf + start);
}

void klog_dump(void (*putc_cb)(char c)) {
    uint32_t start = (klog_count < KLOG_BUF_SIZE) ? 0 : klog_head;
    for (uint32_t i = 0; i < klog_count; i++) {
        putc_cb(klog_buf[(start + i) % KLOG_BUF_SIZE]);
    }
}
