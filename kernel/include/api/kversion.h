#ifndef KVERSION_H
#define KVERSION_H

// What kernel is running, from the kernel itself.
//
// The version macros exist in version.h and every program that includes
// them gets its OWN build's answer -- correct while kernel and userland
// ship as one image, wrong the moment a machine is updated a piece at a
// time. This is the kernel's own copy, reachable from ring 3 through
// QUERY_VERSION.
//
// **NOTHING HERE INCLUDES build_stamp.h.** That header changes on every
// build and is included by kversion.c alone, which is what keeps a
// timestamp from rebuilding the tree (tools/gen_version.sh).

// "toy-os 0.3.0-dev (426601f) built 2026-09-01 10:39:12" -- one line,
// for the boot log and a panic. Linux calls this linux_banner.
const char *kversion_banner(void);

void kversion_query_init(void); // QUERY_VERSION

#endif // KVERSION_H
