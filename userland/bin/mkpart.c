// mkpart -- write an MBR or GPT partition table to the disk.
//
// DESTRUCTIVE, and the counterpart to `parttable`, which reads one.
// The kernel does the encoding (SYS_MKPART); this program's whole job
// is turning a command line into a `struct mkpart_request` and being
// clear about what is about to be destroyed.
//
// WHAT IT DELIBERATELY DOES NOT DO:
//
//   - It does not FORMAT anything. A partition is a range of sectors;
//     putting a filesystem in one is `fsformat`, at the next boot,
//     once the partition is the mounted volume. Two verbs, because
//     they are two decisions -- `fdisk` and `mkfs` are separate on
//     Linux for the same reason.
//   - It does not remount, and the running system does not change.
//     The new table takes effect at the NEXT BOOT. Linux is the same:
//     the kernel refuses to re-read a table on a busy disk.
//   - It has no interactive mode. `fdisk`'s prompt-driven editor is a
//     lot of program for a machine with one disk; a command line that
//     can be typed once and read back later is a better fit, and it is
//     what a test harness can drive.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SECTOR_BYTES 512

// The first LBA a partition may start at, per table kind. Below this
// lies the table itself -- LBA 0 for an MBR, LBA 0..33 for a GPT
// (header at 1, then 32 sectors of entry array). The kernel enforces
// this too; repeating it here is what lets the DEFAULT start be right
// rather than rejected.
#define MBR_FIRST_USABLE 2048  // the 1 MiB alignment every modern tool uses
#define GPT_FIRST_USABLE 2048  // ...comfortably past LBA 33 as well

static const char *USAGE =
    "mkpart [--disk <name>] [--mbr|--gpt] <size>[K|M|G]|rest [<size>|rest ...] confirm\n"
    "  --disk <name>  which disk (`lsblk`); default is the one this machine booted\n"
    "  --gpt          write a GPT (default)\n"
    "  --mbr          write a legacy MBR -- at most 4 partitions\n"
    "  <size>         one partition of that size; `rest` takes what is left\n"
    "  confirm        required -- this destroys the disk's current contents";

// Parses "512M", "2G", "204800" (bare = sectors) into sectors.
// Returns 0 on anything it does not fully understand -- a parser
// REJECTS rather than guesses, and a misread size here is a partition
// somewhere other than where it was asked for.
static uint64_t parse_size(const char *s, uint64_t *out) {
    if (!s || !*s) return 0;
    uint64_t n = 0;
    const char *p = s;
    for (; *p >= '0' && *p <= '9'; p++) {
        uint64_t next = n * 10 + (uint64_t)(*p - '0');
        if (next < n) return 0; // overflow
        n = next;
    }
    if (p == s) return 0; // no digits at all

    uint64_t mult = 1;
    if (*p == 'K' || *p == 'k') { mult = 1024ull; p++; }
    else if (*p == 'M' || *p == 'm') { mult = 1024ull * 1024; p++; }
    else if (*p == 'G' || *p == 'g') { mult = 1024ull * 1024 * 1024; p++; }
    if (*p != '\0') return 0; // trailing junk

    // A bare number is SECTORS; a suffixed one is bytes. Sizes are
    // rounded DOWN to a whole sector -- rounding up would be a
    // partition slightly larger than asked for, which is how the last
    // one runs off the end of the disk.
    *out = (mult == 1) ? n : (n * mult) / SECTOR_BYTES;
    return *out != 0;
}

// How many sectors a disk has, so `rest` and the bounds checks mean
// something before the syscall refuses them. The BOOT disk's comes from
// QUERY_PARTTABLE, which carries it (kernel/drivers/partition_query.c);
// a named one comes from QUERY_BLKDEV, which is the only place another
// disk's size is reported. Returns 0 for a name that is not a whole
// disk, which the syscall would refuse anyway.
static uint64_t disk_sectors_of(const char *name) {
    if (!name) {
        struct query_parttable t;
        if (sys_query_record(QUERY_PARTTABLE, 0, &t, sizeof t) < (int)sizeof t) return 0;
        return t.disk_sectors;
    }
    struct query_blkdev b;
    for (int i = 0; sys_query_record(QUERY_BLKDEV, i, &b, sizeof b) >= (int)sizeof b; i++) {
        if (strcmp(b.name, name) != 0) continue;
        if (b.parent[0]) return 0; // a partition, not a disk
        return b.sectors;
    }
    return 0;
}

