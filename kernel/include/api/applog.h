#ifndef API_APPLOG_H
#define API_APPLOG_H

#include <stdint.h>

// WHAT A PROCESS SAID, tagged with which process said it.
//
// klog is the KERNEL's log and it stores BYTES, which is right for it: a
// kernel line is already prefixed with its subsystem and nothing needs
// to attribute it later. Application output needs the opposite -- the
// text is whatever the program printed and the useful fact is WHO -- so
// this stores RECORDS, one per write, each carrying a tag.
//
// SEPARATE FROM klog ON PURPOSE, and the reason is measured rather than
// tidy: on 2026-09-06 a driver logging once a second flushed a laptop's
// entire boot log out of the kernel ring and left an intermittent fault
// undiagnosable. A chatty PROGRAM must not be able to do that to kernel
// evidence, and sharing one ring is exactly how it could.
//
// ONE WRITE IS ONE RECORD. stdio line-buffers by default, so a program's
// printf arrives whole and this is right in practice; a program that
// writes half a line gets half a record, which is stated here rather
// than hidden because the alternative -- holding partial lines per
// writer -- costs a buffer per open descriptor to fix a case nothing
// here produces.

#define APPLOG_TAG_MAX  16   // PROC_NAME_MAX-ish; a tag is a program name
#define APPLOG_TEXT_MAX 200  // beyond this a line is truncated, not split

struct applog_rec {
    // Monotonic, from 1. A reader compares the sequence it wants
    // against the oldest still held: if the second is larger, records
    // aged out. That is the same guarantee klog_read()'s absolute
    // offset gives, and it exists for the same reason -- a gap a reader
    // cannot see is indistinguishable from a quiet machine.
    uint64_t seq;
    // Hundredths of a second since boot, from the same pit_ticks() klog
    // stamps its lines with -- so a merged log of both sources can be
    // read in order. Stamped at the WRITE, not when a reader drains it,
    // which can be a second later.
    uint64_t cs;
    char     tag[APPLOG_TAG_MAX];
    char     text[APPLOG_TEXT_MAX];
    uint16_t len;
};

// Append one record. `tag` is the writing program's name; a NULL or
// empty one becomes "?" rather than being refused, because losing the
// line would be worse than losing its attribution.
void applog_write(const char *tag, const char *text, uint32_t len);

// Records ever written, and the oldest sequence still held. A reader
// needs both: the first says how far to go, the second whether it has
// already missed something.
uint64_t applog_total(void);
uint64_t applog_oldest(void);

// Copy the record with sequence `seq`. Returns 0 if it has aged out or
// has not happened yet.
int applog_get(uint64_t seq, struct applog_rec *out);

#endif
