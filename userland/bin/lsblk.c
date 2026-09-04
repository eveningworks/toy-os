// lsblk -- every block device this boot found, disks and partitions.
//
// WHAT IT IS FOR. Every disk driver runs at boot now and registers what
// it finds into a table (kernel/include/kernel/block.h); the root is a
// SEPARATE choice, made by `root=` on the boot line or by driver
// precedence. This is how a person sees the first half of that -- which
// devices exist, what they are called, and which one the root is on.
//
// The names it prints are the names `root=` and `mount` take, and that
// is the whole point of printing them: `mount ahci0p1 /mnt` is not
// something anyone can guess without this.
//
// WHY IT IS NOT `parttable`. That reads a table off a DISK and reports
// what is written there; this reports what the KERNEL found and named.
// They disagree usefully -- a partition whose window the kernel refused
// appears in one and not the other -- and merging them would lose that.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/human.h"
#include <stdio.h>

#define SECTOR_BYTES 512


int main(void) {
    char line[160], size[32];
    int n = 0;

    sys_print("NAME        SIZE      TYPE   ROOT  WHERE\n");

    // The loop ends on the record that is NOT THERE rather than on a
    // count read first: a list's length is itself a fact and can change
    // between reads. Same rule every other list consumer here follows.
    struct query_blkdev d;
    QUERY_FOREACH(QUERY_BLKDEV, d, i) {
        n++;

        human_size(size, sizeof size, d.sectors * (unsigned long long)SECTOR_BYTES);

        // A partition says where on its disk it starts, because that is
        // the number that makes it identifiable against `parttable`'s
        // output; a whole disk has nothing useful to say there.
        char where[64];
        if (d.parent[0])
            snprintf(where, sizeof where, "%s at lba %llu",
                     d.parent, (unsigned long long)d.base_lba);
        else
            snprintf(where, sizeof where, "%s",
                     d.persistent ? "disk" : "memory (not persistent)");

        snprintf(line, sizeof line, "%-11s %-9s %-6s %-5s %s\n",
                 d.name, size, d.parent[0] ? "part" : "disk",
                 d.is_root ? "yes" : "", where);
        sys_print(line);
    }

    // Never a silent empty table. A machine with no disk at all is a
    // real and supported state (the root is ramfs then), and saying so
    // is different from printing a header and stopping, which reads
    // like the command failed.
    if (n == 0)
        sys_print("no block devices -- this boot found no disk\n");

    return 0;
}
