// The first real disk-hosted ELF64 program (see docs/roadmap.md's
// real-disk-hosted-ELF-binaries entry) -- a genuine syscall-driven
// userland process, not a kernel-space shell built-in, that lists the
// PCI devices pci_init() found at boot via the two syscalls added
// alongside this file (SYS_PCI_COUNT/SYS_PCI_INFO, see syscall_abi.h).
// Prints in the same `bus:device.function  vendor:device  class name`
// shape the `lspci` shell command (apps/shell_sys.c's cmd_lspci(),
// kernel-space) already uses, so the two outputs read the same even
// though this one got there through a completely different path (real
// ring-3 syscalls instead of calling pci_device_at()/pci_class_name()
// directly).
//
// Strings and number conversion come from lib/string.h and knum.h,
// linked out of libuapp.a. This file used to carry its own my_strlen(),
// a truncating copy, a decimal digit loop and a hex nibble loop, with a
// comment calling that a deliberate duplicate; it was only ever
// deliberate because the toolkit could not be linked into a ring-3 ELF.
// It can be now, so the copies are gone -- put_udec()/put_hex_digits()
// below are two-line wrappers that pick the buffer and the sink, which
// is the part that really is this file's business.
//
// `pci_class_name()` itself
// can't be called from here even though its declaration is visible via
// "pci.h" (Makefile's USERLAND_CFLAGS pulls in kernel/include) --
// that's kernel-space code in kernel/drivers/pci.c, never linked into
// a userland ELF (see userland/link.ld: one object file, no kernel
// code). So this file carries its own small copy of the same
// class/subclass -> name table instead. If pci_class_name() ever grows
// a new case, this table doesn't pick it up automatically -- the one
// duplication here that is still real, since that table lives in a
// kernel driver rather than in the shared toolkit.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h> // strlen
#include "knum.h"       // k_htoa -- fixed-width hex, which kfmt has no
                         // conversion for (no `*` width in its printf)
#include "pci.h" // struct pci_device only -- see this file's top comment

static void put(const char *s) {
    sys_write(1, s, strlen(s));
}

static void put_udec(uint32_t v) {
    char buf[21]; // k_utoa documents 21 as always sufficient
    k_utoa(v, buf, sizeof buf);
    put(buf);
}

// Fixed-width, no "0x" -- a table column, which is the exact case
// k_htoa's min_digits argument exists for (see knum.h's note on why the
// prefix is the caller's business).
static void put_hex_digits(uint32_t v, int digits) {
    char buf[17];
    k_htoa(v, buf, sizeof buf, (unsigned)digits);
    put(buf);
}

// Small local duplicate of pci_class_name() (kernel/drivers/pci.c) --
// see this file's top comment for why it can't just call that one.
// Same subset of class/subclass pairs (what a QEMU machine or ordinary
// PC actually presents), same fallback for anything else.
static const char *class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x00: return "unclassified device";
        case 0x01:
            switch (subclass) {
                case 0x01: return "IDE controller";
                case 0x06: return "SATA controller";
                default:   return "mass storage controller";
            }
        case 0x02:
            switch (subclass) {
                case 0x00: return "ethernet controller";
                default:   return "network controller";
            }
        case 0x03:
            switch (subclass) {
                case 0x00: return "VGA-compatible controller";
                default:   return "display controller";
            }
        case 0x04: return "multimedia controller";
        case 0x05: return "memory controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI-to-PCI bridge";
                default:   return "bridge device";
            }
        case 0x07: return "communication controller";
        case 0x08: return "system peripheral";
        case 0x09: return "input device controller";
        case 0x0C:
            switch (subclass) {
                case 0x03: return "USB controller";
                default:   return "serial bus controller";
            }
        default: return "unknown device";
    }
}

// ---------------------------------------------------------------------
// Vendor/device names, from the PCI ID Database
// ---------------------------------------------------------------------
//
// /usr/share/hwdata/pci.ids is a verbatim copy of the file the PCI ID
// Project publishes (pci-ids.ucw.cz), seeded onto the disk image from
// seed/ at build time -- the same path real Linux distributions use, and
// the same file `lspci` reads there. See LICENSE's "Third-party data"
// section: it's redistributed under its 3-clause BSD option, not MIT.
//
// The format is two significant levels, tab-indented, sorted by id:
//
//     8086  Intel Corporation
//     <TAB>7010  82371SB PIIX3 IDE [Natoma/Triton II]
//     <TAB><TAB>1af4 1100  Subsystem name        <- ignored here
//
// **Parsed as a single streaming pass, never held in memory.** The file
// is ~1.6MB and this process's heap is a bump allocator (SYS_SBRK) with
// no free -- so the loop below reads 1KB at a time, keeps only the
// current line, and copies out just the handful of names that match a
// device actually present. Peak memory is a few KB regardless of how
// large the database grows. It also means no seeking, which matters:
// SYS_READ advances a per-fd offset and there is no lseek yet.
#define PCI_IDS_PATH "/usr/share/hwdata/pci.ids"
#define MAX_DEVS         32
#define VENDOR_NAME_MAX  40
#define DEVICE_NAME_MAX  64
#define LINE_MAX        256
#define CHUNK           1024 // SYS_WRITE_MAX -- the per-call cap on SYS_READ too

static struct pci_device g_dev[MAX_DEVS];
static char g_vendor_name[MAX_DEVS][VENDOR_NAME_MAX];
static char g_device_name[MAX_DEVS][DEVICE_NAME_MAX];
static int  g_count;

static void put_err(const char *s) {
    sys_call(SYS_WRITE, 2, (uint64_t)(uintptr_t)s, strlen(s));
}

