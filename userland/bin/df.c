// df -- how much of the filesystem is used.
//
// The numbers are SYS_SYSINFO's, which already carried them: `total` is
// usable DATA space, with the superblock, journal, bitmaps and record
// table excluded, so it answers "how much can I actually store" rather
// than "how big is the disk" -- the same framing df gives on a real
// system. See fs.h's fs_disk_usage().
//
// SYS_INFO_DISK_VALID is checked rather than assumed: with no mounted
// filesystem the two byte counts are not small, they are MEANINGLESS,
// and printing "0B used of 0B" would read as an empty disk rather than
// as no disk.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/stdio.h"

// 1.2K / 4.0M, integer only -- there is no floating point in ring 3
// either (-mno-sse), so the tenth comes out of the remainder. Same shape
// /bin/ls's -h uses.
static void put_size(char *out, unsigned long cap, unsigned long long n) {
    static const char unit[] = { 'B', 'K', 'M', 'G' };
    int u = 0;
    unsigned long long whole = n, rem = 0;
    while (whole >= 1024 && u < 3) {
        rem = whole % 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0) snprintf(out, cap, "%lluB", whole);
    else snprintf(out, cap, "%llu.%llu%c", whole, (rem * 10) / 1024, unit[u]);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    struct sys_info si;
    if (sys_sysinfo(&si) < 0) {
        cmd_fail("df", 0);
        return 1;
    }

    if (!(si.flags & SYS_INFO_DISK_VALID)) {
        sys_print("df: no filesystem is mounted\n");
        return 1;
    }
    unsigned long long used = si.disk_used_bytes, total = si.disk_total_bytes;
    unsigned long long free_b = total > used ? total - used : 0;
    char u[16], t[16], f[16], line[128];
    put_size(u, sizeof u, used);
    put_size(t, sizeof t, total);
    put_size(f, sizeof f, free_b);
    // Percent computed on the SMALLER side first so a multi-gigabyte
    // volume does not overflow the multiply before the divide.
    unsigned long long pct = total ? (used * 100) / total : 0;
    snprintf(line, sizeof line, "%-8s %-8s %-8s %-5s\n", "size", "used", "free", "use%");
    sys_print(line);
    snprintf(line, sizeof line, "%-8s %-8s %-8s %llu%%\n", t, u, f, pct);
    sys_print(line);
    return 0;
}
