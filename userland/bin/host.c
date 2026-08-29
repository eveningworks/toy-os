// host -- turn a name into an address.
//
// Named after BIND's `host` rather than `nslookup` because that is the
// one that does exactly this and nothing else. The resolving lives in
// userland/lib/uresolv.c, shared with `ping`, so a name means the same
// thing in both.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/uresolv.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        cmd_usage("host <name> [server]");
        return 1;
    }

    uint32_t server = 0;
    if (argc == 3 && !uresolv_parse_ip(argv[2], &server)) {
        printf("host: the server must be an address, not a name: %s\n", argv[2]);
        return 1;
    }

    // An address given as the name is answered without a query. Not a
    // shortcut: asking a server to resolve "10.0.2.2" is a question
    // about a name that happens to look like an answer, and the reply
    // would be NXDOMAIN.
    uint32_t ip = 0;
    if (uresolv_parse_ip(argv[1], &ip)) {
        printf("%s is already an address\n", argv[1]);
        return 0;
    }

    int rc = uresolv_lookup(argv[1], server, &ip);
    if (rc == 0) {
        printf("%s has address %u.%u.%u.%u\n", argv[1],
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        return 0;
    }

    // Each failure names something different to go and check, which is
    // the whole reason they are separate codes rather than one.
    uint32_t used = server ? server : uresolv_server();
    switch (rc) {
    case -ENODEV:
        printf("host: no nameserver configured -- run `dhcp`, or set one in %s\n",
               URESOLV_CONF);
        break;
    case -ENOENT:
        printf("host: %s not found\n", argv[1]);
        break;
    case -EAGAIN:
        printf("host: no answer from %u.%u.%u.%u\n",
               (used >> 24) & 0xFF, (used >> 16) & 0xFF, (used >> 8) & 0xFF, used & 0xFF);
        break;
    default:
        printf("host: %s: %s\n", argv[1], strerror(-rc));
        break;
    }
    return 1;
}
