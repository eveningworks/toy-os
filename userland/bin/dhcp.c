// dhcp -- ask the network for an address instead of inventing one.
//
// A FRONT END NOW, not the client. The protocol, the lease file, the
// RFC 3927 fallback and RFC 2131's renewal states are all in
// userland/lib/udhcp.c, because /bin/netd needs the same code for
// several cards at once and two implementations of a DHCP client is two
// chances for one of them to be wrong. What is left here is what a
// COMMAND is: argument parsing, which cards to act on, and a loop that
// sleeps between steps.
//
// WITH NO ARGUMENT IT CONFIGURES EVERY DEVICE THAT HAS NO ADDRESS,
// which is what dhclient does when no interface is named. Naming one
// takes that device whatever state it is in, which is how a card is
// re-leased by hand.
//
// ONE-SHOT BY DEFAULT; `-k` stays resident and renews. On a machine
// that boots normally neither is what runs -- /bin/netd holds a lease
// per card and this is the tool for reaching in by hand.
#include <stdint.h>
#include "rt/sys.h"
#include "net_abi.h"
#include "query_abi.h"
#include "lib/cmd.h"
#include "lib/udhcp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Sleeping is the FRONT END's job: udhcp_step() returns a deadline and
// never sleeps on a caller's behalf, which is the whole reason netd can
// hold several cards without one blocking the others.
static void sleep_until(uint64_t deadline_ns) {
    for (;;) {
        uint64_t now = sys_monotonic_ns();
        if (now >= deadline_ns) return;
        uint64_t left_ms = (deadline_ns - now) / 1000000ull;
        if (!left_ms) return;
        if (left_ms > SYS_SLEEP_MAX_MS) left_ms = SYS_SLEEP_MAX_MS;
        sys_sleep_ms((int)left_ms);
    }
}

int main(int argc, char **argv) {
    // STAYING RESIDENT IS WHAT HAS TO BE ASKED FOR. The other way round
    // was tried and is wrong: `dhcp net-718ebf` typed at a prompt then
    // never returned, because the supervisor does not exit. A command
    // that hangs the terminal unless you know a flag is a worse default
    // than one that needs a flag to do the new thing.
    int keep = 0;
    const char *want = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k")) keep = 1;
        else if (!strcmp(argv[i], "-1")) keep = 0;   // the default; accepted
        else if (!want && argv[i][0] != '-') want = argv[i];
        else { cmd_usage("dhcp [-k] [<device>]"); return 1; }
    }
    // fd 1 reaches nobody when init spawns this, so a resident run sends
    // its diagnostics to the kernel log instead. `-k` is the flag that
    // MEANS "I am the resident service", so it is the thing that already
    // knows the answer -- isatty(1) does not, since init hands a service
    // a console fd 1 and it answers true.
    udhcp_log_to_kernel(keep);

    int tried = 0, done = 0;
    static struct udhcp first;
    int have_first = 0;

    struct query_netdev dev;
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_NETDEV, i, &dev, sizeof dev) < (int)sizeof dev)
            break;
        if (want) {
            if (strcmp(dev.name, want)) continue;
        } else if (dev.ip && udhcp_is_link_local(dev.ip)) {
            // dhcpcd and NetworkManager keep asking while they hold one.
            printf("dhcp: %s has only a link-local address -- asking again\n", dev.name);
        } else if (dev.ip) {
            // Said out loud rather than skipped silently: on a boot
            // where one card is already configured this is the whole
            // difference between "nothing to do" and "nothing worked".
            printf("dhcp: %s already has an address -- leaving it\n", dev.name);
            continue;
        }
        tried++;

        uint8_t mac[6];
        for (int b = 0; b < 6; b++) mac[b] = (uint8_t)(dev.mac >> (b * 8));
        struct udhcp u;
        udhcp_init(&u, dev.name, mac);
        if (udhcp_once(&u, &dev)) done++;

        // THE FIRST DEVICE TRIED IS THE ONE SUPERVISED, whether or not
        // it got a lease -- a card that fell back to link-local is
        // precisely the one worth asking again. That this command
        // supervises only ONE card is why /bin/netd exists: on a machine
        // with two, the second one's lease is nobody's here.
        if (!have_first) { first = u; have_first = 1; }
    }

    if (!tried) {
        if (want) printf("dhcp: no such device: %s\n", want);
        else      printf("dhcp: no device without an address\n");
        return 1;
    }
    if (!keep || !have_first) return done == tried ? 0 : 1;

    for (;;) sleep_until(udhcp_step(&first, sys_monotonic_ns()));
    return 0;
}
