// ntpd -- ask a time server what time it is, and set the clock.
//
// SNTP, RFC 4330, not full NTP. The difference is real and worth
// stating: NTP disciplines the clock's RATE against several servers and
// never lets time run backwards; SNTP asks ONE server and STEPS. Real
// systems run the full thing (chrony, ntpd, w32time), and toy-os does
// not, because a rate-adjustable clock is a different kernel feature
// from the one that exists -- ktime is an epoch plus a monotonic delta,
// with no tick-rate knob to turn. Anything measuring an interval uses
// SYS_MONOTONIC_NS, which a step cannot move, so the cost of stepping is
// bounded to whatever reads wall time.
//
// WHY THIS IS A RING-3 PROGRAM, and not a kernel timer. Same argument as
// /bin/dhcp: which server to trust, how long to wait, how often to ask
// and what to do when nobody answers are all policy. The kernel owns the
// clock and exposes exactly one verb for it (SYS_SETTIME), the same way
// it owns the network device and exposes SYS_NET_CONFIG.
//
// RESIDENT BY DEFAULT, unlike dhcp -- `ntpd` with no argument syncs and
// keeps syncing, `-1` syncs once and exits. It is spelled the other way
// round from dhcp's `-k` because this program is named for the daemon
// and that one is named for the protocol, and a `dhcp` that never
// returned would have surprised everything that runs it by hand.
//
// THE SERVER IS RESOLVED EVERY CYCLE, not once at startup. A pool name
// answers with a different address each time by design, and a resident
// client that cached the first one would hammer one member of the pool
// for as long as the machine is up -- which is the behaviour pool
// operators ask clients not to have.
#include <stdint.h>
#include "rt/sys.h"
#include "net_abi.h"
#include "query_abi.h"
#include "setting_abi.h"
#include "ntp_config.h"
#include "lib/cmd.h"
#include "lib/uresolv.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "lib/uargs.h"

#define NTP_PACKET 48

// THE PORT IS A FLAG because a time server on a port other than 123 has
// to be reachable: an unprivileged test server cannot bind 123, and this
// is what lets tools/ntp_test.py run one on loopback instead of the
// suite reaching the real internet. chrony's `port` directive exists for
// the same reason. 123 unless -p says otherwise.
#define NTP_PORT_DEFAULT 123
static uint16_t g_port = NTP_PORT_DEFAULT;

// 1900-01-01 to 1970-01-01 in seconds. NTP counts from 1900; everything
// here counts from 1970.
#define NTP_TO_UNIX 2208988800ull

// ONE EXCHANGE'S WHOLE BUDGET, and it is longer than dhcp's 4 s on
// purpose: the FIRST exchange after boot has to resolve ARP for the
// gateway before a datagram can leave, and a budget that expires during
// that reports "no answer from <server>" about a server that was never
// reached. Nothing waits on this -- the resident loop is asleep and a
// person running it by hand would rather wait than be told a lie.
#define WAIT_MS   8000  // one exchange's whole budget
#define RETX_MS   1000  // first retransmit; doubles inside the budget
#define POLL_MS   10    // the ARP-still-resolving retry
#define ARP_MS    3000  // how long a first send may spend resolving one

// How long to wait at startup for a network device to have an address.
// A resident client retries forever anyway, so this only shortens the
// first cycle's failure on a machine that is still running dhcp.
#define ADDR_WAIT_MS 15000
#define ADDR_POLL_MS 250

// A cycle that failed retries sooner than the configured interval, and
// backs off to it. Without this, a machine whose first sync raced the
// network would sit with a wrong clock for the whole interval.
#define RETRY_MIN_S 30

// --- where the output goes ---------------------------------------------
//
// fd 2 IS THE KERNEL LOG here (abi/syscall_abi.h), so a resident
// service's diagnostics land in `dmesg` where somebody can find them,
// while a person typing `ntpd -1` at a prompt gets the answer on their
// own terminal. dhcp.c carries the same split and the same reason: it
// picks on the FLAG that means "I am the service", never on isatty(),
// because init hands a service a console fd 1 that answers true and
// presents nothing on a graphical boot.
static int g_to_log;