// Exactly `digits` lowercase-or-uppercase hex characters, or -1. Strict
// on purpose: a malformed line should be skipped, not half-parsed into
// a plausible wrong id (the toolkit's "a parser rejects rather than
// guesses" rule, see CLAUDE.md).
static int32_t parse_hex(const char *s, int digits) {
    int32_t v = 0;
    for (int i = 0; i < digits; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

// True once every device has both names, so the scan can stop early
// rather than always reading all 1.6MB. Devices missing from the
// database never resolve, so this is an optimization for the common
// case, not something the loop's correctness depends on.
static int all_resolved(void) {
    for (int i = 0; i < g_count; i++) {
        if (!g_vendor_name[i][0] || !g_device_name[i][0]) return 0;
    }
    return 1;
}

static void handle_line(char *line, int32_t *cur_vendor) {
    if (line[0] == '#' || line[0] == '\0') return;

    if (line[0] != '\t') {                       // vendor: "8086  Intel Corporation"
        int32_t id = parse_hex(line, 4);
        if (id < 0) return;
        *cur_vendor = id;
        const char *name = line + 4;
        while (*name == ' ') name++;
        for (int i = 0; i < g_count; i++) {
            if (g_dev[i].vendor_id == (uint16_t)id && !g_vendor_name[i][0]) {
                strlcpy(g_vendor_name[i], name, VENDOR_NAME_MAX);
            }
        }
        return;
    }

    if (line[1] == '\t') return;                 // subsystem line -- not used here
    if (*cur_vendor < 0) return;                 // device line before any vendor: malformed

    int32_t id = parse_hex(line + 1, 4);         // device: "\t7010  82371SB PIIX3 IDE"
    if (id < 0) return;
    const char *name = line + 5;
    while (*name == ' ') name++;
    for (int i = 0; i < g_count; i++) {
        if (g_dev[i].vendor_id == (uint16_t)*cur_vendor &&
            g_dev[i].device_id == (uint16_t)id && !g_device_name[i][0]) {
            strlcpy(g_device_name[i], name, DEVICE_NAME_MAX);
        }
    }
}

// Fills in whatever names the database has. Silent no-op if the file
// isn't there -- the numeric output below still works, which is the
// point of keeping the two separable.
static void load_names(void) {
    int64_t fd = sys_open(PCI_IDS_PATH, 0);
    if (fd < 0) {
        put_err("lspci: " PCI_IDS_PATH " not found -- showing numeric ids only\n");
        return;
    }

    char chunk[CHUNK];
    char line[LINE_MAX];
    uint64_t line_len = 0;
    int32_t cur_vendor = -1;
    int overlong = 0; // dropping the tail of a too-long line, not restarting mid-way

    for (;;) {
        int64_t n = sys_read((int)fd, chunk, CHUNK);
        if (n <= 0) break;
        for (int64_t i = 0; i < n; i++) {
            char c = chunk[i];
            if (c != '\n') {
                if (line_len + 1 < LINE_MAX) line[line_len++] = c;
                else overlong = 1;
                continue;
            }
            line[line_len] = '\0';
            if (!overlong) handle_line(line, &cur_vendor);
            line_len = 0;
            overlong = 0;
        }
        if (all_resolved()) break;
    }

    if (line_len > 0 && !overlong) {  // last line without a trailing newline
        line[line_len] = '\0';
        handle_line(line, &cur_vendor);
    }
    sys_close((int)fd);
}

int main(void) {
    int64_t count = sys_pci_count();
    if (count <= 0) {
        put("No PCI devices found.\n");
        sys_exit(0);
    }
    if (count > MAX_DEVS) count = MAX_DEVS; // more than this and names are the least of it

    for (int64_t i = 0; i < count; i++) {
        if (sys_pci_info((int)i, &g_dev[g_count]) == 1) g_count++;
    }

    load_names();

    for (int i = 0; i < g_count; i++) {
        const struct pci_device *dev = &g_dev[i];

        put_hex_digits(dev->bus, 2);
        put(":");
        put_hex_digits(dev->device, 2);
        put(".");
        put_hex_digits(dev->function, 1);
        put("  ");
        put_hex_digits(dev->vendor_id, 4);
        put(":");
        put_hex_digits(dev->device_id, 4);
        put("  ");
        put(class_name(dev->class_code, dev->subclass));

        // IRQ line and nonzero BARs, matching what the kernel-side
        // cmd_lspci() printed before it started deferring to this
        // binary -- otherwise moving to one implementation would have
        // quietly dropped output that already existed. Same two-line
        // decode as pci.c's pci_bar_is_io()/pci_bar_addr(), which is
        // kernel-space and can't be called from ring 3 (see this file's
        // top comment); it's three bits of masking, not worth a syscall.
        if (dev->interrupt_line != 0 && dev->interrupt_line != 0xFF) {
            put("  irq ");
            put_udec(dev->interrupt_line);
        }
        for (int b = 0; b < 6; b++) {
            uint32_t bar = dev->bar[b];
            if (bar == 0) continue;
            int is_io = (bar & 0x1) != 0;
            put("  bar");
            put_udec((uint32_t)b);
            put("=0x");
            put_hex_digits(is_io ? (bar & 0xFFFFFFFCu) : (bar & 0xFFFFFFF0u), 8);
            put(is_io ? "(io)" : "(mem)");
        }
        put("\n");

        // Names on their own indented line rather than appended: a
        // device name alone can be 60+ characters, and keeping the
        // first line's columns fixed means the numeric output still
        // lines up exactly as it did before this existed.
        if (g_vendor_name[i][0] || g_device_name[i][0]) {
            put("           ");
            put(g_vendor_name[i][0] ? g_vendor_name[i] : "(unknown vendor)");
            if (g_device_name[i][0]) {
                put("  ");
                put(g_device_name[i]);
            }
            put("\n");
        }
    }

    sys_exit(0);
}
