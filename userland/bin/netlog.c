// netlog -- who this machine has talked to: one line per connection.
//
// WHAT IT IS FOR. `netctl` reports what a card did in packets and
// bytes, which cannot answer "what did this machine connect to, and
// which program did it". The kernel keeps a ring of connection records
// (QUERY_CONNLOG); this prints them.
//
// A CONNECTION, NOT A PACKET, which is what makes the output readable:
// a TCP open is one line however many segments follow it, and a UDP or
// ICMP socket makes one the first time it sends to a destination. That
// is conntrack's flow rather than tcpdump's packet, the same choice
// Linux's `-m conntrack --ctstate NEW -j LOG` and Sysmon's Event 3
// both make.
//
// WHAT IT DELIBERATELY DOES NOT DO: block anything. This is a log, not
// a firewall -- there is no filtering layer in this stack to hang one
// off. What it can do is stop recording: `config set system.conn_log
// off`.
#include <stdint.h>
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lib/cmd.h"
#include "lib/usetting.h"

#define USAGE "netlog [-i|-o] [-n <count>] [-f]"

// How often -f looks again. A poll because a log has no readiness to
// wait on -- the ring is memory the kernel writes, and nothing wakes a
// reader.
#define FOLLOW_MS 500

struct filter { int in, out; };

// The record's UTC seconds as the local civil time everything else here
// prints. There is no stored offset to ask for: time() is a
// LOCAL-derived epoch (userland/libc/time.c) and QUERY_CLOCK's `utc` is
// the same instant in UTC, so their difference is the offset -- sampled
// once, since a timezone does not change between two lines of output.
static long long g_local_offset;

static void local_offset_init(void) {
    struct query_clock c;
    if (sys_query_record(QUERY_CLOCK, 0, &c, sizeof c) < (int)sizeof c) return;
    g_local_offset = (long long)time(0) - (long long)c.utc;
}

static void format_time(char *out, size_t cap, unsigned long long utc) {
    time_t t = (time_t)((long long)utc + g_local_offset);
    struct tm tm;
    if (!utc || !gmtime_r(&t, &tm)) { snprintf(out, cap, "--:--:--"); return; }
    snprintf(out, cap, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static const char *proto_name(unsigned long long proto) {
    switch (proto) {
    case 1:  return "icmp";
    case 6:  return "tcp";
    case 17: return "udp";
    default: return "?";
    }
}

static int wanted(const struct query_connlog *r, const struct filter *f) {
    return r->direction == QUERY_CONNLOG_IN ? f->in : f->out;
}

static void print_record(const struct query_connlog *r) {
    char when[16], remote[96], line[220];
    format_time(when, sizeof when, r->utc);

    // ICMP HAS NO PORTS, so it gets none rather than a zero that reads
    // like one. A name goes in brackets AFTER the address and never
    // instead of it: the address is what this machine really contacted,
    // and the name is only what a resolver said it was called.
    unsigned long long ip = r->remote_ip;
    if (r->remote_port)
        snprintf(remote, sizeof remote, "%llu.%llu.%llu.%llu:%llu",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                 (unsigned long long)r->remote_port);
    else
        snprintf(remote, sizeof remote, "%llu.%llu.%llu.%llu",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);

    snprintf(line, sizeof line, "%s  %-3s  %-4s  %-16s  %s%s%s%s\n",
             when, r->direction == QUERY_CONNLOG_IN ? "in" : "out",
             proto_name(r->proto), r->comm[0] ? r->comm : "-", remote,
             r->host[0] ? " (" : "", r->host[0] ? r->host : "",
             r->host[0] ? ")" : "");
    sys_print(line);
}

// Records the ring dropped before this got to them. Reported against
// the UNFILTERED sequence, because a gap counted after a filter would
// be the records this run was never going to print.
static void report_gap(unsigned long long seen, unsigned long long seq) {
    if (!seen || seq <= seen + 1) return;
    char note[80];
    snprintf(note, sizeof note, "... %llu records lost\n",
             (unsigned long long)(seq - seen - 1));
    sys_print(note);
}

int main(int argc, char **argv) {
    struct filter f = { 1, 1 };
    int follow = 0, keep = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0)      { f.in = 1; f.out = 0; }
        else if (strcmp(argv[i], "-o") == 0) { f.out = 1; f.in = 0; }
        else if (strcmp(argv[i], "-f") == 0) follow = 1;
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) keep = atoi(argv[++i]);
        else { cmd_usage(USAGE); return 1; }
    }

    local_offset_init();

    struct query_connlog r;
    int matched = 0, total = 0;
    unsigned long long seen = 0;
    QUERY_FOREACH(QUERY_CONNLOG, r, i) {
        if (r.seq > seen) seen = r.seq;
        total++;
        if (wanted(&r, &f)) matched++;
    }

    if (!matched && !follow) {
        // "None matched the filter" and "the log is empty" are
        // different answers, and only the second one is worth naming
        // the setting for.
        if (total) { sys_print("no connections match that filter\n"); return 0; }
        // An empty log is an ordinary state -- nothing has connected
        // yet, or the setting is off -- and saying so is different from
        // printing nothing, which reads as the command having failed.
        char mode[16];
        sys_print("no connections recorded");
        if (usetting_get("system.conn_log", mode, sizeof mode) &&
            strcmp(mode, "off") == 0)
            sys_print(" (system.conn_log is off)");
        sys_print("\n");
        return 0;
    }

    // `-n` prints the LAST n, which is the end anybody reading a log
    // wants. Counted first because a list class has no count to ask for.
    int skip = (keep > 0 && matched > keep) ? matched - keep : 0;
    QUERY_FOREACH(QUERY_CONNLOG, r, i) {
        if (!wanted(&r, &f)) continue;
        if (skip > 0) { skip--; continue; }
        print_record(&r);
    }

    while (follow) {
        sys_sleep_ms(FOLLOW_MS);
        unsigned long long high = seen;
        QUERY_FOREACH(QUERY_CONNLOG, r, i) {
            if (r.seq <= seen) continue;
            report_gap(high, r.seq);
            high = r.seq;
            if (wanted(&r, &f)) print_record(&r);
        }
        seen = high;
    }
    return 0;
}
