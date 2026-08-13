// Failure-injection counters -- see kernel/include/kernel/fault_inject.h
// for what this is for and why it's always compiled in.
//
// Deliberately dumb: three counters and no policy. Anything cleverer
// (fail every Nth call, fail a specific LBA, fail randomly) can be built
// on top by a test that arms and disarms around the specific call it
// cares about, and none of it belongs in the injection points
// themselves, which sit on hot paths.
#include "fault_inject.h"

static uint32_t g_ata_writes;
static uint32_t g_ata_reads;
static uint32_t g_allocs;

void fault_fail_next_ata_writes(uint32_t count) { g_ata_writes = count; }
void fault_fail_next_ata_reads(uint32_t count)  { g_ata_reads = count; }
void fault_fail_next_allocs(uint32_t count)     { g_allocs = count; }

int fault_any_armed(void) {
    return g_ata_writes != 0 || g_ata_reads != 0 || g_allocs != 0;
}

static int consume(uint32_t *counter) {
    if (*counter == 0) return 0;
    (*counter)--;
    return 1;
}

int fault_should_fail_ata_write(void) { return consume(&g_ata_writes); }
int fault_should_fail_ata_read(void)  { return consume(&g_ata_reads); }
int fault_should_fail_alloc(void)     { return consume(&g_allocs); }
