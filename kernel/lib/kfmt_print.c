// The two kernel SINKS for kfmt's formatter: vga_printf() and
// klog_printf(). Declared in kfmt.h alongside k_snprintf(), because
// from a caller's point of view they are one facility -- the split is
// about what each half is allowed to include, not about what they are.
//
// WHY THIS IS A SEPARATE FILE. kfmt.c is compiled twice: once into the
// kernel and once into build/userland/shared/ for libuapp.a, so ring-3
// programs get the same formatter instead of hand-rolling a digit loop
// (which two of them had done -- see userland/lib/string.h). The
// shared-source rule in the Makefile requires such a file to be
// freestanding, and these two functions are not: they need vga.h and
// klog.h. Moving them out is what made kfmt.c shareable at all.
//
// So the rule for adding to either file: a new CONVERSION goes in
// kfmt.c, a new SINK goes here. One kernel include in kfmt.c silently
// takes the formatter away from userland again.
#include "kfmt.h"
#include "vga.h"
#include "klog.h"

void vga_printf(const char *fmt, ...) {
    char line[KFMT_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    vga_write(line);
}

void klog_printf(const char *fmt, ...) {
    char line[KFMT_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    klog_write(line);
}
