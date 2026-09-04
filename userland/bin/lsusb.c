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
#include <fcntl.h>
#include <unistd.h>

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
//
// SCOPED TO CLASS 3, and that is the whole point of the first argument:
// subclass 1 means the boot protocol only for HID. On an audio device
// it is AudioControl, and this helper without the class check reported
// a USB DAC as "Class 01 Audio, Boot Interface Subclass".
static const char *hid_protocol_name(unsigned long long cls,
                                     unsigned long long sub,
                                     unsigned long long proto) {
    if (cls != 3 || sub != 1) return "";
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
    int64_t fd = open(USB_IDS_PATH, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "lsusb: %s not found -- showing numeric ids only\n",
                USB_IDS_PATH);
        return;
    }
    char chunk[CHUNK], line[LINE_MAX];
    unsigned line_len = 0;
    int cur_vendor = -1, overlong = 0;

    for (;;) {
        int64_t n = read((int)fd, chunk, CHUNK);
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
    close((int)fd);
}

// --- the raw configuration descriptor (-D) ----------------------------
//
// The bytes a device sent about itself, unwalked. This exists because
// the interesting half of a device no driver here binds is exactly the
// class-specific descriptors nothing looked at -- and because a hex
// dump pastes straight into a KTEST fixture, which is the only way a
// device nobody owns is ever tested against (sound_usb.c's fixture is
// QEMU's, captured the same way).
//
// The decode below is a SUMMARY, not a full one: enough to answer "why
// did no driver take this", which for an audio device means the UAC
// version, the alternate settings and what each one's format is.

#define DESC_MAX 4096
static uint8_t g_desc[DESC_MAX];

// Assembles one device's configuration out of QUERY_USBDESC's slices.
// Returns its length, or 0. The slices carry their own offset, so a
// short or reordered read leaves a hole rather than a splice.
static unsigned load_desc(unsigned long long slot) {
    struct query_usbdesc r;
    unsigned len = 0;
    QUERY_FOREACH(QUERY_USBDESC, r, i) {
        if (r.slot != slot) continue;
        if (r.offset >= DESC_MAX || r.len > DESC_MAX - r.offset) continue;
        memcpy(g_desc + r.offset, r.data, r.len);
        if (r.offset + r.len > len) len = r.offset + r.len;
    }
    return len;
}

static void hex_dump(const uint8_t *b, unsigned n) {
    for (unsigned o = 0; o < n; o += 16) {
        printf("    %04x ", o);
        for (unsigned i = 0; i < 16 && o + i < n; i++)
            printf(" %02x", b[o + i]);
        printf("\n");
    }
}

static const char *ep_type_name(unsigned attr) {
    switch (attr & 3) {  // dispatch-ok: the four transfer types, and there are four
        case 0:  return "control";
        case 1:  return "isochronous";
        case 2:  return "bulk";
        default: return "interrupt";
    }
}

// An isochronous endpoint's synchronisation type, which is the field
// that says whether the device expects a feedback endpoint to tell the
// host how fast to send.
static const char *ep_sync_name(unsigned attr) {
    if ((attr & 3) != 1) return "";
    switch ((attr >> 2) & 3) {  // dispatch-ok: two bits
        case 0:  return " sync=none";
        case 1:  return " sync=async";
        case 2:  return " sync=adaptive";
        default: return " sync=sync";
    }
}

static const char *ac_subtype_name(unsigned st) {
    switch (st) {  // dispatch-ok: the AudioControl subtypes, a closed set
        case 0x01: return "HEADER";
        case 0x02: return "INPUT_TERMINAL";
        case 0x03: return "OUTPUT_TERMINAL";
        case 0x04: return "MIXER_UNIT";
        case 0x05: return "SELECTOR_UNIT";
        case 0x06: return "FEATURE_UNIT";
        case 0x07: return "PROCESSING_UNIT";
        case 0x08: return "EXTENSION_UNIT";
        case 0x0a: return "CLOCK_SOURCE";
        case 0x0b: return "CLOCK_SELECTOR";
        case 0x0c: return "CLOCK_MULTIPLIER";
        default:   return "?";
    }
}

static unsigned le24(const uint8_t *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
}

