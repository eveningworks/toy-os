// about -- what this system is.
//
// IT REPORTS TWO VERSIONS, AND SAYS SO WHEN THEY DIFFER. The kernel's
// comes from QUERY_VERSION -- the running kernel's own copy. This
// program's comes from version.h at ITS compile time. Printing only the
// second is what this used to do, and it is right only while kernel and
// userland ship as one image: on a machine updated over the network a
// piece at a time they diverge, and the old code reported the version of
// /bin/about while calling it the kernel's. That is not hypothetical --
// a laptop here read 214d29e while running 083cf8e, and nothing on it
// could say otherwise.
//
// The version string is TOYOS_VERSION_FULL, not TOYOS_VERSION: on a dev
// build that carries the commit id (and "-dirty" when the tree did not
// match it), which is the only way to tell which build an image
// actually is. A release shows the bare number, since its tag pins it.
//
// The filesystem line reads QUERY_FSINFO. It could not, until that
// provider existed -- fs_backend_name() and fs_is_persistent() are
// kernel calls with no syscall behind them, which is why `about` was a
// builtin at all. The GUI About window still omits this line and is a
// roadmap item; this one has it.
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>
#include "version.h"     // TOYOS_VERSION*, generated -- tools/gen_version.sh
#include "build_date.h"  // TOYOS_BUILD_DATE -- the DAY this program was built

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    char line[160];
    snprintf(line, sizeof line,
             "toy-os v%s -- a small x86-64 hobby kernel\n", TOYOS_VERSION);
    sys_print(line);

    struct query_version kv;
    int have_kernel = sys_query_record(QUERY_VERSION, 0, &kv, sizeof kv)
                          >= (int)sizeof kv;
    if (have_kernel) {
        snprintf(line, sizeof line, "  kernel:   %s (%s)  built %s\n",
                 kv.version, kv.build_id, kv.stamp);
        sys_print(line);
    } else {
        // A kernel too old to carry the provider. Say which fact is
        // missing rather than printing nothing -- the absence IS the
        // answer, and it dates the kernel more precisely than silence.
        sys_print("  kernel:   (this kernel does not report its version)\n");
    }

    snprintf(line, sizeof line, "  userland: %s  built %s\n",
             TOYOS_VERSION_FULL, TOYOS_BUILD_DATE);
    sys_print(line);

    // THE COMPARISON IS ON THE BUILD ID, not the version: two builds of
    // 0.3.0-dev from different commits are exactly the case this is for,
    // and they share a version string. A "-dirty" suffix on either side
    // is a real difference and is left in the compare on purpose.
    if (have_kernel && k_strcmp(kv.build_id, TOYOS_BUILD_ID) != 0)
        sys_print("  ** kernel and userland are from different builds **\n");

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
