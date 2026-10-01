// df -- how much of the filesystem is used, and WHICH filesystem.
//
// It reads QUERY_FSINFO, one record, and that is the whole design.
//
// This used to read SYS_SYSINFO, which carried the byte counts and
// nothing about the filesystem they described -- so the kernel shell
// kept a `df` builtin in front of this program purely to print the
// backend name, and two implementations of one command existed because
// of one string. The provider (kernel/fs/fs_query.c) closed that, and
// the builtin is gone.
//
// ONE RECORD PER MOUNT, NOT TWO READS. The name and the numbers arrive
// together on purpose: a `used` sampled at one moment beside a `total`
// sampled at another describes no filesystem that ever existed, and on
// a live disk they genuinely differ.
//
// EVERY MOUNT, since Real mount points. It printed one line while the
// provider was a scalar; a machine with a TFS3 root and a FAT32 /boot
// has two filesystems with different sizes, different fullness and
// different persistence, and one line for them is not a summary, it is
// the wrong answer for one of them.
//
// `total` is usable DATA space -- superblock, journal, bitmaps and
// record table excluded -- so it answers "how much can I actually
// store" rather than "how big is the disk", the framing df gives on a
// real system. See fs.h's fs_disk_usage().
//
// QUERY_FS_MOUNTED is checked rather than assumed: with nothing
// mounted the two counts are not small, they are MEANINGLESS, and
// "0B used of 0B" reads as an empty disk rather than as no disk.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include <stdio.h>
#include <locale.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setlocale(LC_ALL, "");

    struct query_fsinfo fs;
    char u[16], t[16], f[16], line[200];
    int shown = 0;

    snprintf(line, sizeof line, "%-10s %-12s %-8s %-8s %-8s %-5s %s\n",
             "filesystem", "on", "size", "used", "free", "use%", "persists");
    sys_print(line);

    for (int i = 0; ; i++) {
        int n = sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs);
        if (n <= 0) break;
        // The ABI promises min(len, record), so a kernel whose struct is
        // SHORTER than this build expects would leave the tail reading
        // as zeroes -- which for `name` is an empty string and for
        // `flags` is "not mounted". Checked rather than assumed; this is
        // the version tolerance being used rather than merely
        // documented.
        if ((unsigned)n < sizeof fs) {
            sys_print("df: this kernel reports fewer fields than expected\n");
            return 1;
        }
        if (!(fs.flags & QUERY_FS_MOUNTED)) continue;

        unsigned long long used = fs.used_bytes, total = fs.total_bytes;
        unsigned long long free_b = total > used ? total - used : 0;
        human_size(u, sizeof u, used);
        human_size(t, sizeof t, total);
        human_size(f, sizeof f, free_b);
        // Percent computed on the SMALLER side first so a multi-gigabyte
        // volume does not overflow the multiply before the divide.
        unsigned long long pct = total ? (used * 100) / total : 0;

        snprintf(line, sizeof line, "%-10s %-12s %-8s %-8s %-8s %llu%%%s %s\n",
                 fs.name, fs.point, t, u, f, pct,
                 pct < 10 ? "  " : (pct < 100 ? " " : ""),
                 (fs.flags & QUERY_FS_PERSISTENT) ? "yes" : "no -- RAM only");
        sys_print(line);
        shown++;
    }

    if (!shown) {
        sys_print("df: no filesystem is mounted\n");
        return 1;
    }
    return 0;
}
