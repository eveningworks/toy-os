#ifndef KLOG_H
#define KLOG_H

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

// Streams the ring buffer's current contents to putc_cb, one character
// at a time, oldest first -- what `dmesg` is built on. Safe to call at
// any time; never blocks. No userdata parameter on `putc_cb`, matching
// this codebase's existing plain-callback convention (see fs_list()'s
// callback in fs.h, and syscall.c's "SYS_LISTDIR scratch state"
// comment for the same pattern) -- a caller that needs to carry state
// across calls (e.g. `dmesg`'s own pagination) uses file-scope statics
// for it, not a userdata pointer.
void klog_dump(void (*putc_cb)(char c));

#endif
