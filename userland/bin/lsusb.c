// lsusb -- the USB devices the xHCI driver enumerated.
//
// Reads QUERY_USB (a provider, not a syscall of its own -- CLAUDE.md's
// rule for facts) and resolves the numeric ids through
// /usr/share/hwdata/usb.ids, exactly as /bin/lspci resolves PCI ids
// through pci.ids beside it: same directory, same streaming parse, same
// licence carve-out in LICENSE.
//
// DATABASE NAME BY DEFAULT, THE DEVICE'S OWN STRINGS UNDER -v. That is
// what real lsusb does, and the two genuinely differ: a USB device
// carries Manufacturer and Product string descriptors, which PCI has no
// equivalent of, and they frequently disagree with the database. The
// database is the stable identity; the strings are what this particular
// device claims to be.
//
// The strings are therefore behind a flag that nobody types, which is
// how a code path rots -- so tools/usb_test.py asserts on `lsusb -v`
// specifically, not just on the default output.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include "query_abi.h"

#define USB_IDS_PATH "/usr/share/hwdata/usb.ids"
#define MAX_DEVS         8
#define VENDOR_NAME_MAX  48
#define PRODUCT_NAME_MAX 64
#define LINE_MAX        256
#define CHUNK           1024   // SYS_WRITE_MAX also caps a single SYS_READ

static struct query_usb g_dev[MAX_DEVS];
static char g_vendor_name[MAX_DEVS][VENDOR_NAME_MAX];
static char g_product_name[MAX_DEVS][PRODUCT_NAME_MAX];
static int  g_count;

static const char *speed_name(unsigned long long s) {
    switch (s) {  // dispatch-ok: bounded by QUERY_USB_SPEED_*
        case QUERY_USB_SPEED_LOW:   return "1.5M";
        case QUERY_USB_SPEED_FULL:  return "12M";
        case QUERY_USB_SPEED_HIGH:  return "480M";
        case QUERY_USB_SPEED_SUPER: return "5G";
        default:                    return "?";
    }
}

// The USB class codes worth naming. Deliberately short: this is the
// handful a person reading `lsusb` on this OS can actually encounter,
// and usb.ids' own class section would be a second parser for the sake
// of names nothing here reports.
static const char *class_name(unsigned long long c) {
    switch (c) {  // dispatch-ok: a deliberately partial, bounded list
        case 0x00: return "(defined at interface level)";
        case 0x01: return "Audio";
        case 0x03: return "Human Interface Device";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0E: return "Video";
        case 0xE0: return "Wireless";
        case 0xFF: return "Vendor Specific";
        default:   return "Unknown";
    }
}

// A HID interface's protocol, which is the part that says whether a
// keyboard is actually usable by a boot-protocol driver.
static const char *hid_protocol_name(unsigned long long sub,
                                     unsigned long long proto) {
    if (sub != 1) return "";           // not the boot subclass
    if (proto == 1) return ", Boot Interface Subclass, Keyboard";
    if (proto == 2) return ", Boot Interface Subclass, Mouse";
    return ", Boot Interface Subclass";
}

