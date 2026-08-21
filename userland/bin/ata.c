// ata -- which transfer path the disk is using, and forcing the fallback.
//
//   ata                 DMA or PIO, and whether that is a choice
//   ata nodma on|off    force the PIO fallback / allow DMA again
//
// THREE STATES, NOT TWO, and the distinction is the whole reason this
// prints prose rather than a flag. "This controller has no Bus-Master
// DMA" and "DMA works and is switched off" look identical from a
// throughput number and mean completely different things.
//
// The forcing exists because the PIO path is otherwise UNREACHABLE: DMA
// comes up on every machine this OS boots, so the fallback driver never
// runs and cannot be tested. It also root-caused a real DMA failure
// once, by making the two comparable without editing the driver.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include <stdio.h>
#include <string.h>
#include "lib/tunable.h"

#define NODMA_TUNABLE "kernel.ata_nodma"

static int show(void) {
    struct query_ata a;
    if (sys_query_record(QUERY_ATA, 0, &a, sizeof a) < (int)sizeof a) {
        cmd_fail("ata", 0);
        return 1;
    }
    if (!(a.flags & QUERY_ATA_PRESENT)) {
        // A real answer: this machine may be booting off virtio-blk,
        // which is the preferred disk (docs/conventions/storage.md).
        sys_print("ata: no ATA drive present\n");
        return 0;
    }

    char line[192], cap[16];
    human_size(cap, sizeof cap, a.sector_count * 512ull);
    sys_print("ata: transfers are going through ");
    if (a.flags & QUERY_ATA_DMA_ON) {
        sys_print("DMA\n");
    } else if (!(a.flags & QUERY_ATA_DMA_HW)) {
        sys_print("PIO (this machine has no Bus-Master DMA)\n");
    } else {
        sys_print("PIO (forced -- `ata nodma off` to restore DMA)\n");
    }
    snprintf(line, sizeof line, "  max sectors/transfer: %llu\n  capacity: %llu sectors (%s)\n",
             (unsigned long long)a.max_sectors_xfer,
             (unsigned long long)a.sector_count, cap);
    sys_print(line);
    // TRIM is about what happens to the HOST IMAGE, not to throughput,
    // which is why it is stated rather than left as a flag: with it in
    // use a deleted file's blocks are actually discarded and disk.img
    // shrinks, and without it the image only ever grows.
    sys_print((a.flags & QUERY_ATA_TRIM)
              ? "  TRIM: in use -- freed blocks are discarded to the host image\n"
              : "  TRIM: not supported by this drive\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return show();

    if (strcmp(argv[1], "nodma") == 0) {
        char cur[64];
        if (argc == 2) {
            if (!tunable_get(NODMA_TUNABLE, cur, sizeof cur)) {
                cmd_fail("ata", NODMA_TUNABLE);
                return 1;
            }
            char line[64];
            snprintf(line, sizeof line, "ata nodma: %s\n", cur);
            sys_print(line);
            return 0;
        }
        if (argc == 3) {
            if (!tunable_set(NODMA_TUNABLE, argv[2])) {
                // TWO REASONS THIS FAILS, and they are worth telling
                // apart: a value that is not on/off, and a driver that
                // refuses right now because a non-blocking transfer is
                // in flight (switching would strand its poller).
                if (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0) {
                    sys_print("ata: nodma takes `on` or `off`\n");
                } else {
                    sys_print("ata: refused -- a transfer is in flight; try again\n");
                }
                return 1;
            }
            return show();
        }
    }

    cmd_usage("ata | ata nodma [on|off]");
    return 1;
}
