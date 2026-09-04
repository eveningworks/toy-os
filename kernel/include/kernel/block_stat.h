#ifndef BLOCK_STAT_H
#define BLOCK_STAT_H

#include <stdint.h>

// Where the time goes in the block layer, per operation kind.
//
// WHY IT EXISTS. Three unrelated things make a disk slow here and a
// MB/s figure tells them apart from none of them: commands that are too
// small, commands that are too many, and cache flushes. A flush moves
// no sectors at all and can still be most of the wall clock -- on a
// real SSD it forces the drive's DRAM to NAND, where an emulated one
// returns immediately. That difference is why a number measured in QEMU
// says nothing about hardware, and why this counts TIME rather than
// just calls.
//
// Always on, like `net_device`'s counters: two clocksource reads per
// operation is nothing beside a disk command, and a counter that has to
// be enabled first is a counter nobody has when they need it.
enum {
    BLK_STAT_READ  = 0,
    BLK_STAT_WRITE = 1,
    BLK_STAT_FLUSH = 2,
    BLK_STAT_TRIM  = 3,
    BLK_STAT_OPS   = 4,
};

// TIMED AROUND THE DRIVER CALL, so a hit in a sector cache below this
// layer is a fast call rather than an absent one. `calls` is what the
// filesystem asked for; what reached a platter is the driver's own
// question.
void blk_stat_add(int op, uint32_t sectors, uint64_t ns, int ok);

// Zeroes every counter. `/bin/diskbench` brackets a profile with this,
// which is the only way to attribute time to one pass rather than to
// everything since boot.
void blk_stat_reset(void);

// 1 and fills the outputs, or 0 for an op out of range. Any pointer may
// be NULL.
int blk_stat_get(int op, uint64_t *calls, uint64_t *sectors,
                 uint64_t *ns, uint64_t *failures);

// "read", "write", "flush", "trim" -- or "?" out of range.
const char *blk_stat_op_name(int op);

#endif
