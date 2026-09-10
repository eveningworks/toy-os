#ifndef AML_DATA_H
#define AML_DATA_H

#include <stdint.h>

// AML STAGE 2: constant DataObjects and NameStrings, DECODED -- integers,
// strings, buffers, packages, and a name resolved against the namespace
// aml.c built. Nothing here evaluates: a value that is an expression is
// reported as such (0 / -1) and never guessed. See docs/aml-design.md.

struct aml_name {
    char seg[8][4];   // NameSegs, first to last
    int  nsegs;
    int  root;        // leading `\`
    int  ups;         // leading `^` count
};

int aml_is_name_lead(uint8_t c);

// Bytes consumed, or 0 when `p` is not a NameString that fits `avail`.
uint32_t aml_name_parse(const uint8_t *p, uint32_t avail, struct aml_name *out);

// Length of the constant DataObject (or NameString) at `p`, 0 otherwise.
uint32_t aml_object_len(const uint8_t *p, uint32_t avail);

// An integer constant: bytes consumed (0 when not one), value in *out.
uint32_t aml_int(const uint8_t *p, uint32_t avail, uint64_t *out);

// A Package: its element count and where the elements start. The
// elements follow one another; step with aml_object_len().
int aml_package(const uint8_t *p, uint32_t avail, uint32_t *count,
                const uint8_t **elems, uint32_t *elems_avail);

// A Buffer whose size is a constant: the bytes and how many.
int aml_buffer(const uint8_t *p, uint32_t avail, const uint8_t **bytes, uint32_t *n);

// Resolves `name` from node `scope` by ACPI's rules: absolute or
// parent-relative by its prefixes, and a single bare segment searched in
// the scope and each enclosing one. Node index, or -1.
int aml_resolve(int scope, const struct aml_name *name);

// The interrupt a resource buffer names -- an IRQ descriptor (ISA-style
// mask) or an Extended Interrupt descriptor (a GSI with its trigger and
// polarity). 1 when found.
int aml_crs_irq(const uint8_t *buf, uint32_t n, uint32_t *irq, int *level, int *low);

#endif
