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
// **BY DEFAULT IT LISTS ONLY WHAT IS ACTUALLY DRIVING SOMETHING**, and
// `-a` lists every driver in the build. Both questions are real, but
// they are asked at different rates: "what is running this machine" is
// the everyday one, and a bare listing that was mostly `(none)` buried
// it -- on a QEMU guest more than half the rows bind nothing. That is
// `lspci -k`'s default too, which names a driver only where one is in
// use.
//
// The `-a` half is not a nicety. A driver appears there whether or not
// it found hardware, because the declaration is data in the image
// (`.drivers`) and not a call inside an init() that a "no card on this
// bus" return can skip -- so `-a` is the only way to ask "is virtio-blk
// compiled into this build at all?", which no other listing answers.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

#define USAGE "lsdrv [-a] [-v]"

int main(int argc, char **argv) {
    int verbose = 0, all = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-a")) all = 1;
        else { cmd_usage(USAGE); return 1; }
    }

    printf("%-14s %-8s %s\n", "DRIVER", "CLASS", verbose ? "SOURCE" : "DEVICES");

    // `bound` counts what was PRINTED and `known` every driver there is,
    // so the footer below can tell "this machine drives nothing" from
    // "this kernel reports no drivers" -- which are different faults.
    int bound = 0, known = 0;
    struct query_driver d;
    QUERY_FOREACH(QUERY_DRIVER, d, i) {
        known++;
        if (!all && !d.devices[0]) continue;   // bound nothing; -a shows it
        bound++;
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

    if (!known) {
        // Distinguished from "nothing is bound", below: a kernel with no
        // driver at all would not have booted, so this means the query
        // is missing rather than the machine being bare.
        printf("lsdrv: this kernel does not report its drivers\n");
        return 1;
    }
    if (!bound) {
        // NOT an error. A machine really can drive nothing this build
        // knows about, and saying so beats an empty table -- with the
        // flag that shows what was filtered out.
        printf("lsdrv: no driver is bound to anything -- "
               "`lsdrv -a` lists all %d in this build\n", known);
    }
    return 0;
}
