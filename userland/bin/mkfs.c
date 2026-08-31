// mkfs -- put an empty filesystem on a partition.
//
//     mkfs [-t <type>] <partition> confirm
//
// THE INSTALLER'S OPERATION, and deliberately not `fsformat`. That one
// reformats the volume this machine is RUNNING FROM: it unmounts
// everything, formats, and re-probes the world. This one writes a
// filesystem onto some OTHER partition and disturbs nothing.
//
// AND IT CANNOT ACTUALLY RUN YET. A backend keeps its volume in
// module-level state, so formatting ANY device repoints whatever is
// mounted -- both through the target's own format() and through
// mount_wipe_others(), which wipes every OTHER backend against the same
// device. Measured twice, each time as a crash that took /bin with it.
// So the kernel refuses while any disk-backed filesystem is mounted,
// and no boot mode today leaves them all unmounted. What ships here is
// the syscall, the checks and the refusal; making it USEFUL needs
// per-volume state in the backends, which docs/bugs.md records as the
// installer's real prerequisite.
//
// `confirm` IS A WORD YOU TYPE, not a flag, following `fsformat tfs3
// confirm` and SYS_MKPART's MKPART_CONFIRM. There is no privilege model
// in this OS to gate a destructive storage operation with, so the stand-
// in is that the person asking spells it out. It is a speed bump, and
// the ABI comment says so rather than letting anybody mistake it for a
// permission check.
#include <stdint.h>
#include "rt/sys.h"
#include "mount_abi.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

#define USAGE "mkfs [-t <type>] <partition> confirm"

int main(int argc, char **argv) {
    const char *type = "tfs3";
    const char *part = 0;
    int confirmed = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) type = argv[++i];
        else if (!strcmp(argv[i], "confirm")) confirmed = 1;
        else if (argv[i][0] == '-') { cmd_usage(USAGE); return 1; }
        else if (!part) part = argv[i];
        else { cmd_usage(USAGE); return 1; }
    }
    if (!part) { cmd_usage(USAGE); return 1; }

    if (!confirmed) {
        printf("mkfs: this ERASES %s. Add the word `confirm` if that is "
               "what you want:\n    mkfs -t %s %s confirm\n", part, type, part);
        return 1;
    }

    struct mkfs_request req;
    memset(&req, 0, sizeof req);
    strlcpy(req.source, part, sizeof req.source);
    strlcpy(req.fstype, type, sizeof req.fstype);
    req.flags = MKFS_CONFIRM;

    if (sys_mkfs(&req) < 0) {
        // THE REASON IS IN sys_errno(), NOT IN THE RETURN VALUE. Every
        // wrapper here converts a negative errno to -1 and stashes the
        // code (rt/sys.c's err()), so comparing the return against
        // -EINVAL matches nothing -- and -1 IS -EPERM, so a chain of
        // such comparisons reports every failure as the LAST thing it
        // happens to equal. This one printed "refused without
        // confirmation" for a partition that did not exist.
        int e = sys_errno();
        const char *why =
            e == EBUSY  ? "a disk-backed filesystem is mounted.\n"
                          "       Formatting anything now would repoint that backend --\n"
                          "       they keep their volume in module-level state -- so it is\n"
                          "       refused. NOTE: no boot mode today leaves them all\n"
                          "       unmounted, so this is not reachable yet. See docs/bugs.md"
          : e == EINVAL ? "no such partition, or no such filesystem type"
          : e == EPERM  ? "refused without confirmation"
          : e == EIO    ? "the format itself failed -- see dmesg"
          : strerror(e);
        printf("mkfs: %s: %s\n", part, why);
        return 1;
    }
    printf("mkfs: %s is now an empty %s filesystem\n", part, type);
    return 0;
}