// One audio class-specific interface descriptor. `uac2` selects the
// layout: UAC1 and UAC2 share subtype numbers and agree on almost
// nothing else -- a UAC2 FORMAT_TYPE carries no sample rate at all
// (the rate lives in a CLOCK_SOURCE and is set by a class request),
// which is the single most useful thing this dump can tell you.
static void print_audio_cs(const uint8_t *d, unsigned blen, int streaming,
                           int uac2) {
    unsigned st = d[2];
    if (!streaming) {
        printf("      AC %s", ac_subtype_name(st));
        if (st == 0x01 && blen >= 5)
            printf(", UAC %x.%02x", d[4], d[3]);
        else if (st == 0x06 && blen >= 5)
            printf(", unit %u, source %u", d[3], d[4]);
        else if (st == 0x0a && blen >= 6)
            printf(", clock %u, attr %02x, controls %02x", d[3], d[4], d[5]);
        else if ((st == 0x02 || st == 0x03) && blen >= 5)
            printf(", terminal %u, type %04x", d[3],
                   (unsigned)(d[4] | ((unsigned)d[5] << 8)));
        printf("\n");
        return;
    }

    if (st == 0x01) {                       // AS_GENERAL
        if (uac2 && blen >= 16)
            printf("      AS_GENERAL, terminal %u, formats %02x, %u channel(s)\n",
                   d[3], d[8], d[10]);
        else if (blen >= 7)
            printf("      AS_GENERAL, terminal %u, format tag %04x\n",
                   d[3], (unsigned)(d[5] | ((unsigned)d[6] << 8)));
        return;
    }
    if (st != 0x02) {                       // not FORMAT_TYPE
        printf("      AS subtype %02x\n", st);
        return;
    }
    if (uac2) {
        if (blen >= 6)
            printf("      FORMAT_TYPE %u, %u byte(s)/sample, %u bit"
                   "  (rate is the CLOCK_SOURCE's, not here)\n",
                   d[3], d[4], d[5]);
        return;
    }
    if (blen < 8) return;
    printf("      FORMAT_TYPE %u, %u channel(s), %u byte(s)/sample, %u bit, ",
           d[3], d[4], d[5], d[6]);
    unsigned freq_type = d[7];
    if (freq_type == 0 && blen >= 14) {
        printf("%u-%u Hz continuous\n", le24(d + 8), le24(d + 11));
        return;
    }
    printf("rates");
    for (unsigned i = 0; i < freq_type && 8 + i * 3 + 3 <= blen; i++)
        printf(" %u", le24(d + 8 + i * 3));
    printf("\n");
}

// Walks the configuration once, one line per descriptor. Bounds-checked
// the same way the kernel's walk is: a bLength that does not fit stops
// the walk and says so, rather than being followed.
static void decode_desc(const uint8_t *cfg, unsigned total) {
    unsigned o = 0;
    unsigned cls = 0, sub = 0, proto = 0;   // the interface in force
    while (o + 2 <= total) {
        unsigned blen = cfg[o], type = cfg[o + 1];
        if (blen < 2 || o + blen > total) {
            printf("    (malformed descriptor at offset %u)\n", o);
            return;
        }
        const uint8_t *d = cfg + o;
        if (type == 0x02 && blen >= 9) {
            printf("    Configuration %u: %u interface(s), %u mA\n",
                   d[5], d[4], (unsigned)d[8] * 2);
        } else if (type == 0x0b && blen >= 8) {
            printf("    Interface association: first %u, count %u, "
                   "class %02x/%02x/%02x\n", d[2], d[3], d[4], d[5], d[6]);
        } else if (type == 0x04 && blen >= 9) {
            cls = d[5]; sub = d[6]; proto = d[7];
            printf("    Interface %u alt %u: class %02x %s, subclass %02x, "
                   "protocol %02x, %u endpoint(s)\n",
                   d[2], d[3], cls, class_name(cls), sub, proto, d[4]);
        } else if (type == 0x05 && blen >= 7) {
            unsigned addr = d[2], attr = d[3];
            unsigned w = (unsigned)(d[4] | ((unsigned)d[5] << 8));
            unsigned mps = w & 0x7ff, mult = ((w >> 11) & 3) + 1;
            printf("      Endpoint %02x %s %s%s, %u B/interval",
                   addr, ep_type_name(attr), (addr & 0x80) ? "IN" : "OUT",
                   ep_sync_name(attr), mps * mult);
            if (mult > 1) printf(" (%u x %u)", mps, mult);
            printf(", bInterval %u\n", d[6]);
        } else if (type == 0x24 && blen >= 3 && cls == 1) {
            print_audio_cs(d, blen, sub == 2, proto == 0x20);
        } else if (type == 0x25 && blen >= 3) {
            printf("      CS_ENDPOINT, subtype %02x\n", d[2]);
        } else {
            printf("    Descriptor type %02x, %u bytes\n", type, blen);
        }
        o += blen;
    }
}

int main(int argc, char **argv) {
    int verbose = 0, dump = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "-D") || !strcmp(argv[i], "--descriptors"))
            dump = 1;
        else { cmd_usage("lsusb [-v] [-D]"); return 1; }
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
               hid_protocol_name(d->if_class, d->if_subclass,
                                 d->if_protocol));
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
        if (dump) {
            unsigned n = load_desc(d->slot);
            if (!n) {
                printf("  (no configuration descriptor kept for this device)\n");
                continue;
            }
            printf("  Configuration descriptor, %u bytes:\n", n);
            hex_dump(g_desc, n);
            decode_desc(g_desc, n);
        }
    }
    return 0;
}
