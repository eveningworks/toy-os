// acpi -- what this machine's firmware described, and what this kernel
// will do with it when asked to stop.
//
// WHAT IT IS FOR. `reboot --poweroff` either stops the machine or it
// does not, and when it does not there is nothing on screen to say
// which half failed. This is that: the tables that were found, the port
// the S5 write goes to, and the sleep type this machine's own `_S5_`
// object named. A `poweroff: no` line here is the answer to "why did it
// just halt instead of turning off".
//
// It is deliberately not a debug dump of every table's body -- there is
// no AML interpreter in this OS and printing bytes nobody can decode
// would suggest otherwise. See kernel/include/kernel/acpi.h.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

// Mirrors of the kernel-side flags. They cross the boundary as a number
// in the record rather than as a header apps could include: kernel/
// headers are deliberately absent from userland's include path.
#define ACPI_F_TABLES     (1u << 0)
#define ACPI_F_FADT       (1u << 1)
#define ACPI_F_S5         (1u << 2)
#define ACPI_F_HW_REDUCED (1u << 3)
#define ACPI_F_RESET      (1u << 4)
#define ACPI_F_ENABLED    (1u << 5)
#define ACPI_F_XSDT       (1u << 6)

#define ACPI_CPU_ENABLED        (1u << 0)
#define ACPI_CPU_ONLINE_CAPABLE (1u << 1)
#define ACPI_CPU_X2APIC         (1u << 2)

static void put(const char *s) { sys_print(s); }

static const char *yesno(int v) { return v ? "yes" : "no"; }

static const char *source_name(uint64_t v) {
    switch (v) {
        case 1: return "multiboot2 tag";
        case 2: return "BIOS-area scan";
        default: return "not found";
    }
}

static const char *space_name(uint64_t v) {
    return v == 1 ? "port" : v == 0 ? "memory" : "unsupported space";
}

// --- --dump: the raw bytes of one table -------------------------------
//
// The same move `lsusb -D` makes, and for the same reason: a parser for
// firmware data has to be tested against real firmware data, and these
// bytes paste straight into a KTEST fixture. A real DSDT is tens of
// kilobytes, so a range is the normal way to ask.

