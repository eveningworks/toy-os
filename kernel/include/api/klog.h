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

// A LEVEL IS AN IN-BAND PREFIX, as in Linux's KERN_ERR: the marker sits
// on the front of the string and klog_write() takes it off at a line
// start. That is what let levels arrive without touching any of the
// ~940 existing call sites, which keep the default.
//
// The NUMBERS are Linux's, so `<3>` means there what it means here.
// Levels 0, 1 and 5 (EMERG, ALERT, NOTICE) are deliberately not
// defined: this kernel has nothing that is above CRIT or between INFO
// and WARN, and a level nothing writes is a filter option that never
// matches.
//
// **THE MARKER NEVER REACHES THE WIRE.** klog_write() strips it before
// any byte is emitted, and the ring records the level in its own
// timestamp instead (`[7.03] <3> `) -- so serial.log's format, which
// every test harness reads, is exactly what it was.
#define KLOG_SOH    "\001"
#define KLOG_CRIT   KLOG_SOH "2"   // the machine is going down
#define KLOG_ERR    KLOG_SOH "3"   // something failed
#define KLOG_WARN   KLOG_SOH "4"   // something recovered, and you should know
#define KLOG_INFO   KLOG_SOH "6"   // the default -- boot progress, what bound
#define KLOG_DEBUG  KLOG_SOH "7"   // per-device chatter, off by default

#define KLOG_LEVEL_CRIT   2
#define KLOG_LEVEL_ERR    3
#define KLOG_LEVEL_WARN   4
#define KLOG_LEVEL_INFO   6
#define KLOG_LEVEL_DEBUG  7

// What still reaches the console -- serial and, during boot, the
// screen. The RING ALWAYS KEEPS EVERYTHING: a threshold that dropped
// bytes from the log would be losing exactly the evidence a fault
// needs, which this project has already paid for once. Linux's
// console_loglevel, same split.
//
// Defaults to KLOG_LEVEL_INFO, so a future KLOG_DEBUG line is quiet
// until somebody asks for it. Nothing logs at debug today, so no line
// that used to appear stopped appearing.
void klog_set_console_level(int level);
int  klog_console_level(void);


// Reads `loglevel=<0-7>` out of the GRUB command line, if it is there.
void klog_apply_cmdline(const char *cmdline);

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

// A SLICE OF THE RING BY ABSOLUTE OFFSET -- what /bin/dmesg reads
// through QUERY_KLOG, and what klog_dump() above cannot express: a
// callback streams, and a syscall has to fill a fixed buffer and
// return.
//
// `from` counts from the first byte ever logged, not from a ring
// position, because the ring moves under a reader that takes more than
// one call to walk it. A reader compares `from` against the
// `*out_first` it gets back: if the second is larger, bytes it asked
// for had already aged out. That is the guarantee /dev/kmsg's sequence
// numbers give on Linux.
//
// Returns bytes copied, 0 at the end of the log.
uint32_t klog_read(uint64_t from, char *out, uint32_t cap, uint64_t *out_first);

// Bytes ever written, and bytes still retained. The first is monotonic
// and is what an offset is relative to; the second is at most
// KLOG_BUF_SIZE.
uint64_t klog_total_bytes(void);
uint32_t klog_retained_bytes(void);

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

// HOLDS the log's bytes off the serial wire, so one reply can go out in
// a single uninterrupted block (the debug console's `readfile`). Held
// bytes still reach the ring and dmesg at once; the WIRE gets them on
// release, in order. A hold that outgrows its buffer drops the overflow
// from the wire only and says how much on release.
void klog_serial_hold(void);
void klog_serial_release(void);

#endif
