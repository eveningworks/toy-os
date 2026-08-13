#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

// The kernel's in-memory log -- see klog.c's top comment for the full
// design (a ring buffer, timestamps added once per logical line, and
// why this is a thin decorator around serial.c rather than a
// replacement for it). Every kernel-side diagnostic message should
// call klog_write() instead of serial_write() directly -- it still
// forwards every raw byte to the physical COM1 port exactly as
// serial_write() always did, so nothing about the wire format
// changes; it just also buffers a timestamped copy for the shell's
// `dmesg` command (apps/shell.c) to read back.

void klog_write(const char *s);
void klog_putc(char c);

// Decimal/hex number formatting for klog messages that need to include
// a value (a device ID, a resolution, a count) -- mirrors
// vga_write_dec()/vga_write_hex() (vga.h) exactly (same no-padding
// decimal, same "0x" + leading-zeros-trimmed hex), just routed to the
// ring buffer (and physical serial) rather than the screen.
//
// These two and vga.c's pair used to be four separate digit loops, and
// this comment used to explain why: sharing them would have meant klog
// depending on a driver. They all go through knum.h now, which is that
// dependency done right -- a converter into a caller-owned buffer,
// depending on nothing itself, so both sinks share one implementation
// and one set of tests. If you're writing a whole formatted LINE rather
// than one value, reach for kfmt.h's klog_printf() instead of chaining
// these.
void klog_write_dec(uint32_t n);
void klog_write_hex(uint64_t n);

// Streams the ring buffer's current contents to putc_cb, one character
// at a time, oldest first -- what `dmesg` is built on. Safe to call at
// any time; never blocks. No userdata parameter on `putc_cb`, matching
// this codebase's existing plain-callback convention (see fs_list()'s
// callback in fs.h, and syscall.c's "SYS_LISTDIR scratch state"
// comment for the same pattern) -- a caller that needs to carry state
// across calls (e.g. `dmesg`'s own pagination) uses file-scope statics
// for it, not a userdata pointer.
void klog_dump(void (*putc_cb)(char c));

// Mirrors every klog_write()/klog_putc() byte to the physical console
// as well as the serial port, while enabled.
//
// kernel_main() turns this on early and off again just before
// apps_start(), so the boot sequence is visible on screen the way a
// real kernel's is -- and then stays out of the way. Everything the
// kernel logs after that is still in `dmesg` and on the serial line.
// The console keeps scrollback (see vga.h), so the boot messages remain
// readable with PageUp once the shell is up.
void klog_set_console_echo(int on);

#endif
