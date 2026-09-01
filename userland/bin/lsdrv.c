// lsdrv -- which drivers this build has, and what each one is driving.
//
// The per-class listings answer a different question. `lsblk` says which
// disks are present, `lsusb` which devices are attached, `ifconfig`
// which cards are configured -- none of them says whether a driver is in
// this build at all, and a driver that bound nothing appears in none of
// them. "Is virtio-blk compiled in?" had no answer short of reading the
// source, and "which driver claimed that USB device?" had one only in
// the boot log, which scrolls away.
//
// The model is Linux's /sys/bus/*/drivers/<drv>/ -- a directory per
// driver with a link per bound device -- and `lspci -k`'s "Kernel driver
// in use". This is the same fact without the filesystem: QUERY_DRIVER.
//
// `-v` prints the SOURCE FILE each driver declared itself in and its
// one-line description, which modinfo carries as `filename:` and
// `description:` for the same reasons. In a hobby OS the question after
// "which driver is this?" is almost always "where is that code?".
//
// A driver appears here whether or not it found hardware -- the
// declaration is data in the image (`.drivers`), not a call inside an
// init() that a "no card on this bus" return can skip.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

#define USAGE "lsdrv [-v]"

int main(int argc, char **argv) {
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else { cmd_usage(USAGE); return 1; }
    }

    printf("%-14s %-8s %s\n", "DRIVER", "CLASS", verbose ? "SOURCE" : "DEVICES");

    int shown = 0;
    for (int i = 0; ; i++) {
        struct query_driver d;
        int n = sys_query_record(QUERY_DRIVER, i, &d, sizeof d);
        if (n < (int)sizeof d) break;
        shown++;
        if (verbose) {
            printf("%-14s %-8s %s\n", d.name, d.cls,
                   d.file[0] ? d.file : "(unknown)");
            // Continuation lines rather than a wider table: a source
            // path is already 30+ columns and an 80-column console has
            // to fit the description and the devices as well.
            if (d.desc[0]) printf("%24s%s\n", "", d.desc);
            printf("%24sdevices: %s\n", "",
                   d.devices[0] ? d.devices : "(none)");
        } else {
            printf("%-14s %-8s %s\n", d.name, d.cls,
                   d.devices[0] ? d.devices : "(none)");
        }
    }

    if (!shown) {
        // Distinguished from "no drivers", which cannot happen: a kernel
        // with no driver at all would not have booted.
        printf("lsdrv: this kernel does not report its drivers\n");
        return 1;
    }
    return 0;
}
