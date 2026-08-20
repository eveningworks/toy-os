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
#include "lib/human.h"

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

// The firmware's own view of the machine, one record per region. This
// is what the kernel shell's `meminfo` builtin printed and this program
// could not: multiboot_print_meminfo() writes straight to the console,
// and there was no provider behind it until QUERY_MEMMAP.
//
// The type NUMBER comes across the ABI and the NAME is chosen here,
// deliberately -- naming is a formatter's business, and an enum in the
// ABI would be a second table to keep in step with multiboot's.
static const char *region_type(unsigned long long type) {
    switch (type) {
    case QUERY_MEMMAP_USABLE: return "usable";
    case 3:  return "ACPI reclaimable";
    case 4:  return "ACPI NVS";
    case 5:  return "bad";
    default: return "reserved";
    }
}

static int print_memmap(void) {
    struct query_memmap r;
    char line[160], base[24], len[16];
    unsigned long long usable = 0;
    int printed = 0;

    // The loop ends on the record that is NOT THERE rather than on a
    // count read beforehand: a list's length is itself a fact and can
    // change between reads. Same rule list_providers() above follows.
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_MEMMAP, i, &r, sizeof r) <= 0) break;
        if (!printed) {
            sys_print("Firmware memory map\n");
            printed = 1;
        }
        human_size(len, sizeof len, r.length);
        snprintf(base, sizeof base, "0x%llx", (unsigned long long)r.base);
        snprintf(line, sizeof line, "  %-18s %-9s %s\n",
                 base, len, region_type(r.type));
        sys_print(line);
        if (r.type == QUERY_MEMMAP_USABLE) usable += r.length;
    }
    // A map with no regions is a real answer, not an error -- it is what
    // a boot with no multiboot mmap tag reports. Say so rather than
    // printing a bare heading with nothing under it.
    if (!printed) {
        sys_print("Firmware memory map: none reported by the bootloader\n");
        return 0;
    }
    human_size(len, sizeof len, usable);
    snprintf(line, sizeof line, "  usable total: %s\n", len);
    sys_print(line);
    return 0;
}

// `--audit` -- every mapping in every live address space that points at
// a frame the allocator considers FREE. ZERO FINDINGS IS THE HEALTHY
// ANSWER, and it is what this prints for a working machine.
//
// The invariant, stated because a reader meeting this output needs it:
// a frame a live mapping points at must be one pmm has handed out. A
// mapping of a free frame costs nothing at all until pmm gives that
// frame to somebody else, which is why this class of bug goes unnoticed
// (it has twice here). See kernel/mm/mm_audit.c.
//
// It does NOT look for ordinary leaks -- a used frame nothing references
// -- and does not claim to. That needs every kernel-side owner to
// declare its frames and is a separate job.
static int print_audit(void) {
    struct query_mmaudit f;
    char line[160];
    int found = 0;

    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_MMAUDIT, i, &f, sizeof f) <= 0) break;
        if (!found) {
            sys_print("DANGLING MAPPINGS -- a live mapping points at a FREE frame:\n");
            found = 1;
        }
        snprintf(line, sizeof line, "  pid %llu  va 0x%llx -> frame 0x%llx\n",
                 (unsigned long long)f.pid, (unsigned long long)f.vaddr,
                 (unsigned long long)f.frame);
        sys_print(line);
    }

    if (!found) {
        sys_print("audit: no dangling mappings -- every mapped frame is one\n"
                  "       the allocator considers handed out.\n");
        return 0;
    }
    // Non-zero exit, because this is the one thing here a script would
    // want to branch on.
    sys_print("audit: the allocator may hand these frames to somebody else\n"
              "       while their owner is still using them.\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--list") == 0) return list_providers();
    if (argc > 1 && strcmp(argv[1], "--audit") == 0) return print_audit();
    if (argc > 1 && strcmp(argv[1], "--map") == 0) return print_memmap();
    if (argc > 1) {
        cmd_usage("meminfo [--map | --audit | --list]");
        return 1;
    }

    // The MAP first, then the allocator -- the order the kernel shell's
    // builtin used, and the order that reads as a narrowing: what the
    // firmware says the machine has, then what pmm is doing with it.
    print_memmap();
    sys_print("\n");

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
    human_size(t, sizeof t, m.frame_total * m.frame_bytes);
    human_size(f, sizeof f, m.frame_free * m.frame_bytes);
    human_size(u, sizeof u, used * m.frame_bytes);

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

    human_size(t, sizeof t, m.heap_total_bytes);
    human_size(u, sizeof u, m.heap_used_bytes);
    snprintf(line, sizeof line, "Kernel heap\n  used:  %s of %s\n", u, t);
    sys_print(line);
    return 0;
}