static void say(const char *fmt, ...) {
    va_list ap;
    char buf[200];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    write(g_to_log ? 2 : 1, buf, strlen(buf));
}

// --- the settings ------------------------------------------------------

static int setting_str(const char *name, char *out, unsigned cap) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GET;
    strlcpy(m.name, name, sizeof m.name);
    if (sys_setting(&m) != 0 || !m.value[0]) return 0;
    strlcpy(out, m.value, cap);
    return 1;
}

static int ntp_enabled(void) {
    char v[SETTING_ABI_VALUE_MAX];
    if (!setting_str("system.ntp", v, sizeof v)) return 0;
    return strcmp(v, "on") == 0;
}

// A SERVER NAMED ON THE COMMAND LINE OUTRANKS THE SETTING AND IS NOT
// PERSISTED. `ntpd -q time.example.com` is a question about that host,
// not a decision about this machine -- `config set system.ntp_server`
// is how the machine is configured, and a command that quietly did both
// would make the visible one wrong.
static const char *g_server_override;

static void ntp_server_name(char *out, unsigned cap) {
    if (g_server_override) { strlcpy(out, g_server_override, cap); return; }
    if (!setting_str("system.ntp_server", out, cap))
        strlcpy(out, NTP_DEFAULT_SERVER, cap);
}

static int ntp_interval_minutes(void) {
    char v[SETTING_ABI_VALUE_MAX];
    if (!setting_str("system.ntp_interval", v, sizeof v)) return NTP_DEFAULT_INTERVAL;
    int n = atoi(v);
    // The registry bounds this, so an out-of-range value here means the
    // /etc file was hand-edited past it. Clamped rather than refused: a
    // daemon that exits over a config typo is worse than one that syncs
    // hourly instead of never.
    if (n < NTP_INTERVAL_MIN) n = NTP_INTERVAL_MIN;
    if (n > NTP_INTERVAL_MAX) n = NTP_INTERVAL_MAX;
    return n;
}

// --- the clock ---------------------------------------------------------

// This machine's UTC, in nanoseconds. QUERY_CLOCK rather than
// sys_gettime(), whose civil fields stop at the whole second -- too
// coarse to compare against a timestamp that arrived off the wire.
static uint64_t local_utc_ns(void) {
    struct query_clock c;
    if (sys_query_record(QUERY_CLOCK, 0, &c, sizeof c) < (int)sizeof c) return 0;
    return c.utc_ns;
}

// --- the packet --------------------------------------------------------

static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void put64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

// An NTP timestamp is 32 bits of seconds since 1900 and 32 bits of
// fraction. Converting to nanoseconds since 1970 is the whole of this
// client's arithmetic, and the ERA is the part that is easy to get
// wrong.
//
// **THE 2036 ROLLOVER.** The seconds field is 32 bits, so it wraps on
// 2036-02-07. RFC 4330 s3 says to read the top bit as the era: set means
// era 0 (1968-2036, the epoch is 1900), clear means era 1 (2036-2104,
// the epoch is 2036). Reading it as era 0 unconditionally is correct
// today and puts the clock 136 years in the past the moment it wraps --
// which is exactly the shape of bug that ships because nothing can test
// it, so it is handled here rather than noted as a limitation.
static uint64_t ntp_to_unix_ns(uint64_t ts) {
    uint64_t secs = ts >> 32;
    uint64_t frac = ts & 0xFFFFFFFFull;
    uint64_t ns = (frac * 1000000000ull) >> 32;

    if (secs & 0x80000000ull) {
        // Era 0: seconds since 1900.
        return (secs - NTP_TO_UNIX) * 1000000000ull + ns;
    }
    // Era 1: seconds since 2036-02-07 06:28:16 UTC, which is
    // 2^32 - NTP_TO_UNIX in Unix seconds.
    return (secs + (0x100000000ull - NTP_TO_UNIX)) * 1000000000ull + ns;
}

