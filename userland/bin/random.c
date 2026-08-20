// random -- the entropy source, and some bytes from it.
//
// IT PRINTS THE SOURCE FIRST, AND THAT IS THE POINT. The numbers look
// equally random whatever produced them, so the only part a reader can
// actually judge is where they came from -- which under QEMU is usually
// TSC jitter, i.e. the weakest case. A tool that printed only the
// values would be reassuring and uninformative.
//
// The source comes from QUERY_RANDOM, not from SYS_GETRANDOM: that
// syscall deliberately does not report quality (abi/syscall_abi.h says
// why), and this is the diagnostic question rather than a promise
// attached to the bytes. See kernel/lib/krandom_query.c.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/stdio.h"
#include <knum.h>   // k_parse_u32 -- rejects rather than guessing

#define MAX_VALUES 32

int main(int argc, char **argv) {
    unsigned count = 4;
    if (argc > 2) {
        cmd_usage("random [count]   (1..32 values, default 4)");
        return 1;
    }
    if (argc == 2) {
        uint32_t n;
        if (!k_parse_u32(argv[1], &n) || n == 0 || n > MAX_VALUES) {
            cmd_usage("random [count]   (1..32 values, default 4)");
            return 1;
        }
        count = n;
    }

    struct query_random r;
    int n = sys_query_record(QUERY_RANDOM, 0, &r, sizeof r);
    if (n >= (int)sizeof r) {
        char line[128];
        snprintf(line, sizeof line, "Entropy source: %s\n", r.name);
        sys_print(line);
        // Said plainly rather than left to be inferred from a label.
        // Both of these are about the TRUST BOUNDARY, not the quality
        // of any individual byte, which is why they are sentences.
        if (r.quality == QUERY_RANDOM_VIRTIO) {
            sys_print("  (from the host, via virtio-rng -- real entropy, and the\n"
                      "   hypervisor already owns this machine's memory anyway)\n");
        } else if (r.quality == QUERY_RANDOM_JITTER) {
            sys_print("  (no RDSEED/RDRAND on this CPU -- weak under emulation)\n");
        }
    } else {
        // A missing provider is not a reason to refuse the bytes; it is
        // a reason to refuse to VOUCH for them.
        sys_print("Entropy source: unknown (no provider registered)\n");
    }

    uint32_t vals[MAX_VALUES];
    if (sys_getrandom(vals, count * sizeof vals[0]) < 0) {
        cmd_fail("random", 0);
        return 1;
    }
    for (unsigned i = 0; i < count; i++) {
        char line[32];
        snprintf(line, sizeof line, "  %u\n", (unsigned)vals[i]);
        sys_print(line);
    }
    return 0;
}
