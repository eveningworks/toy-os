// mount -- attach a filesystem at a path, or list what is attached.
//
// With no arguments it LISTS, which is what `mount` does on every Unix
// and is the form people actually type. The list is read from
// QUERY_FSINFO, the same record `df` formats differently -- one fact,
// two views, and no syscall of its own for listing.
//
// THE SOURCE IS A PARTITION NUMBER, NOT A PATH. toy-os has no /dev, so
// there is nothing to name a volume with; `mount 2 /boot` means "the
// second partition of the boot disk". `parttable` prints the numbers.
// A backend that needs no volume is named directly instead
// (`mount -t ramfs none /mnt`), which is how a scratch filesystem is
// made. See abi/mount_abi.h.
//
// WHAT IT DELIBERATELY DOES NOT DO: no /etc/fstab, no `-a`, no
// remount-in-place, no per-filesystem option string. The first three
// want a config file this OS does not have; the fourth wants options no
// backend here has. To change a mount's flags: `umount` it and mount it
// again, which is two commands and no new mechanism.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include <stdio.h>
#include <string.h>

static const char *USAGE =
    "mount [-r] [-t <fstype>] <device|partition|none> <mountpoint>\n"
    "       mount                  list what is mounted";

static int list(void) {
    struct query_fsinfo fs;
    char line[160];
    snprintf(line, sizeof line, "%-8s %-14s %-8s %s\n",
             "device", "on", "type", "options");
    sys_print(line);

    for (int i = 0; ; i++) {
        int n = sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs);
        if (n <= 0) break;
        if (!(fs.flags & QUERY_FS_MOUNTED)) continue;
        snprintf(line, sizeof line, "%-8s %-14s %-8s %s%s\n",
                 fs.device[0] ? fs.device : "-", fs.point, fs.name,
                 (fs.flags & QUERY_FS_RDONLY) ? "ro" : "rw",
                 (fs.flags & QUERY_FS_PERSISTENT) ? "" : ",volatile");
        sys_print(line);
    }
    return 0;
}

int main(int argc, char **argv) {
    struct mount_request req;
    memset(&req, 0, sizeof req);

    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            req.flags |= SYS_MNT_RDONLY;
        } else if (strcmp(argv[i], "-t") == 0) {
            if (++i >= argc) { cmd_usage(USAGE); return 1; }
            snprintf(req.fstype, sizeof req.fstype, "%s", argv[i]);
        } else {
            cmd_usage(USAGE);
            return 1;
        }
    }

    if (i == argc) return list();
    if (argc - i != 2) { cmd_usage(USAGE); return 1; }

    // `none` is the conventional Unix spelling for "this filesystem has
    // no volume", and it reads better in a command line than an empty
    // argument would.
    if (strcmp(argv[i], "none") != 0) {
        snprintf(req.source, sizeof req.source, "%s", argv[i]);
    }
    snprintf(req.point, sizeof req.point, "%s", argv[i + 1]);

    if (sys_mount(&req) < 0) {
        cmd_fail("mount", argv[i + 1]);
        return 1;
    }
    return 0;
}