// One exchange. Returns 1 with *out_utc_ns set to what this machine's
// clock SHOULD read, and *out_rtt_ns to the measured round trip.
//
// **THE ROUND TRIP IS MEASURED ON THE MONOTONIC CLOCK, and the answer is
// derived from the server's transmit timestamp alone.** The textbook
// offset formula uses four timestamps, two of which are this machine's
// wall clock -- and on the boot this program exists for, that clock is
// wrong by an unknown amount, so feeding it into the arithmetic and
// subtracting it out again is a longer way to the same number. What the
// server said, plus half the time the reply spent in flight, is the
// whole answer.
static int exchange(uint32_t server_ip, uint64_t *out_utc_ns, uint64_t *out_rtt_ns) {
    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    if (fd < 0) return 0;

    uint8_t tx[NTP_PACKET], rx[NTP_PACKET];
    memset(tx, 0, sizeof tx);
    // LI 0 (no warning), VN 4, Mode 3 (client).
    tx[0] = 0x23;

    // A NONCE IN THE TRANSMIT TIMESTAMP, and the server echoes it back
    // in the originate field -- which is what tells our reply from a
    // stray datagram on the same socket. Random rather than the local
    // clock, because the local clock is the thing that may be wrong and
    // two boots with the same dead RTC would send the same "nonce".
    uint64_t nonce = 0;
    if (sys_getrandom(&nonce, sizeof nonce) != (int64_t)sizeof nonce)
        nonce = sys_monotonic_ns();
    put64(tx + 40, nonce);

    uint64_t deadline = sys_monotonic_ns() + (uint64_t)WAIT_MS * 1000000ull;
    uint64_t next_send = 0;
    uint32_t backoff_ms = RETX_MS;
    uint64_t sent_mono = 0;
    int ok = 0;

    for (;;) {
        uint64_t now = sys_monotonic_ns();
        if (now >= deadline) break;

        if (now >= next_send) {
            // ARP_MS, not a couple of hundred milliseconds. The FIRST
            // send after boot has no ARP entry for the gateway, and a
            // budget shorter than one ARP exchange reports "no answer
            // from <server>" for a server that was never asked --
            // measured, and it is why this number matches uresolv's.
            int64_t rc = -1;
            for (int waited = 0; waited < ARP_MS; waited += POLL_MS) {
                rc = sys_sendto(fd, tx, sizeof tx, server_ip, g_port);
                if (rc >= 0 || sys_errno() != EAGAIN) break; // ARP still resolving
                sys_sleep_ms(POLL_MS);
            }
            if (rc < 0) break;
            sent_mono = sys_monotonic_ns();
            next_send = sent_mono + (uint64_t)backoff_ms * 1000000ull;
            backoff_ms *= 2;
        }

        uint64_t until = next_send < deadline ? next_send : deadline;
        now = sys_monotonic_ns();
        if (now >= until) continue;
        unsigned left = (unsigned)((until - now) / 1000000ull);

        uint32_t src = 0;
        uint16_t port = 0;
        int64_t n = sys_recvfrom(fd, rx, sizeof rx, &src, &port, left ? left : 1);
        if (n <= 0) continue; // this slice expired -- retransmit or give up
        uint64_t arrived = sys_monotonic_ns();

        if (n < NTP_PACKET || src != server_ip) continue;
        // OURS: the originate timestamp is the nonce we sent.
        if (get64(rx + 24) != nonce) continue;

        // The three refusals RFC 4330 s5 requires of a client, and each
        // one is a server saying its own answer is no good:
        int li = (rx[0] >> 6) & 3;
        int mode = rx[0] & 7;
        int stratum = rx[1];
        if (li == 3) { say("ntpd: the server is not synchronised\n"); break; }
        if (mode != 4) continue;              // not a server reply
        if (stratum == 0) {                   // a KISS-O'-DEATH packet
            say("ntpd: the server refused us (kiss code %c%c%c%c)\n",
                rx[12] ? rx[12] : ' ', rx[13] ? rx[13] : ' ',
                rx[14] ? rx[14] : ' ', rx[15] ? rx[15] : ' ');
            break;
        }
        if (stratum > 15) continue;

        uint64_t transmit = get64(rx + 40);
        if (!transmit) continue;              // an unset clock says nothing

        // Measured from the LAST send, so a reply to an earlier
        // retransmission understates it by up to one backoff interval.
        // Accepted rather than fixed with a fresh nonce per send, which
        // would discard such a reply and leave a link slower than the
        // first backoff never syncing at all -- the wrong trade for a
        // clock that is accurate to the second.
        uint64_t rtt = arrived - sent_mono;
        *out_rtt_ns = rtt;
        // Half the round trip, on the assumption the path is symmetric.
        // It is the assumption SNTP is built on, and it is why this
        // cannot be more accurate than the network is even.
        *out_utc_ns = ntp_to_unix_ns(transmit) + rtt / 2;
        ok = 1;
        break;
    }

    sys_close(fd);
    return ok;
}

