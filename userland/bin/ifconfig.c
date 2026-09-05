// ifconfig -- the network devices this boot found, and their addresses.
//
// WHAT IT IS FOR. Every NIC driver runs at boot and registers what it
// finds into a table (kernel/include/kernel/netdev.h); the first device
// gets QEMU's user-networking addresses because nothing here speaks
// DHCP yet. This is how a person sees both halves -- which cards exist,
// and what each one thinks its address is.
//
// THE COUNTERS ARE THE DIAGNOSIS, which is why they are printed rather
// than hidden behind a flag. A card with rx climbing and tx flat is
// listening to a network it cannot answer on; one with rx_dropped
// climbing alongside rx is being handed frames faster than the stack
// drains them. Neither is visible from `ping` alone.
//
// WHAT IT DELIBERATELY DOES NOT DO: bring an interface up or down.
// There is no administrative state in this stack -- a registered device
// is up, and the way to stop using one is not to address it. Adding a
// flag would mean inventing the state to go with it.
#include <stdint.h>
#include "rt/sys.h"
#include <stdio.h>
#include "lib/cmd.h"
#include <string.h>

static void print_ip(char *out, size_t cap, unsigned long long ip) {
    if (!ip) { snprintf(out, cap, "-"); return; }
    snprintf(out, cap, "%llu.%llu.%llu.%llu",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// Dotted quad to a host-order address. Returns 0 on anything that is
// not exactly four numbers under 256 -- a parser that guesses is worse
// than one that refuses, and "10.0.2" silently meaning 10.0.0.2 is the
// classic way to configure the wrong subnet.
static int parse_ip(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned octet = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            octet = octet * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || octet > 255) return 0;
        }
        v = (v << 8) | octet;
        if (part < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s) return 0;
    *out = v;
    return 1;
}

static int show(void) {
    int n = 0;
    struct query_netdev d;
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        n++;

        char ip[24], mask[24], gw[24], line[200];
        print_ip(ip, sizeof ip, d.ip);
        print_ip(mask, sizeof mask, d.netmask);
        print_ip(gw, sizeof gw, d.gateway);

        // WHERE the card is, beside what it is called. The name is an
        // identity that follows the card; this is the changeable half.
        //
        // FOR USB IT IS THE CONTROLLER'S PORT, NOT THE SOCKET: a USB3
        // controller numbers the same socket twice, once per speed
        // range, so a device that falls back from SuperSpeed reports a
        // different port without having moved (docs/bugs.md).
        char at[20] = "";
        if (d.location[0]) snprintf(at, sizeof at, "  at %s", d.location);

        snprintf(line, sizeof line,
                 "%s: %s  %02llx:%02llx:%02llx:%02llx:%02llx:%02llx%s  mtu %llu\n",
                 d.name, d.driver,
                 (d.mac) & 0xFF, (d.mac >> 8) & 0xFF, (d.mac >> 16) & 0xFF,
                 (d.mac >> 24) & 0xFF, (d.mac >> 32) & 0xFF, (d.mac >> 40) & 0xFF,
                 at, (unsigned long long)d.mtu);
        sys_print(line);

        if (d.ip)
            snprintf(line, sizeof line, "    inet %s  netmask %s  gateway %s\n", ip, mask, gw);
        else
            snprintf(line, sizeof line, "    inet (unconfigured)\n");
        sys_print(line);

        // LINK, and only when the driver can actually answer. A card
        // with no way to ask is not a card whose cable is unplugged, so
        // the line is ABSENT rather than saying "down" -- the same
        // three-valued honesty `poweroff: no` uses in /bin/acpi.
        if (d.link_known) {
            char speed[32];
            if (!d.link_up)
                snprintf(speed, sizeof speed, "down");
            else if (d.link_bps >= 1000000000ULL)
                snprintf(speed, sizeof speed, "up, %llu Gb/s",
                         (unsigned long long)(d.link_bps / 1000000000ULL));
            else if (d.link_bps >= 1000000ULL)
                snprintf(speed, sizeof speed, "up, %llu Mb/s",
                         (unsigned long long)(d.link_bps / 1000000ULL));
            else if (d.link_bps)
                snprintf(speed, sizeof speed, "up, %llu bit/s",
                         (unsigned long long)d.link_bps);
            else
                snprintf(speed, sizeof speed, "up");
            snprintf(line, sizeof line, "    link %s\n", speed);
            sys_print(line);
        }

        snprintf(line, sizeof line,
                 "    rx %llu packets, %llu bytes, %llu dropped\n"
                 "    tx %llu packets, %llu bytes, %llu dropped\n",
                 (unsigned long long)d.rx_packets, (unsigned long long)d.rx_bytes,
                 (unsigned long long)d.rx_dropped,
                 (unsigned long long)d.tx_packets, (unsigned long long)d.tx_bytes,
                 (unsigned long long)d.tx_dropped);
        sys_print(line);
    }

    // Never a silent empty listing: a machine with no NIC is an
    // ordinary state, and saying so is different from printing nothing,
    // which reads as the command having failed.
    if (!n) sys_print("no network devices\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return show();
    if (argc < 3 || argc > 5) {
        cmd_usage("ifconfig [<device> <address> [netmask [gateway]]]");
        return 1;
    }

    uint32_t ip = 0, mask = 0, gw = 0;
    if (!parse_ip(argv[2], &ip)) {
        sys_print("ifconfig: not an address: ");
        sys_print(argv[2]);
        sys_print("\n");
        return 1;
    }
    if (argc > 3 && !parse_ip(argv[3], &mask)) {
        sys_print("ifconfig: not a netmask: ");
        sys_print(argv[3]);
        sys_print("\n");
        return 1;
    }
    if (argc > 4 && !parse_ip(argv[4], &gw)) {
        sys_print("ifconfig: not a gateway: ");
        sys_print(argv[4]);
        sys_print("\n");
        return 1;
    }

    if (sys_net_config(argv[1], ip, mask, gw) < 0) {
        sys_print("ifconfig: no such device: ");
        sys_print(argv[1]);
        sys_print("\n");
        return 1;
    }
    return show();
}
