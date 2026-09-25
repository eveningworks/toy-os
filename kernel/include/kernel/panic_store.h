#ifndef KERNEL_PANIC_STORE_H
#define KERNEL_PANIC_STORE_H

#include <stdint.h>

// THE PANIC STORE: a fixed range of physical RAM that a panic copies the
// kernel log into and the NEXT boot reads back -- Linux's pstore/ramoops
// shape. It survives only a WARM reset (panic.c's countdown ends in one),
// and only on firmware that does not scrub memory. A record that did not
// survive fails its checksum, so the failure is "no record", never a
// wrong one.
//
// **THE RANGE IS A CONSTANT, because two things need it before anything
// could look it up**: KASLR (reloc.c) must not copy the kernel over it,
// and pmm must not hand it out. 32 MiB is above the kernel image and
// below where GRUB's heap grows down from; panic_store_probe() refuses
// it, and says so, on a machine where that does not hold.
#define PANIC_STORE_BASE 0x2000000ULL
#define PANIC_STORE_SIZE 0x8000ULL   // the whole 16 KiB klog ring, and a header

// Called from pmm_init(), before the frame allocator exists: is the range
// RAM clear of the image and GRUB's allocations, and does it hold a
// record from the previous boot? pmm reserves the range iff usable.
void panic_store_probe(void);
int panic_store_usable(void);

// At panic: copy the kernel log's tail in. No locks, no heap, IF off.
void panic_store_save(void);

// The previous boot's record, or NULL when there is none (or it has been
// cleared). The text stays where it is -- the range is reserved.
const char *panic_store_text(uint32_t *len, uint64_t *uptime_ns, const char **build);
void panic_store_clear(void);

// The record format, as pure functions over a caller's buffer so the
// KTESTs can reach it. The text is written at PANIC_RECORD_TEXT(buf)
// first, then sealed; check() answers the text length, or -1.
#define PANIC_RECORD_HDR 72u
#define PANIC_RECORD_TEXT(buf) ((char *)(buf) + PANIC_RECORD_HDR)
int panic_record_seal(void *buf, uint32_t cap, uint32_t len,
                      uint64_t uptime_ns, const char *build);
int panic_record_check(const void *buf, uint32_t cap);

#endif
