// sleep -- wait for a while, then exit 0.
//
// GNU's operand syntax (lib/uduration.h): `sleep 2`, `sleep 0.25`,
// `sleep 1m 30s` -- several operands are added together.
//
// ONE SYS_SLEEP IS CAPPED AT SYS_SLEEP_MAX_MS AND RETURNS EARLY SILENTLY,
// so this loops against the monotonic clock until the deadline has
// passed rather than trusting a single call (`/bin/dhcp` once renewed
// hourly by trusting one).
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/uduration.h"
#include "syscall_abi.h"
#include <unistd.h>

#define USAGE "sleep <number>[s|m|h|d]...   (several are added: sleep 1m 30s)"

// Far past any wait anyone means, and far short of wrapping the clock.
#define SLEEP_MAX_MS (100ull * 365 * 86400000)

int main(int argc, char **argv) {
    if (argc < 2) {
        cmd_usage(USAGE);
        return 1;
    }
    uint64_t total = 0;
    for (int i = 1; i < argc; i++) {
        uint64_t ms;
        if (!uduration_parse_ms(argv[i], &ms)) {
            cmd_fail_msg("sleep", argv[i], "not a duration");
            cmd_usage(USAGE);
            return 1;
        }
        if (ms > SLEEP_MAX_MS || total + ms > SLEEP_MAX_MS) {
            cmd_fail_msg("sleep", argv[i], "longer than a hundred years");
            return 1;
        }
        total += ms;
    }

    uint64_t deadline = sys_monotonic_ns() + total * 1000000ull;
    for (;;) {
        uint64_t now = sys_monotonic_ns();
        if (now >= deadline) return 0;
        uint64_t left = (deadline - now + 999999) / 1000000;   // up: never early
        if (left > SYS_SLEEP_MAX_MS) left = SYS_SLEEP_MAX_MS;
        if (usleep((unsigned long)left * 1000) < 0) {
            // No scheduler slot: the legacy `run` loader cannot park.
            cmd_fail_msg("sleep", 0, "cannot sleep here (start it with spawn, not run)");
            return 1;
        }
    }
}
