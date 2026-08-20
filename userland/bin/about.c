// about -- what this system is.
//
// The version string is TOYOS_VERSION_FULL, not TOYOS_VERSION: on a dev
// build that carries the commit id (and "-dirty" when the tree did not
// match it), which is the only way to tell which build an image
// actually is. A release shows the bare number, since its tag pins it.
// Same call userland/gui/system/about.c makes, for the same reason.
//
// The filesystem line reads QUERY_FSINFO. It could not, until that
// provider existed -- fs_backend_name() and fs_is_persistent() are
// kernel calls with no syscall behind them, which is why `about` was a
// builtin at all. The GUI About window still omits this line and is a
// roadmap item; this one has it.
#include "rt/sys.h"
#include "lib/stdio.h"
#include "version.h"   // TOYOS_VERSION_FULL, generated -- tools/gen_version.sh

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    char line[160];
    snprintf(line, sizeof line,
             "toy-os v%s -- a small x86-64 hobby kernel\n", TOYOS_VERSION_FULL);
    sys_print(line);
    sys_print("Boot: GRUB/Multiboot2 | C + ASM | Tested on QEMU\n");

    struct query_fsinfo fs;
    int n = sys_query_record(QUERY_FSINFO, 0, &fs, sizeof fs);
    if (n < (int)sizeof fs || !(fs.flags & QUERY_FS_MOUNTED)) {
        // A real state, not a failure: a boot that found no disk has no
        // filesystem, and saying so is the answer.
        sys_print("Storage: none mounted\n");
        return 0;
    }
    snprintf(line, sizeof line, "Storage: %s, %s\n", fs.name,
             (fs.flags & QUERY_FS_PERSISTENT)
                 ? "disk-backed (files persist across reboots)"
                 : "RAM only (no disk found -- files won't survive a reboot)");
    sys_print(line);
    return 0;
}
