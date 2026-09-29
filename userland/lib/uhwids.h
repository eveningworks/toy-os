#ifndef ULIB_UHWIDS_H
#define ULIB_UHWIDS_H

#include <stdint.h>

// Names for hardware ids, from the pci.ids / usb.ids databases -- the
// files a Linux distribution ships as hwdata, refreshed by /bin/hwdata.
// One streaming parser for lspci, lsusb and the Device Manager.
//
// STREAMED, NEVER HELD: pci.ids is ~1.6 MB and usb.ids ~730 KB, so the
// file is read in chunks, one line at a time, and only the names of the
// entries asked about are copied out. The scan stops as soon as every
// entry has every name it can have -- an id the database lacks keeps it
// reading to the end, which is correct and merely slower.
//
// A MISSING FILE IS NOT AN ERROR for a caller: the names stay "" and
// the numeric ids still print, which is why the two are separable.

#define UHWIDS_PCI "/usr/share/hwdata/pci.ids"
#define UHWIDS_USB "/usr/share/hwdata/usb.ids"

struct uhwids_entry {
    // In.
    uint16_t vendor, device;
    // The class, subclass and programming interface to name from the
    // file's class section ("C 04", the "\t03" under it, the "\t\t00"
    // under that) -- pci.ids and usb.ids share the shape, and USB's third
    // level is the interface protocol. -1 for none.
    int cls, subclass;
    // Out: "" where the database has nothing.
    char vendor_name[64];
    char device_name[96];
    char class_name[48];     // the class's name
    char subclass_name[48];  // the subclass's, the more specific
    // After the rest, so a caller that sets nothing here still means 0
    // -- set it to -1 for no third level.
    int prog_if;
    char progif_name[48];    // the prog-if's (PCI) or protocol's (USB)
};

// Fills every entry's names from `path`. 0, or -1 when the file cannot
// be opened (every name then "").
int uhwids_resolve(const char *path, struct uhwids_entry *e, int n);

#endif
