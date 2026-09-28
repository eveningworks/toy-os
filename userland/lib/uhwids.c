// The pci.ids / usb.ids resolver -- see uhwids.h.
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "lib/uhwids.h"

// The format is two significant levels, tab-indented, sorted by id:
//
//     8086  Intel Corporation
//     <TAB>7010  82371SB PIIX3 IDE [Natoma/Triton II]
//     <TAB><TAB>1af4 1100  Subsystem name        <- ignored here
//
// and, after every vendor, the CLASS section in the same shape:
//
//     C 04  Multimedia controller
//     <TAB>03  Audio device
//     <TAB><TAB>00  prog-if name                  <- ignored here
//
// **THE CLASS SECTION IS THE LAST THING IN THE FILE**, so naming classes
// means reading all of it -- the early exit below only stops once every
// class is named too, which is at the end.
//
// usb.ids has the same two levels and its own class section, which is
// not read (a USB caller asks for no class). Its later sections -- "AT",
// "HID", "L" and the rest -- start with a word that is not four hex
// digits, so they end the vendor section rather than joining it.

#define LINE_MAX_ 256
#define CHUNK     16384   // the whole file is read, and 1 KiB reads cost 16x the syscalls

// Exactly `digits` hex characters, or -1. Strict on purpose: a malformed
// line is skipped, not half-parsed into a plausible wrong id.
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

static const char *after_id(const char *s) {
    while (*s == ' ') s++;
    return s;
}

struct scan {
    struct uhwids_entry *e;
    int n;
    int32_t vendor, cls;   // the section being read, -1 outside one
};

static void on_line(struct scan *sc, const char *line) {
    if (line[0] == '#' || line[0] == '\0') return;

    // The class section: "C 04  Multimedia controller", then "\t03  Audio
    // device" under it, then "\t\t00  ..." prog-ifs, which are not read.
    if (line[0] == 'C' && line[1] == ' ') {
        sc->vendor = -1;
        sc->cls = parse_hex(line + 2, 2);
        if (sc->cls < 0) return;
        for (int i = 0; i < sc->n; i++)
            if (sc->e[i].cls == sc->cls && !sc->e[i].class_name[0])
                strlcpy(sc->e[i].class_name, after_id(line + 4), sizeof sc->e[i].class_name);
        return;
    }
    if (line[0] == '\t' && sc->cls >= 0) {
        if (line[1] == '\t') return;
        int32_t sub = parse_hex(line + 1, 2);
        if (sub < 0) return;
        for (int i = 0; i < sc->n; i++)
            if (sc->e[i].cls == sc->cls && sc->e[i].subclass == sub && !sc->e[i].subclass_name[0])
                strlcpy(sc->e[i].subclass_name, after_id(line + 3), sizeof sc->e[i].subclass_name);
        return;
    }

    if (line[0] != '\t') {                       // vendor: "8086  Intel Corporation"
        sc->cls = -1;
        int32_t id = parse_hex(line, 4);
        if (id < 0 || line[4] != ' ') { sc->vendor = -1; return; }
        sc->vendor = id;
        for (int i = 0; i < sc->n; i++)
            if (sc->e[i].vendor == (uint16_t)id && !sc->e[i].vendor_name[0])
                strlcpy(sc->e[i].vendor_name, after_id(line + 4), sizeof sc->e[i].vendor_name);
        return;
    }
    if (line[1] == '\t' || sc->vendor < 0) return;   // a subsystem line, or no vendor yet
    int32_t id = parse_hex(line + 1, 4);           // device: "\t7010  82371SB PIIX3 IDE"
    if (id < 0) return;
    for (int i = 0; i < sc->n; i++)
        if (sc->e[i].vendor == (uint16_t)sc->vendor && sc->e[i].device == (uint16_t)id &&
            !sc->e[i].device_name[0])
            strlcpy(sc->e[i].device_name, after_id(line + 5), sizeof sc->e[i].device_name);
}

static int all_resolved(const struct scan *sc) {
    for (int i = 0; i < sc->n; i++) {
        const struct uhwids_entry *x = &sc->e[i];
        if (!x->vendor_name[0] || !x->device_name[0]) return 0;
        if (x->cls >= 0 && !x->subclass_name[0]) return 0;
    }
    return 1;
}

int uhwids_resolve(const char *path, struct uhwids_entry *e, int n) {
    for (int i = 0; i < n; i++)
        e[i].vendor_name[0] = e[i].device_name[0] = e[i].class_name[0] = e[i].subclass_name[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    static char chunk[CHUNK];   // static: 16 KiB is not a stack frame
    char line[LINE_MAX_];
    unsigned len = 0;
    int overlong = 0;           // dropping a too-long line's tail, not restarting mid-way
    struct scan sc = { e, n, -1, -1 };

    for (;;) {
        long got = read(fd, chunk, CHUNK);
        if (got <= 0) break;
        for (long i = 0; i < got; i++) {
            char c = chunk[i];
            if (c != '\n') {
                if (len + 1 < LINE_MAX_) line[len++] = c;
                else overlong = 1;
                continue;
            }
            line[len] = '\0';
            if (!overlong) on_line(&sc, line);
            len = 0;
            overlong = 0;
        }
        if (all_resolved(&sc)) break;
    }
    if (len > 0 && !overlong) {       // a last line with no newline
        line[len] = '\0';
        on_line(&sc, line);
    }
    close(fd);
    return 0;
}
