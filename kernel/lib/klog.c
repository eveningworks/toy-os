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
// this (see the git history for the build), specifically so dmesg
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
#include "vga.h"
#include "serial.h"
#include "timer.h"
#include "knum.h"
#include "kfmt.h"

#define KLOG_BUF_SIZE 16384

static char klog_buf[KLOG_BUF_SIZE];
static uint32_t klog_head = 0;  // next write position, wraps mod KLOG_BUF_SIZE
static uint32_t klog_count = 0; // valid bytes currently buffered, caps at KLOG_BUF_SIZE
// EVERY BYTE EVER WRITTEN, never reset and never wrapped in practice --
// 64 bits at this log's rate is longer than any machine will run. It is
// what gives a byte in the ring a name that stays true after the ring
// has moved past it; see klog_read(). Monotonic for the same reason
// kbdtap's `seq` is.
static uint64_t klog_total = 0;
static int at_line_start = 1;   // true at boot and right after the last '\n' written

static void klog_buf_putc(char c) {
    klog_buf[klog_head] = c;
    klog_head = (klog_head + 1) % KLOG_BUF_SIZE;
    if (klog_count < KLOG_BUF_SIZE) klog_count++;
    klog_total++;
}

// Writes "[secs.hh] " straight into the ring buffer (not through
// klog_putc() -- this is buffer-only content, never sent to the
// physical port, and doesn't itself count as "starting a new line" for
// at_line_start purposes).
static void klog_write_timestamp(void) {
    uint64_t ticks = pit_ticks(); // 100 Hz -- see top comment
    uint32_t secs = (uint32_t)(ticks / 100);
    uint32_t hund = (uint32_t)(ticks % 100);

    // Formatted through knum rather than a digit loop of its own (this
    // file had two more of those until the toolkit landed). It can't go
    // through klog_printf(), which would recurse straight back into
    // klog_putc() and back into here -- buffer-only content, written
    // with klog_buf_putc() directly, is the whole point of this
    // function.
    char stamp[24];
    k_snprintf(stamp, sizeof stamp, "[%u.%02u] ", secs, hund);
    for (const char *p = stamp; *p; p++) klog_buf_putc(*p);
}

// See klog_set_console_echo() -- 0 until boot switches it on, and off
// again before the shell starts.
static int g_console_echo = 0;

void klog_set_console_echo(int on) {
    g_console_echo = on ? 1 : 0;
}

void klog_putc(char c) {
    serial_putc(c); // raw wire byte, unchanged -- see top comment
    if (at_line_start) klog_write_timestamp();
    klog_buf_putc(c);
    at_line_start = (c == '\n');
    // Echo to the physical console too, while enabled. Off by default
    // and switched off for good just before apps_start() (kernel.c), so
    // this only ever covers boot: the messages a real kernel prints on
    // screen while coming up, which until now went exclusively to the
    // serial port and dmesg. Leaving it on afterwards would put every
    // ATA retry and filesystem warning on top of whatever the shell or
    // the GUI is drawing.
    if (g_console_echo) vga_putc(c);
}

void klog_write(const char *s) {
    while (*s) klog_putc(*s++);
}

// Both of these used to be full digit loops, byte-for-byte identical to
// vga.c's pair -- this header's own comment used to explain that
// duplicating them was cheaper than making klog.c depend on a driver
// for them. knum.c is the third option that comment didn't have: a
// converter that depends on nothing, fills a caller's buffer, and lets
// each sink print it however it likes.
void klog_write_dec(uint32_t n) {
    char buf[21];
    k_utoa(n, buf, sizeof buf);
    klog_write(buf);
}

void klog_write_hex(uint64_t n) {
    char buf[17];
    k_htoa(n, buf, sizeof buf, 0); // 0 = shortest form, no leading zeros
    klog_write("0x");
    klog_write(buf);
}

// A SLICE OF THE RING, for a reader that is not a callback -- the query
// provider (kernel/lib/klog_query.c), which has to fill a fixed-size
// record and hand it across the syscall boundary rather than stream
// into a sink.
//
// **`from` IS AN ABSOLUTE OFFSET, counted from the first byte ever
// logged, and that is what makes a walking reader safe.** The ring
// overwrites its oldest bytes as the kernel keeps logging, so a reader
// that walked by ring position would silently re-read or skip whatever
// moved under it between two calls. An absolute offset cannot: a
// reader that asks for a range which has already aged out is told so
// (0 bytes copied and *out_first set past what it asked for) rather
// than handed different bytes than it expected. Linux's /dev/kmsg
// gives every record a sequence number for exactly this, and prints a
// '-' when a reader notices a gap.
//
// Returns bytes copied. `out_first` receives the absolute offset the
// FIRST copied byte actually came from, which is `from` clamped up to
// the oldest byte still retained -- so a reader compares the two and
// knows whether it lost anything.
uint32_t klog_read(uint64_t from, char *out, uint32_t cap, uint64_t *out_first) {
    uint64_t total = klog_total;
    uint64_t oldest = (total > KLOG_BUF_SIZE) ? total - KLOG_BUF_SIZE : 0;
    if (from < oldest) from = oldest;
    if (out_first) *out_first = from;
    if (!out || !cap || from >= total) return 0;

    uint64_t avail = total - from;
    uint32_t n = (avail < cap) ? (uint32_t)avail : cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = klog_buf[(uint32_t)((from + i) % KLOG_BUF_SIZE)];
    return n;
}

// How many bytes have EVER been written, and how many are still
// retained. The pair is what a reader needs to size its walk and to
// notice that the window moved.
uint64_t klog_total_bytes(void) { return klog_total; }
uint32_t klog_retained_bytes(void) { return klog_count; }

void klog_dump(void (*putc_cb)(char c)) {
    uint32_t start = (klog_count < KLOG_BUF_SIZE) ? 0 : klog_head;
    for (uint32_t i = 0; i < klog_count; i++) {
        putc_cb(klog_buf[(start + i) % KLOG_BUF_SIZE]);
    }
}