// Exactly `digits` hex characters, or -1. Strict on purpose: a
// malformed line is skipped rather than half-parsed into a plausible
// wrong id -- the same rule lspci.c's parse_hex() states.
static int parse_hex(const char *s, int digits) {
    int v = 0;
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

static int all_resolved(void) {
    for (int i = 0; i < g_count; i++)
        if (!g_vendor_name[i][0] || !g_product_name[i][0]) return 0;
    return 1;
}

// usb.ids has the same two significant levels pci.ids does:
//
//     0627  Adomax Technology Co., Ltd
//     <TAB>0001  QEMU USB Keyboard
//
// plus a trailing class section beginning "C 00", which this ignores.
static void handle_line(char *line, int *cur_vendor) {
    if (!line[0] || line[0] == '#') return;

    if (line[0] != '\t') {
        // A vendor line -- or the class section, which starts with a
        // letter and must not be parsed as a vendor.
        int id = parse_hex(line, 4);
        if (id < 0) { *cur_vendor = -1; return; }
        if (line[4] != ' ') { *cur_vendor = -1; return; }
        *cur_vendor = id;
        const char *name = line + 4;
        while (*name == ' ') name++;
        for (int i = 0; i < g_count; i++)
            if (g_dev[i].vendor_id == (unsigned)id && !g_vendor_name[i][0])
                strlcpy(g_vendor_name[i], name, VENDOR_NAME_MAX);
        return;
    }

    if (*cur_vendor < 0) return;          // a product line before any vendor
    if (line[1] == '\t') return;          // a third level; not used here
    int id = parse_hex(line + 1, 4);
    if (id < 0) return;
    const char *name = line + 5;
    while (*name == ' ') name++;
    for (int i = 0; i < g_count; i++)
        if (g_dev[i].vendor_id == (unsigned)*cur_vendor &&
            g_dev[i].product_id == (unsigned)id && !g_product_name[i][0])
            strlcpy(g_product_name[i], name, PRODUCT_NAME_MAX);
}

// Streams the database rather than holding it. It is ~730 KB and this
// process's heap is a bump allocator with no free, so the loop keeps
// one line and copies out only the names of devices actually present --
// peak memory is a few KB however large the database grows. Identical
// reasoning to lspci.c, which says it at length.
static void load_names(void) {
    int64_t fd = sys_open(USB_IDS_PATH, 0);
    if (fd < 0) {
        fprintf(stderr, "lsusb: %s not found -- showing numeric ids only\n",
                USB_IDS_PATH);
        return;
    }
    char chunk[CHUNK], line[LINE_MAX];
    unsigned line_len = 0;
    int cur_vendor = -1, overlong = 0;

    for (;;) {
        int64_t n = sys_read((int)fd, chunk, CHUNK);
        if (n <= 0) break;
        for (int64_t i = 0; i < n; i++) {
            char c = chunk[i];
            if (c != '\n') {
                if (line_len + 1 < LINE_MAX) line[line_len++] = c;
                else overlong = 1;      // drop the tail, do not restart mid-line
                continue;
            }
            line[line_len] = '\0';
            if (!overlong) handle_line(line, &cur_vendor);
            line_len = 0; overlong = 0;
        }
        if (all_resolved()) break;
    }
    if (line_len > 0 && !overlong) {
        line[line_len] = '\0';
        handle_line(line, &cur_vendor);
    }
    sys_close((int)fd);
}

int main(int argc, char **argv) {
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = 1;
        else { cmd_usage("lsusb [-v]"); return 1; }
    }

    for (int i = 0; i < MAX_DEVS; i++) {
        if (sys_query_record(QUERY_USB, (unsigned)i, &g_dev[i],
                             sizeof g_dev[i]) < (int)sizeof g_dev[i]) break;
        g_count++;
    }
    if (!g_count) {
        // Says which of the two it is. "No devices" on a machine with no
        // controller reads as a driver failure; it is not one.
        printf("No USB devices. (lsdev says whether a controller was found.)\n");
        return 0;
    }

    load_names();

    for (int i = 0; i < g_count; i++) {
        struct query_usb *d = &g_dev[i];
        printf("Port %u Device %u: ID %04x:%04x %s\n",
               (unsigned)d->port, (unsigned)d->slot,
               (unsigned)d->vendor_id, (unsigned)d->product_id,
               g_vendor_name[i][0] ? g_vendor_name[i] : "(unknown vendor)");
        printf("  %s%s, %sb/s%s\n",
               g_product_name[i][0] ? g_product_name[i] : "(unknown product)",
               "", speed_name(d->speed), d->bound ? "" : ", no driver");
        printf("  Class %02x %s%s\n",
               (unsigned)d->if_class, class_name(d->if_class),
               hid_protocol_name(d->if_subclass, d->if_protocol));
        if (verbose) {
            // The device's OWN account of itself, which is the half the
            // database cannot give and can legitimately disagree with.
            printf("  iManufacturer  %s\n",
                   d->manufacturer[0] ? d->manufacturer : "(none)");
            printf("  iProduct       %s\n",
                   d->product[0] ? d->product : "(none)");
            printf("  bDeviceClass   %02x\n", (unsigned)d->dev_class);
            printf("  Interface      class %02x subclass %02x protocol %02x\n",
                   (unsigned)d->if_class, (unsigned)d->if_subclass,
                   (unsigned)d->if_protocol);
        }
    }
    return 0;
}