int main(int argc, char **argv) {
    struct mkpart_request req;
    memset(&req, 0, sizeof req);
    req.kind = MKPART_KIND_GPT;

    // --disk has to be read BEFORE the geometry, because the geometry
    // is that disk's.
    const char *disk_name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--disk") != 0) continue;
        if (i + 1 >= argc) { cmd_fail("mkpart", "--disk needs a device name"); return 1; }
        disk_name = argv[i + 1];
    }

    uint64_t disk = disk_sectors_of(disk_name);
    if (disk == 0) {
        cmd_fail("mkpart", disk_name ? "no such disk" : "no disk");
        return 1;
    }
    if (disk_name) snprintf(req.device, sizeof req.device, "%s", disk_name);

    int confirmed = 0;
    uint64_t sizes[MKPART_MAX_ENTRIES];
    int rest_at = -1; // index of the `rest` partition, if any
    unsigned n = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--disk") == 0) { i++; continue; }  // read above, with its value
        if (strcmp(a, "--gpt") == 0) { req.kind = MKPART_KIND_GPT; continue; }
        if (strcmp(a, "--mbr") == 0) { req.kind = MKPART_KIND_MBR; continue; }
        if (strcmp(a, "confirm") == 0) { confirmed = 1; continue; }

        if (n >= MKPART_MAX_ENTRIES) {
            cmd_fail("mkpart", "at most 4 partitions");
            return 1;
        }
        if (strcmp(a, "rest") == 0) {
            if (rest_at >= 0) {
                cmd_fail("mkpart", "only one partition can be `rest`");
                return 1;
            }
            rest_at = (int)n;
            sizes[n++] = 0; // filled in below, once the others are known
            continue;
        }
        if (!parse_size(a, &sizes[n])) {
            cmd_fail("mkpart", a);
            return 1;
        }
        n++;
    }

    if (n == 0) { cmd_usage(USAGE); return 1; }
    if (!confirmed) {
        // The word is the whole point: a table write is unrecoverable
        // and there is no undo anywhere in this OS. Same shape as
        // `fsformat tfs3 confirm`.
        char total[16];
        human_size(total, sizeof total, disk * SECTOR_BYTES);
        fprintf(stderr, "mkpart: this ERASES the partition layout of the disk "
                        "(%s), and any filesystem in it.\n", total);
        fprintf(stderr, "mkpart: re-run with `confirm` as the last argument if that is what you want.\n");
        return 1;
    }
    if (req.kind == MKPART_KIND_MBR && n > 4) {
        cmd_fail("mkpart", "an MBR holds at most 4 partitions -- use --gpt");
        return 1;
    }

    // Lay them out end to end from the first usable sector. No gaps and
    // no reordering: what was typed is what is written, in order, which
    // is the only layout a person can predict from the command line.
    uint64_t first = (req.kind == MKPART_KIND_GPT) ? GPT_FIRST_USABLE : MBR_FIRST_USABLE;
    // The tail GPT reserves for its backup header and entry array. Left
    // out for an MBR, which has no backup.
    uint64_t tail = (req.kind == MKPART_KIND_GPT) ? 33 : 0;
    uint64_t usable = (disk > first + tail) ? disk - first - tail : 0;

    uint64_t fixed = 0;
    for (unsigned i = 0; i < n; i++) {
        if ((int)i == rest_at) continue;
        fixed += sizes[i];
    }
    if (fixed > usable) {
        cmd_fail("mkpart", "the requested partitions do not fit on this disk");
        return 1;
    }
    if (rest_at >= 0) {
        sizes[rest_at] = usable - fixed;
        if (sizes[rest_at] == 0) {
            cmd_fail("mkpart", "`rest` has nothing left over");
            return 1;
        }
    }

    uint64_t at = first;
    for (unsigned i = 0; i < n; i++) {
        req.entries[i].start_lba = at;
        req.entries[i].sectors = sizes[i];
        snprintf(req.entries[i].name, sizeof req.entries[i].name, "toyos%u", i + 1);
        at += sizes[i];
    }
    req.count = n;
    req.flags = MKPART_CONFIRM;

    int rc = sys_mkpart(&req);
    if (rc < 0) {
        // The kernel logs the specific reason; repeat the errno here so
        // the person who typed the command sees something without
        // reaching for `dmesg`.
        cmd_fail("mkpart", rc == -1 ? "refused" : "failed");
        return 1;
    }

    char size[16];
    printf("%s partition table written:\n", req.kind == MKPART_KIND_GPT ? "GPT" : "MBR");
    for (unsigned i = 0; i < n; i++) {
        human_size(size, sizeof size, req.entries[i].sectors * SECTOR_BYTES);
        printf("  %u  LBA %llu  %llu sectors  %s\n", i + 1,
               (unsigned long long)req.entries[i].start_lba,
               (unsigned long long)req.entries[i].sectors, size);
    }
    if (disk_name) {
        // The kernel re-read the table because nothing is mounted from
        // it, so the windows are already devices.
        printf("%sp1.. are ready now -- `mkfs %sp1 confirm` to put a filesystem in one.\n",
               disk_name, disk_name);
    } else {
        printf("Reboot for this to take effect, then `fsformat tfs3 confirm` to put a\n"
               "filesystem in the first partition.\n");
    }
    return 0;
}
