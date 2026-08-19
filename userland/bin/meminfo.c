// meminfo -- memory, read as a FACT through SYS_QUERY.
//
// The first consumer of the query registry (docs/query-design.md's stage
// 0), and the proof that the mechanism works: this program and the
// kernel shell's own `meminfo` do not merely agree, they call the same
// provider through the same query_read() -- one reader, so they cannot
// drift.
//
// It asks for QUERY_MEMINFO directly rather than walking class 0 to find
// it. That is the purpose-built pattern: discovery exists for a generic
// tool that does not know what it is looking for, and this one does.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/stdio.h"
#include "lib/string.h"

// --list walks the registry instead -- the generic path, and the reason
// class 0 exists. Worth having in the FIRST consumer rather than a later
// one: it is what proves the list half of the mechanism works, which a
// scalar-only caller would leave unexercised.
static int list_providers(void) {
    struct query_provider_info p;
    char line[96];
    sys_print("CLASS  NAME              RECORDS  SIZE  KIND\n");
    // `count` from the registry is a HINT, not a bound: a list's length
    // is itself a fact and can change between reads. The loop ends on
    // the record that is not there, which is the answer that cannot be
    // stale.
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_PROVIDERS, i, &p, sizeof p) <= 0) break;
        // NUMBERS ARE FORMATTED THEN PADDED AS STRINGS. kfmt's numeric
        // width ZERO-pads (%-6u of 0 is "000000"), which is right for a
        // timestamp and wrong for every column -- see CLAUDE.md.
        char cls[12], cnt[12], sz[12];
        snprintf(cls, sizeof cls, "%u", p.cls);
        snprintf(cnt, sizeof cnt, "%u", p.count);
        snprintf(sz, sizeof sz, "%u", p.record_size);
        snprintf(line, sizeof line, "%-6s %-17s %-8s %-5s %s\n",
                 cls, p.name, cnt, sz,
                 (p.flags & QUERY_F_LIST) ? "list" : "scalar");
        sys_print(line);
    }
    return 0;
}

// 1.2K / 4.0M, integer only -- there is no floating point in ring 3
// (-mno-sse), so the tenth comes out of the remainder. Same shape
// /bin/ls -h and /bin/df use.
static void put_size(char *out, unsigned long cap, unsigned long long n) {
    static const char unit[] = { 'B', 'K', 'M', 'G' };
    int u = 0;
    unsigned long long whole = n, rem = 0;
    while (whole >= 1024 && u < 3) {
        rem = whole % 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0) snprintf(out, cap, "%lluB", whole);
    else snprintf(out, cap, "%llu.%llu%c", whole, (rem * 10) / 1024, unit[u]);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--list") == 0) return list_providers();
    if (argc > 1) {
        cmd_usage("meminfo [--list]");
        return 1;
    }

    struct query_meminfo m;
    int n = sys_query_record(QUERY_MEMINFO, 0, &m, sizeof m);
    if (n <= 0) {
        cmd_fail("meminfo", 0);
        return 1;
    }
    // `returned` is checked rather than assumed: the ABI promises
    // min(len, record), so a kernel whose struct is SHORTER than this
    // build expects would otherwise leave the tail reading as zeroes.
    // This is the version tolerance actually being used, not just
    // documented.
    if ((unsigned)n < sizeof m) {
        sys_print("meminfo: this kernel reports fewer fields than expected\n");
        return 1;
    }

    unsigned long long used = m.frame_total - m.frame_free;
    char t[16], f[16], u[16], line[128];
    put_size(t, sizeof t, m.frame_total * m.frame_bytes);
    put_size(f, sizeof f, m.frame_free * m.frame_bytes);
    put_size(u, sizeof u, used * m.frame_bytes);

    snprintf(line, sizeof line, "Physical frames (%llu bytes each)\n",
             (unsigned long long)m.frame_bytes);
    sys_print(line);
    // Same zero-padding rule as above: format, then pad as a string.
    char n1[24], n2[24], n3[24];
    snprintf(n1, sizeof n1, "%llu", (unsigned long long)m.frame_total);
    snprintf(n2, sizeof n2, "%llu", used);
    snprintf(n3, sizeof n3, "%llu", (unsigned long long)m.frame_free);
    snprintf(line, sizeof line, "  total: %-10s %s\n", n1, t);
    sys_print(line);
    snprintf(line, sizeof line, "  used:  %-10s %s\n", n2, u);
    sys_print(line);
    snprintf(line, sizeof line, "  free:  %-10s %s\n", n3, f);
    sys_print(line);

    put_size(t, sizeof t, m.heap_total_bytes);
    put_size(u, sizeof u, m.heap_used_bytes);
    snprintf(line, sizeof line, "Kernel heap\n  used:  %s of %s\n", u, t);
    sys_print(line);
    return 0;
}
