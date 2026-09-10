// lsmod -- the loaded kernel modules: name, size, and what each holds.
//
// `lsdrv` answers "which drivers are in this build, and what did they
// bind" for module and built-in drivers alike -- a module's DRIVER_DECLARE
// lands in the same registry. This answers the question that only
// exists for modules: what is loaded right now, how big it is, and
// whether it could be unloaded. QUERY_MODULE, one record per module.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

#define USAGE "lsmod [-v]"

int main(int argc, char **argv) {
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else { cmd_usage(USAGE); return 1; }
    }

    printf("%-14s %8s  %s\n", "MODULE", "SIZE", verbose ? "BASE" : "USED BY");
    int n = 0;
    struct query_module m;
    QUERY_FOREACH(QUERY_MODULE, m, i) {
        n++;
        unsigned kib = (m.text_bytes + m.data_bytes) / 1024;
        if (verbose) {
            printf("%-14s %6u K  %#llx  text %u K, data %u K, %u driver(s)\n",
                   m.name, kib, (unsigned long long)m.base,
                   m.text_bytes / 1024, m.data_bytes / 1024, m.drivers);
        } else if (m.bound || m.pins) {
            printf("%-14s %6u K  ", m.name, kib);
            if (m.bound) printf("%u device(s)", m.bound);
            if (m.bound && m.pins) printf(", ");
            if (m.pins) printf("pinned %u", m.pins);
            printf("%s\n", m.removable ? "" : " (cannot unload)");
        } else {
            printf("%-14s %6u K  -\n", m.name, kib);
        }
    }
    if (!n) printf("lsmod: no module is loaded\n");
    return 0;
}
