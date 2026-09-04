// sync -- flush the disk write-back cache, and REPORT WHETHER IT WORKED.
//
// The whole reason this prints anything: with a write-back cache in
// front of the disk, a write that returned success can be refused later,
// at the flush (see ata_cache.c and CLAUDE.md). "The flush failed and
// your data is still only in RAM" is the one disk answer a caller must
// not read as success, so it is loud and it says not to power off.
#include "rt/sys.h"
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int wrote = sys_sync();
    if (wrote < 0) {
        sys_print("sync: FAILED -- some sectors could NOT be written and\n"
                  "      are still in memory only. Do not power off yet.\n");
        return 1;
    }
    char msg[64];
    // ZERO IS NOT "DID NOTHING". Only the ATA path has a software cache
    // that can hold sectors in RAM; everywhere else the count is
    // legitimately zero and the real work was the device flush every
    // mounted volume just took. Saying "nothing was pending" there
    // reported a working sync as a no-op, which is what it looked like
    // on the machine that needed it most.
    snprintf(msg, sizeof msg, "sync: %s\n",
             wrote == 0 ? "flushed every mounted disk (nothing was buffered in RAM)"
                        : "");
    if (wrote > 0)
        snprintf(msg, sizeof msg,
                 "sync: wrote %d sector%s back, then flushed every mounted disk\n",
                 wrote, wrote == 1 ? "" : "s");
    sys_print(msg);
    return 0;
}
