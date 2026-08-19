#ifndef FAULT_INJECT_H
#define FAULT_INJECT_H

// Deliberate failure injection, for testing error paths that are
// otherwise unreachable from inside a running kernel.
//
// The problem this solves: this kernel has a lot of "what if the disk
// says no" handling -- a record that couldn't be persisted, a block
// that couldn't be zeroed, a superblock that couldn't be read -- and
// every one of those paths only executes when the hardware misbehaves.
// Before this existed the only way to reach any of them was
// tools/tfs2_writer.py's `corrupt` subcommand: damage a disk image on
// the host, boot against it, and read the result. That works, but it's
// slow, it only reaches disk-shaped failures, and it can't test the
// out-of-memory paths at all.
//
// Everything here is compiled in unconditionally and inert until armed,
// which is deliberate: a test-only build would be a second
// configuration to keep working, and the counters cost one comparison
// on paths that already do a DMA transfer or walk a free list. Arming
// is a kernel-internal call (this header is not in include/api/), so
// nothing in apps/ can turn it on.
//
// Every injector is a countdown: arm it with N, the next N calls fail,
// then it disarms itself. Pass 0 to disarm early. A test MUST disarm
// what it arms -- there's no automatic teardown, and a leftover armed
// counter turns the next unrelated test into a confusing failure.

#include <stdint.h>

// The next `count` ata_write_sector()/ata_write_sectors() calls return
// failure without touching the drive. Reads are unaffected.
void fault_fail_next_ata_writes(uint32_t count);

// Same for ata_read_sector()/ata_read_sectors().
void fault_fail_next_ata_reads(uint32_t count);

// The next `count` blk_write_sectors()/blk_read_sectors() calls fail
// without reaching whatever device is registered.
//
// USE THESE, NOT THE ATA PAIR ABOVE, FOR ANYTHING TESTING THE
// FILESYSTEM. The ATA injectors reach only one backend, so a
// filesystem error-path test written against them silently stops
// testing anything the moment the filesystem is mounted on something
// else -- measured: with the filesystem on virtio-blk, four fs error
// tests failed outright, because the writes they expected to fail
// simply succeeded. The block layer is where "the device refused this"
// belongs, since that is the seam every backend passes through.
//
// The ATA pair stays for ATA's OWN tests (the write-back cache, which
// lives below this layer and cannot be reached from here).
void fault_fail_next_block_writes(uint32_t count);
void fault_fail_next_block_reads(uint32_t count);

// The next `count` kmalloc()/kzalloc() calls return NULL without
// allocating. Note kfree() is unaffected -- freeing is not a failure
// path in this heap.
void fault_fail_next_allocs(uint32_t count);

// 1 if any injector is currently armed. The test runner checks this
// after each test and complains, since a leaked injector makes every
// later test suspect.
int fault_any_armed(void);

// Called by the injection points themselves; returns 1 if this call
// should be failed (and consumes one from the countdown).
int fault_should_fail_block_write(void);
int fault_should_fail_block_read(void);
int fault_should_fail_ata_write(void);
int fault_should_fail_ata_read(void);
int fault_should_fail_alloc(void);

#endif
