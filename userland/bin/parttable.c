// parttable -- a disk's MBR/GPT partition table: the boot disk's, or the one named.
//
// READ-ONLY. A blank disk has NO partition table, so "none" is an
// answer rather than a failure, and says so. `/bin/mkpart` writes one.
//
// It reads the WHOLE DISK, not the mounted volume, so it answers the
// same way whether the running system booted flat or from inside a
// partition (kernel/drivers/partition.c goes through
// blk_disk_read_sectors for exactly that reason).
//
// TWO CLASSES, because a list alone cannot tell "a table with no
// partitions" from "no table at all": both are zero records, and the
// second is the common case. QUERY_PARTTABLE answers which kind;
// QUERY_PARTITION is the entries. See kernel/drivers/partition_query.c.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include "lib/uargs.h"
#include <string.h>
#include <stdio.h>

#define SECTOR_BYTES 512

// A GUID in the mixed-endian form every tool prints: the first three
// groups are little-endian, the last two big-endian. Getting this
// wrong produces a plausible-looking GUID that matches nothing, which
// is why it is spelled out rather than looped.
static void put_guid(char *out, unsigned long cap, const uint8_t *g) {
    snprintf(out, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static const char *kind_name(unsigned long long k) {
    switch (k) {
    case QUERY_PART_MBR: return "MBR";
    case QUERY_PART_GPT: return "GPT";
    default:             return "none";
    }
}

static const struct uargs_prog PROG = {
    .name = "parttable",
    .usage = "[DISK]",
    .summary = "Print a disk's partition table: the boot disk's, or DISK's (`lsblk` names them).",
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc > 1) return uargs_error(&PROG, "one disk at most");

    // One record per whole disk, the boot disk's first.
    struct query_parttable t;
    int found = 0;
    QUERY_FOREACH(QUERY_PARTTABLE, t, k) {
        if (a.argc == 0 || !strcmp(t.disk, a.argv[0])) { found = 1; break; }
    }
    if (!found) {
        if (a.argc) return uargs_error(&PROG, "%s is not a disk (`lsblk` lists them)", a.argv[0]);
        cmd_fail("parttable", 0);
        return 1;
    }

    char line[192], guid[40], size[16];

    if (t.kind == QUERY_PART_NONE) {
        sys_print("No partition table on this disk.\n"
                  "  (a raw filesystem volume, which is what this OS ships --\n"
                  "   `mkpart` writes one; see `help mkpart`)\n");
        return 0;
    }

    snprintf(line, sizeof line, "%s partition table, %llu entr%s\n",
             kind_name(t.kind), (unsigned long long)t.entry_count,
             t.entry_count == 1 ? "y" : "ies");
    sys_print(line);
    // Only said when it is not 512, and then with the unit the LBAs
    // below are in -- which is still 512-byte sectors, not the disk's.
    if (t.block_size > SECTOR_BYTES) {
        snprintf(line, sizeof line, "  %llu-byte sectors (LBAs below count 512 bytes)\n",
                 (unsigned long long)t.block_size);
        sys_print(line);
    }
    if (t.kind == QUERY_PART_GPT) {
        put_guid(guid, sizeof guid, t.disk_guid);
        snprintf(line, sizeof line, "  disk GUID: %s\n", guid);
        sys_print(line);
    }

    // The loop ends on the record that is NOT THERE rather than on
    // `entry_count`: a list's length is itself a fact and can change
    // between reads. Same rule every other list consumer here follows.
    struct query_partition p;
    QUERY_FOREACH(QUERY_PARTITION, p, i) {
        if (strcmp(p.disk, t.disk) != 0) continue;   // every disk's are listed
        human_size(size, sizeof size, p.lba_count * (unsigned long long)SECTOR_BYTES);
        if (p.kind == QUERY_PART_GPT) {
            put_guid(guid, sizeof guid, p.type_guid);
            snprintf(line, sizeof line, "  %u: lba %llu +%llu  %-8s  %s\n",
                     (unsigned)p.number, (unsigned long long)p.lba_start,
                     (unsigned long long)p.lba_count, size,
                     p.name[0] ? p.name : "(unnamed)");
            sys_print(line);
            snprintf(line, sizeof line, "     type %s\n", guid);
            sys_print(line);
        } else {
            snprintf(line, sizeof line, "  %u: lba %llu +%llu  %-8s  type 0x%02llx\n",
                     (unsigned)p.number, (unsigned long long)p.lba_start,
                     (unsigned long long)p.lba_count, size,
                     (unsigned long long)p.mbr_type);
            sys_print(line);
        }
    }
    return 0;
}
