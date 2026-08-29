#ifndef ULIB_URESOLV_H
#define ULIB_URESOLV_H

#include <stdint.h>

// Turning a name into an address: DNS over UDP, in ring 3.
//
// A LIBRARY RATHER THAN A DAEMON, and rather than a copy inside each
// program that needs it. There is no nscd here, no resolver cache and
// no /etc/hosts -- a query goes to the wire every time, which is
// honest at this scale and is the thing to revisit first if anything
// ever resolves in a loop.
//
// THE SERVER COMES FROM /etc/resolv.conf, written by `/bin/dhcp`. The
// FILE has Unix's name and the CONTENT is this repo's `key=value`
// (`nameserver=10.0.2.3`), because there is one config parser here and
// a second one for one field is the drift nobody looks for.
//
// A ONLY. No AAAA (there is no IPv6), no MX, no TXT, no reverse
// lookups -- a caller wants an address to connect to, and every other
// record type is a different program's question.

#define URESOLV_CONF "/etc/resolv.conf"

// The nameserver this machine would use, host byte order, or 0 when
// /etc/resolv.conf names none. Exposed so a caller can say WHICH
// server failed to answer rather than reporting a bare timeout.
uint32_t uresolv_server(void);

// Resolve `name` to an IPv4 address. `server` of 0 means "whatever
// uresolv_server() says". Returns 0 on success with *out_ip set, or a
// negative errno: -ENODEV when no server is configured, -EAGAIN when
// nothing answered in time, -ENOENT when the server said the name does
// not exist, -EINVAL on a malformed name or reply.
int uresolv_lookup(const char *name, uint32_t server, uint32_t *out_ip);

// A dotted quad, without asking anyone. Returns 1 on success. Exposed
// because every caller of the above wants to try this FIRST -- looking
// up "10.0.2.2" would be a query for a name that is already an answer.
int uresolv_parse_ip(const char *s, uint32_t *out);

#endif // ULIB_URESOLV_H
