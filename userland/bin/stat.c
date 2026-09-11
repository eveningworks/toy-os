// stat -- what the filesystem knows about one path.
//
// The inode line says whether the number is REAL or synthetic, because
// on a backend without inodes it is the record's table slot: stable, and
// meaningless to compare against anything but another stat of the same
// volume. Saying which is the difference between a fact and a number.
#include "rt/sys.h"
#include <time.h>
#include "lib/cmd.h"
#include <stdio.h>

// "MM/DD/YYYY HH:MM:SS" -- the kernel shell's `stat` shape, kept so the
// two commands do not disagree about how a timestamp looks while both
// exist. Zero-padded widths are kfmt's, which pads NUMBERS with zeroes
// (see CLAUDE.md) -- exactly what is wanted here and nowhere else.
// LOCALISED, like every other displayed timestamp: what the kernel
// stores is a UTC epoch (api/fs.h). This keeps its own spelling rather
// than using lib/udate.h's, so `stat` and the kernel shell agree while
// both exist -- which means it has to convert for itself.
static void put_time(const char *label, const struct rtc_time *t) {
    struct rtc_time l = *t;
    tz_localize(&l);
    char buf[64];
    snprintf(buf, sizeof buf, "  %s%02u/%02u/%04u %02u:%02u:%02u\n", label,
             l.month, l.day, l.year, l.hour, l.minute, l.second);
    sys_print(buf);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        cmd_usage("stat <path>");
        return 1;
    }
    struct sys_stat st;
    if (sys_stat(argv[1], &st) < 0) {
        cmd_fail("stat", argv[1]);
        return 1;
    }

    char buf[96];
    sys_print("  path:     "); sys_print(argv[1]); sys_print("\n");
    sys_print("  type:     ");
    sys_print(st.is_dir ? "directory\n" : "file\n");
    if (!st.is_dir) {
        snprintf(buf, sizeof buf, "  size:     %llu bytes\n",
                 (unsigned long long)st.size);
        sys_print(buf);
    }
    snprintf(buf, sizeof buf, "  inode:    %llu%s\n", (unsigned long long)st.ino,
             (st.flags & SYS_STAT_INODES) ? "" : " (synthetic)");
    sys_print(buf);
    put_time("created:  ", &st.created);
    put_time("modified: ", &st.modified);
    return 0;
}
