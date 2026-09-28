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
// A class is named from pci.ids's class section when it has one, as
// pciutils' lspci does, and from the kernel's own table otherwise
// (pci_class_name(), kernel/lib/pci_class.c, compiled into libuapp too).
#include <stdint.h>
#include "rt/sys.h"
#include <string.h> // strlen
#include "knum.h"       // k_htoa -- fixed-width hex, which kfmt has no
                         // conversion for (no `*` width in its printf)
#include "pci.h" // struct pci_device, pci_class_name()
#include <unistd.h>
#include <stdlib.h>   // system() -- see update_ids()
#include "lib/cmd.h"
#include "lib/uhwids.h"
#include "query_abi.h" // QUERY_PCIDEV -- the driver, and who claimed it

static void put(const char *s) {
    write(1, s, strlen(s));
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

// ---------------------------------------------------------------------
// Vendor, device and class names, from the PCI ID Database
// ---------------------------------------------------------------------
//
// /usr/share/hwdata/pci.ids is a verbatim copy of the file the PCI ID
// Project publishes (pci-ids.ucw.cz), seeded onto the disk image from
// seed/ at build time -- the same path real Linux distributions use, and
// the same file `lspci` reads there. See LICENSE's "Third-party data"
// section: it's redistributed under its 3-clause BSD option, not MIT.
//
// Parsed by lib/uhwids.c, which lsusb and the Device Manager share; the
// format, and why it is streamed rather than held, are described there.
#define MAX_DEVS         32
#define VENDOR_NAME_MAX  40
#define DEVICE_NAME_MAX  64
#define CLASS_NAME_MAX  48

static struct pci_device g_dev[MAX_DEVS];
static char g_vendor_name[MAX_DEVS][VENDOR_NAME_MAX];
static char g_device_name[MAX_DEVS][DEVICE_NAME_MAX];
// From pci.ids's class section: the SUBCLASS's name when it has one,
// which is the more specific, else the class's.
static char g_subclass_name[MAX_DEVS][CLASS_NAME_MAX];
static char g_class_name[MAX_DEVS][CLASS_NAME_MAX];
static int  g_count;

static void put_err(const char *s) {
    sys_call(SYS_WRITE, 2, (uint64_t)(uintptr_t)s, strlen(s));
}

// The name printed for device `i`: the database's, most specific first,
// then the built-in table's.
static const char *class_label(int i) {
    if (g_subclass_name[i][0]) return g_subclass_name[i];
    if (g_class_name[i][0]) return g_class_name[i];
    return pci_class_name(g_dev[i].class_code, g_dev[i].subclass);
}

// Fills in whatever names the database has (lib/uhwids.c, shared with
// lsusb and the Device Manager). Silent past one line if the file isn't
// there -- the numeric output below still works, which is the point of
// keeping the two separable.
static void load_names(void) {
    static struct uhwids_entry e[MAX_DEVS];   // static: ~16 KiB is not a stack frame
    for (int i = 0; i < g_count; i++) {
        e[i].vendor = g_dev[i].vendor_id;
        e[i].device = g_dev[i].device_id;
        e[i].cls = g_dev[i].class_code;
        e[i].subclass = g_dev[i].subclass;
    }
    if (uhwids_resolve(UHWIDS_PCI, e, g_count) < 0) {
        put_err("lspci: " UHWIDS_PCI " not found -- showing numeric ids only\n");
        return;
    }
    for (int i = 0; i < g_count; i++) {
        strlcpy(g_vendor_name[i], e[i].vendor_name, VENDOR_NAME_MAX);
        strlcpy(g_device_name[i], e[i].device_name, DEVICE_NAME_MAX);
        strlcpy(g_class_name[i], e[i].class_name, CLASS_NAME_MAX);
        strlcpy(g_subclass_name[i], e[i].subclass_name, CLASS_NAME_MAX);
    }
}

#define USAGE "lspci [-k] [--update]"

// `lspci --update` is `hwdata update pci`, and it EXECS it rather than
// repeating it. The fetch reaches TLS, and linking libhttp/libssl into
// lspci would put mbedTLS behind a command whose whole job is to print
// a table -- so the logic has one home (userland/bin/hwdata.c) and this
// is a signpost to it from where somebody is standing when they notice
// the names are stale.
static int update_ids(void) {
    return system("/bin/hwdata update pci");
}

// WHO HAS EACH DEVICE, by enumeration index: the bound ring-0 driver
// and the process that has claimed it (docs/umdf-design.md stage 2).
// Read once into an array rather than queried per row, since the
// listing walks devices and the query walks the same order.
static uint32_t g_index[MAX_DEVS];  // each row's PCI enumeration index
static struct query_pcidev g_own[MAX_DEVS];
static int g_own_count;

static void load_owners(void) {
    struct query_pcidev q;
    QUERY_FOREACH(QUERY_PCIDEV, q, i) {
        if (g_own_count >= MAX_DEVS) break;
        g_own[g_own_count++] = q;
    }
}

static const struct query_pcidev *owner_of(uint32_t index) {
    for (int i = 0; i < g_own_count; i++)
        if (g_own[i].index == index) return &g_own[i];
    return 0;
}

int main(int argc, char **argv) {
    int kernel_drivers = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--update")) return update_ids();
        if (!strcmp(argv[i], "-k")) { kernel_drivers = 1; continue; }
        cmd_usage(USAGE);
        return 1;
    }

    int64_t count = sys_pci_count();
    if (count <= 0) {
        put("No PCI devices found.\n");
        sys_exit(0);
    }
    if (count > MAX_DEVS) count = MAX_DEVS; // more than this and names are the least of it

    for (int64_t i = 0; i < count; i++) {
        // The ENUMERATION index is kept, not assumed to be the row
        // number: a device whose info read fails leaves a gap, and
        // -k's lookup is by the index the kernel names.
        if (sys_pci_info((int)i, &g_dev[g_count]) != 0) continue;
        g_index[g_count++] = (uint32_t)i;
    }

    load_names();
    if (kernel_drivers) load_owners();

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
        put(class_label(i));

        // IRQ line and nonzero BARs, matching what the kernel-side
        // cmd_lspci() printed before it started deferring to this
        // binary -- otherwise moving to one implementation would have
        // quietly dropped output that already existed. Same two-line
        // decode as pci.c's pci_bar_is_io()/pci_bar_addr(), which is
        // kernel-space and can't be called from ring 3 (see this file's
        // top comment); it's three bits of masking, not worth a syscall.
        // What this device is ACTUALLY on, then what it could have
        // been. A driver that took a vector had its INTx pin disabled,
        // so printing the routed line there would name something the
        // device can no longer assert.
        if (dev->irq_vector) {
            put(dev->irq_msix ? "  msix vector " : "  msi vector ");
            put_udec(dev->irq_vector);
        } else if (dev->interrupt_line != 0 && dev->interrupt_line != 0xFF) {
            put("  irq ");
            put_udec(dev->interrupt_line);
        }
        if (dev->msix_cap) {
            put("  [msix/");
            put_udec(dev->msix_entries);
            put("]");
        } else if (dev->msi_cap) {
            put("  [msi]");
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
            if (!is_io && dev->bar_size[b]) {
                // Sized in the kernel at enumeration; a whole unit or nothing.
                uint64_t sz = dev->bar_size[b];
                put("/");
                if (sz >= (1ull << 30) && !(sz & ((1ull << 30) - 1))) { put_udec((uint32_t)(sz >> 30)); put("G"); }
                else if (sz >= (1ull << 20) && !(sz & ((1ull << 20) - 1))) { put_udec((uint32_t)(sz >> 20)); put("M"); }
                else if (sz >= (1ull << 10) && !(sz & ((1ull << 10) - 1))) { put_udec((uint32_t)(sz >> 10)); put("K"); }
                else put_udec((uint32_t)sz);
            }
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

        // `lspci -k`'s line, and the same fact Linux's prints: which
        // driver is in use. The second half is toy-os's own -- a device
        // a RING-3 process has taken off the kernel names that process
        // instead (docs/umdf-design.md).
        if (kernel_drivers) {
            const struct query_pcidev *o = owner_of(g_index[i]);
            put("           ");
            if (o && o->holder_pid) {
                put("claimed by pid ");
                put_udec((uint32_t)o->holder_pid);
            } else if (o && o->driver[0]) {
                put("kernel driver: ");
                put(o->driver);
            } else {
                put("no driver");
            }
            // Whether a process could take it off the kernel. Said
            // only where a driver HOLDS it, because that is where the
            // answer is not obvious -- "no driver" already means free.
            if (o && o->claimable && o->driver[0] && !o->holder_pid)
                put("  (claimable)");
            put("\n");
        }
    }

    sys_exit(0);
}