// --- waiting for a network ---------------------------------------------

// Does any device have an address? Not "is the link up" -- dhcp's
// question -- because this program does not name a device and cannot
// send anything until something has been configured.
static int have_address(void) {
    struct query_netdev d;
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_NETDEV, i, &d, sizeof d) < (int)sizeof d) return 0;
        if (d.ip) return 1;
    }
}

static void wait_for_address(void) {
    for (int waited = 0; waited < ADDR_WAIT_MS; waited += ADDR_POLL_MS) {
        if (have_address()) return;
        sys_sleep_ms(ADDR_POLL_MS);
    }
}

// --- one sync ----------------------------------------------------------

// Resolves the configured server and does one exchange. `apply` 0 means
// report only. Returns 1 if a server answered.
static int sync_once(int apply) {
    char host[SETTING_ABI_VALUE_MAX];
    ntp_server_name(host, sizeof host);

    uint32_t ip = 0;
    // A DOTTED QUAD FIRST, as every caller of uresolv does -- looking up
    // "10.0.2.2" would be a query for a name that is already an answer,
    // and on a machine with no nameserver it would fail for a reason
    // that is not the user's.
    if (!uresolv_parse_ip(host, &ip)) {
        int rc = uresolv_lookup(host, 0, &ip);
        if (rc != 0) {
            say("ntpd: cannot resolve %s: %s\n", host,
                rc == -ENODEV ? "no nameserver configured -- run `dhcp`"
                              : sys_strerror(-rc));
            return 0;
        }
    }

    uint64_t server_ns = 0, rtt_ns = 0;
    if (!exchange(ip, &server_ns, &rtt_ns)) {
        say("ntpd: no answer from %s (%u.%u.%u.%u)\n", host,
            (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        return 0;
    }

    uint64_t local_ns = local_utc_ns();
    int64_t offset_ms = (int64_t)((server_ns - local_ns) / 1000000ull);
    // Unsigned subtraction wraps for a clock that is AHEAD of the
    // server, which is half the cases -- so the sign is recovered by
    // comparing before dividing rather than by hoping.
    if (local_ns > server_ns)
        offset_ms = -(int64_t)((local_ns - server_ns) / 1000000ull);

    if (!apply) {
        say("ntpd: %s (%u.%u.%u.%u) offset %+lld ms, delay %llu ms\n", host,
            (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
            (long long)offset_ms, (unsigned long long)(rtt_ns / 1000000ull));
        return 1;
    }

    // SECONDS AND NANOSECONDS, both. Passing only the seconds was the
    // first version and it discarded the sub-second part of every
    // correction -- so a sync that measured a 7 ms round trip left the
    // clock a few hundred milliseconds behind, every time, for ever.
    uint64_t target = server_ns / 1000000000ull;
    uint32_t target_ns = (uint32_t)(server_ns % 1000000000ull);
    if (sys_settime(target, target_ns) != 0) {
        say("ntpd: the kernel refused %llu as a time\n", (unsigned long long)target);
        return 0;
    }
    say("ntpd: clock set from %s, offset was %+lld ms (delay %llu ms)\n", host,
        (long long)offset_ms, (unsigned long long)(rtt_ns / 1000000ull));
    return 1;
}

// --- the resident loop -------------------------------------------------

// sys_sleep_ms() is silently CLAMPED to SYS_SLEEP_MAX_MS (an hour), so a
// longer wait has to be looped rather than asked for. dhcp.c has the
// same helper for the same reason -- a lease renewal past an hour was
// waking up early and re-requesting far too often.
static void sleep_seconds(unsigned secs) {
    while (secs) {
        unsigned chunk = secs > 1800 ? 1800 : secs;
        sys_sleep_ms((int)(chunk * 1000));
        secs -= chunk;
    }
}

static void supervise(void) {
    unsigned retry_s = RETRY_MIN_S;
    int waited_for_address = 0;

    for (;;) {
        // RE-READ EVERY CYCLE, so turning the setting off in System
        // Settings takes effect without restarting the service, and so
        // does changing the server. A daemon that cached its config at
        // startup would make every settings change a reboot.
        if (!ntp_enabled()) {
            sleep_seconds(60);
            continue;
        }

        // ONLY ONCE, AND ONLY WHEN THERE IS SOMETHING TO DO. Waiting at
        // startup instead meant every boot of a machine with network
        // time switched OFF spent fifteen seconds polling the device
        // table to answer a question nobody had asked.
        if (!waited_for_address) {
            wait_for_address();
            waited_for_address = 1;
        }

        if (sync_once(1)) {
            retry_s = RETRY_MIN_S;
            sleep_seconds((unsigned)ntp_interval_minutes() * 60u);
        } else {
            // BACK OFF, doubling to the configured interval. A machine
            // whose first sync raced the network retries in half a
            // minute; one on a segment with no time server at all stops
            // asking every thirty seconds forever.
            sleep_seconds(retry_s);
            retry_s *= 2;
            unsigned cap = (unsigned)ntp_interval_minutes() * 60u;
            if (retry_s > cap) retry_s = cap;
        }
    }
}

static int f_once, f_query;
static const char *f_port;

static const struct uargs_opt OPTS[] = {
    { 0, '1', 0,      "sync once and exit, instead of staying resident", &f_once, 0 },
    { 0, 'q', 0,      "report the offset and change nothing", &f_query, 0 },
    { 0, 'p', "PORT", "the server's UDP port (default 123)", 0, &f_port },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "ntpd",
    .usage = "[-1 | -q] [-p PORT] [SERVER]",
    .summary = "Set this machine's clock from a network time server.",
    .opts = OPTS,
    .notes = "With no SERVER, `system.ntp_server` is used. Resident syncing obeys\n"
             "`system.ntp` and `system.ntp_interval`; -1 and -q do not.",
};

int main(int argc, char **argv) {
    int once = 0, query = 0;
    const char *server = 0;

    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc > 1) return uargs_error(&PROG, "one server at a time");
    if (a.argc == 1) server = a.argv[0];
    once = f_once != 0;
    query = f_query != 0;
    if (f_port) {
        int p = atoi(f_port);
        if (p < 1 || p > 65535) return uargs_error(&PROG, "'%s' is not a port", f_port);
        g_port = (uint16_t)p;
    }

    if (server) g_server_override = server;

    if (once || query) {
        // ONE-SHOT IGNORES `system.ntp`. The setting governs the
        // resident service; somebody typing `ntpd -1` has already said
        // what they want, and refusing them because a background service
        // is switched off would be the machine arguing.
        return sync_once(!query) ? 0 : 1;
    }

    g_to_log = 1;
    supervise();
    return 0; // not reached
}
