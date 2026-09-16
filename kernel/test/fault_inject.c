// Failure-injection counters -- see kernel/include/kernel/fault_inject.h
// for what this is for and why it's always compiled in.
//
// Deliberately dumb: a counter per injection point and no policy. Anything cleverer
// (fail every Nth call, fail a specific LBA, fail randomly) can be built
// on top by a test that arms and disarms around the specific call it
// cares about, and none of it belongs in the injection points
// themselves, which sit on hot paths.
#include "fault_inject.h"
#include "scheduler.h"
#include "kfmt.h"
#include "klog.h"   // KLOG_ERR -- the level on a failure line

// WHO ARMED THE COUNTER, so a failure consumed by somebody else is
// visible: the counters are global, a ring-3 process can be scheduled
// between a test's arm and its call, and its write would then fail in
// the test's place. Logged, bounded, never seen in ~20 runs beside a
// live logd -- so no scoping (Linux's `fail*/task-filter`) until it is.
static int g_armer = -1;
static int g_foreign_logged;

static uint32_t g_ata_writes;
static uint32_t g_ata_reads;
// The BLOCK-LAYER pair, which is what a filesystem test wants: it sits
// at the seam every backend passes through, so it works whether the
// filesystem is on ATA, virtio-blk or a RAM image. The ATA pair below
// it reaches one backend only -- see fault_inject.h.
static uint32_t g_blk_writes;
static uint32_t g_blk_reads;
static uint32_t g_allocs;
static uint32_t g_usb_cmds;
static uint32_t g_blk_flush_skip, g_blk_flushes;

void fault_fail_next_ata_writes(uint32_t count) { if (count) g_armer = scheduler_current_pid(); g_ata_writes = count; }
void fault_fail_next_ata_reads(uint32_t count) { if (count) g_armer = scheduler_current_pid(); g_ata_reads = count; }
void fault_fail_next_block_writes(uint32_t count) { if (count) g_armer = scheduler_current_pid(); g_blk_writes = count; }
void fault_fail_next_block_reads(uint32_t count) { if (count) g_armer = scheduler_current_pid(); g_blk_reads = count; }
void fault_fail_next_usb_commands(uint32_t count) { if (count) g_armer = scheduler_current_pid(); g_usb_cmds = count; }
void fault_fail_next_allocs(uint32_t count) { if (count) g_armer = scheduler_current_pid(); g_allocs = count; }

void fault_fail_block_flushes(uint32_t skip, uint32_t count) {
    if (count) g_armer = scheduler_current_pid();
    g_blk_flush_skip = skip;
    g_blk_flushes = count;
}

int fault_any_armed(void) {
    return g_ata_writes != 0 || g_ata_reads != 0
        || g_blk_writes != 0 || g_blk_reads != 0
        || g_allocs != 0 || g_blk_flushes != 0 || g_usb_cmds != 0;
}

static int consume(uint32_t *counter) {
    if (*counter == 0) return 0;
    (*counter)--;
    int me = scheduler_current_pid();
    if (me != g_armer && g_foreign_logged < 16) {
        g_foreign_logged++;
        klog_printf(KLOG_ERR "fault: injected failure consumed by pid %d (armed by pid %d, %u left)\n",
                    me, g_armer, (unsigned)*counter);
    }
    return 1;
}

int fault_should_fail_ata_write(void) { return consume(&g_ata_writes); }
int fault_should_fail_ata_read(void)  { return consume(&g_ata_reads); }
int fault_should_fail_block_write(void) { return consume(&g_blk_writes); }
int fault_should_fail_block_read(void)  { return consume(&g_blk_reads); }
int fault_should_fail_alloc(void)     { return consume(&g_allocs); }
int fault_should_fail_usb_command(void) { return consume(&g_usb_cmds); }

int fault_should_fail_block_flush(void) {
    if (g_blk_flushes == 0) return 0;
    if (g_blk_flush_skip) { g_blk_flush_skip--; return 0; }
    return consume(&g_blk_flushes);
}
