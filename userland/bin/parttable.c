// parttable -- the attached disk's MBR/GPT partition table.
//
// READ-ONLY, and diagnostic. This repo's stock disk.img has NO
// partition table at all -- one raw filesystem volume -- so "none" is
// the normal answer here rather than a failure, and saying that clearly
// is most of the program's job. `mkpart` (tools/) is what writes one.
//
// TWO CLASSES, because a list alone cannot tell "a table with no
// partitions" from "no table at all": both are zero records, and the
// second is the common case. QUERY_PARTTABLE answers which kind;
// QUERY_PARTITION is the entries. See kernel/drivers/partition_query.c.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include "lib/stdio.h"

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

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct query_parttable t;
    int n = sys_query_record(QUERY_PARTTABLE, 0, &t, sizeof t);
    if (n < (int)sizeof t) {
        cmd_fail("parttable", 0);
        return 1;
    }

    char line[192], guid[40], size[16];

    if (t.kind == QUERY_PART_NONE) {
        sys_print("No partition table on this disk.\n"
                  "  (a raw filesystem volume, which is what this OS ships --\n"
                  "   see tools/mkpart_test.py for writing one)\n");
        return 0;
    }

    snprintf(line, sizeof line, "%s partition table, %llu entr%s\n",
             kind_name(t.kind), (unsigned long long)t.entry_count,
             t.entry_count == 1 ? "y" : "ies");
    sys_print(line);
    if (t.kind == QUERY_PART_GPT) {
        put_guid(guid, sizeof guid, t.disk_guid);
        snprintf(line, sizeof line, "  disk GUID: %s\n", guid);
        sys_print(line);
    }

    // The loop ends on the record that is NOT THERE rather than on
    // `entry_count`: a list's length is itself a fact and can change
    // between reads. Same rule every other list consumer here follows.
    for (unsigned i = 0; ; i++) {
        struct query_partition p;
        if (sys_query_record(QUERY_PARTITION, i, &p, sizeof p) < (int)sizeof p) break;
        human_size(size, sizeof size, p.lba_count * (unsigned long long)SECTOR_BYTES);
        if (p.kind == QUERY_PART_GPT) {
            put_guid(guid, sizeof guid, p.type_guid);
            snprintf(line, sizeof line, "  %u: lba %llu +%llu  %-8s  %s\n",
                     i + 1, (unsigned long long)p.lba_start,
                     (unsigned long long)p.lba_count, size,
                     p.name[0] ? p.name : "(unnamed)");
            sys_print(line);
            snprintf(line, sizeof line, "     type %s\n", guid);
            sys_print(line);
        } else {
            snprintf(line, sizeof line, "  %u: lba %llu +%llu  %-8s  type 0x%02llx\n",
                     i + 1, (unsigned long long)p.lba_start,
                     (unsigned long long)p.lba_count, size,
                     (unsigned long long)p.mbr_type);
            sys_print(line);
        }
    }
    return 0;
}