static int hex_arg(const char *s, long *out) {
    int base = 10, i = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    long v = 0;
    if (!s[i]) return 0;
    for (; s[i]; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else if (base == 16 && s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else if (base == 16 && s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
        else return 0;
        if (d >= base) return 0;
        v = v * base + d;
    }
    *out = v;
    return 1;
}

static int dump_table(const char *sig, long at, long want) {
    struct query_acpidump r;
    char line[128];
    long total = -1, shown = 0;

    for (int i = 0; ; i++) {
        if (sys_query_record(QUERY_ACPIDUMP, (unsigned)i, &r, sizeof r)
            < (int)sizeof r) break;
        if (strncmp(r.signature, sig, 4) != 0) continue;
        if (total < 0) {
            total = (long)r.total;
            snprintf(line, sizeof line, "%s: %ld bytes\n", r.signature, total);
            put(line);
        }
        for (unsigned k = 0; k < r.len; k++) {
            long off = (long)r.offset + (long)k;
            if (off < at) continue;
            if (want > 0 && off >= at + want) continue;
            if ((off % 16) == 0) {
                snprintf(line, sizeof line, "\n  %04lx ", off);
                put(line);
            }
            snprintf(line, sizeof line, " %02x", r.data[k]);
            put(line);
            shown++;
        }
    }
    if (total < 0) {
        snprintf(line, sizeof line, "acpi: no table with signature %s\n", sig);
        put(line);
        return 1;
    }
    put("\n");
    if (!shown) put("acpi: that range is past the end of the table\n");
    return 0;
}

int main(int argc, char **argv) {
    char line[160];
    struct query_acpi a;

    // --dump takes over entirely: it is a different question from the
    // summary, and mixing them would make the output unpasteable.
    if (argc > 1 && !strcmp(argv[1], "--dump")) {
        long at = 0, want = 0;
        if (argc < 3) { cmd_usage("acpi [--dump <SIG> [--at N] [--len N]]"); return 1; }
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--at") && i + 1 < argc) {
                if (!hex_arg(argv[++i], &at)) { cmd_usage("acpi [--dump <SIG> [--at N] [--len N]]"); return 1; }
            } else if (!strcmp(argv[i], "--len") && i + 1 < argc) {
                if (!hex_arg(argv[++i], &want)) { cmd_usage("acpi [--dump <SIG> [--at N] [--len N]]"); return 1; }
            } else {
                cmd_usage("acpi [--dump <SIG> [--at N] [--len N]]");
                return 1;
            }
        }
        return dump_table(argv[2], at, want);
    }
    if (argc > 1) { cmd_usage("acpi [--dump <SIG> [--at N] [--len N]]"); return 1; }

    if (sys_query_record(QUERY_ACPI, 0, &a, sizeof a) < (int)sizeof a) {
        cmd_fail("acpi", 0);
        return 1;
    }

    if (!(a.flags & ACPI_F_TABLES)) {
        // Not an error. A machine can genuinely have no ACPI, and this
        // is exactly the boot where `reboot --poweroff` will halt
        // instead of powering down -- so say both things.
        put("no ACPI tables were found on this machine\n");
        snprintf(line, sizeof line, "RSDP:            %s\n",
                 source_name(a.rsdp_source));
        put(line);
        put("poweroff will fall back to the legacy port write, then halt\n");
        return 0;
    }

    snprintf(line, sizeof line, "RSDP:            revision %llu, from the %s\n",
             (unsigned long long)a.rsdp_revision, source_name(a.rsdp_source));
    put(line);
    snprintf(line, sizeof line, "Root table:      %s at 0x%llx, %llu table(s)\n",
             (a.flags & ACPI_F_XSDT) ? "XSDT" : "RSDT",
             (unsigned long long)a.rsdt_phys, (unsigned long long)a.table_count);
    put(line);
    snprintf(line, sizeof line, "ACPI mode:       %s%s\n",
             yesno(a.flags & ACPI_F_ENABLED),
             (a.flags & ACPI_F_HW_REDUCED) ? "  (hardware-reduced platform)" : "");
    put(line);

    put("\nPower:\n");
    snprintf(line, sizeof line, "  poweroff:      %s",
             yesno(a.flags & ACPI_F_S5));
    put(line);
    if (a.flags & ACPI_F_S5) {
        if (a.flags & ACPI_F_HW_REDUCED)
            snprintf(line, sizeof line, "  -- S5 type %llu to SLEEP_CONTROL_REG 0x%llx\n",
                     (unsigned long long)a.slp_typ_a,
                     (unsigned long long)a.sleep_control_addr);
        else
            snprintf(line, sizeof line, "  -- S5 type %llu to PM1a port 0x%llx%s\n",
                     (unsigned long long)a.slp_typ_a,
                     (unsigned long long)a.pm1a_cnt,
                     a.pm1b_cnt ? " (and PM1b)" : "");
        put(line);
    } else {
        put("  -- no usable _S5_, the legacy port write is what runs\n");
    }

    snprintf(line, sizeof line, "  reset:         %s", yesno(a.flags & ACPI_F_RESET));
    put(line);
    if (a.flags & ACPI_F_RESET) {
        snprintf(line, sizeof line, "  -- value 0x%llx to %s 0x%llx\n",
                 (unsigned long long)a.reset_value, space_name(a.reset_space),
                 (unsigned long long)a.reset_addr);
        put(line);
    } else {
        put("  -- no reset register, the 8042 pulse is what runs\n");
    }
    if (a.smi_cmd) {
        snprintf(line, sizeof line, "  SMI command:   port 0x%llx, enable 0x%llx\n",
                 (unsigned long long)a.smi_cmd, (unsigned long long)a.acpi_enable);
        put(line);
    }

    put("\nTables:\n");
    for (unsigned i = 0; ; i++) {
        struct query_acpi_table t;
        if (sys_query_record(QUERY_ACPI_TABLE, i, &t, sizeof t) < (int)sizeof t) break;
        snprintf(line, sizeof line, "  %-5s 0x%08llx  %6llu bytes  rev %-3llu  %s %s\n",
                 t.signature, (unsigned long long)t.address,
                 (unsigned long long)t.length, (unsigned long long)t.revision,
                 t.oem_id, t.oem_table_id);
        put(line);
    }

    // The MADT's processors, if there was a MADT. Nothing runs on them:
    // this kernel is single-core, and saying so beside the list is what
    // keeps the list from reading as a claim.
    put("\nProcessors (MADT):\n");
    int n = 0;
    for (unsigned i = 0; ; i++) {
        struct query_cpu c;
        if (sys_query_record(QUERY_CPUS, i, &c, sizeof c) < (int)sizeof c) break;
        n++;
        snprintf(line, sizeof line, "  cpu%-3u  %s id %-4llu  acpi id %-4llu  %s  online: no\n",
                 i, (c.flags & ACPI_CPU_X2APIC) ? "x2apic" : "apic  ",
                 (unsigned long long)c.apic_id, (unsigned long long)c.acpi_id,
                 (c.flags & ACPI_CPU_ENABLED) ? "enabled "
                   : (c.flags & ACPI_CPU_ONLINE_CAPABLE) ? "offline " : "disabled");
        put(line);
    }
    if (n == 0) {
        put("  none listed -- no MADT on this machine\n");
    } else {
        snprintf(line, sizeof line,
                 "  %d listed, %llu I/O APIC(s), local APIC at 0x%llx\n"
                 "  This kernel runs on one core; see docs/smp-design.md.\n",
                 n, (unsigned long long)a.ioapic_count,
                 (unsigned long long)a.lapic_phys);
        put(line);
    }

    return 0;
}
