#ifndef AML_H
#define AML_H

#include <stdint.h>

// The ACPI namespace, as far as DECLARATIONS go -- see kernel/acpi/aml.c.
//
// THIS IS A WALK, NOT AN INTERPRETER, and the line is EXECUTION. Every
// Method body is skipped by its PkgLength; nothing here evaluates
// anything. `docs/aml-design.md` stages what this is for and carries the
// honest case against going further.
//
// WHY IT EXISTS: the GPE wake set is a `_PRW` object per device, and
// there is no other place a machine writes down which of its events may
// wake it. Three cheaper approximations were built and measured first;
// `docs/decisions.md` records why none of them worked.

#define AML_MAX_NODES 1024
#define AML_ROOT      0

enum aml_kind {
    AML_KIND_ROOT = 0,
    AML_KIND_SCOPE,
    AML_KIND_DEVICE,
    AML_KIND_NAME,       // a Name binding; `data` is its DataObject
    AML_KIND_METHOD,     // recorded, NEVER entered
    AML_KIND_POWER,
    AML_KIND_THERMAL,
    AML_KIND_PROCESSOR,
    AML_KIND_OTHER,      // an OperationRegion, Mutex, Event, Alias...
};

// A NameSeg is EXACTLY four characters and is not NUL-terminated -- AML
// pads short names with '_' rather than shortening them, so "PCI0" and
// "EC__" are both four. Anything printing one must bound it.
struct aml_node {
    char     name[4];
    uint16_t parent;     // index of the enclosing node; AML_ROOT for top level
    uint8_t  kind;       // enum aml_kind
    uint8_t  depth;
    const uint8_t *data; // the object's body, for the stages that decode it
    uint32_t len;        // bytes at `data`: a Name's DataObject, a Method's
                         // flags byte plus body, a container's term list
};

// The child of `parent` named `seg` (four characters), or -1.
int aml_child(int parent, const char *seg);

// Empties the namespace to just the root. Separate from aml_build()
// because that one PARSES THIS MACHINE'S FIRMWARE, so a test that wanted
// a clean slate and called it instead got QEMU's whole DSDT and then
// found the real `PCI0` rather than its own.
void aml_reset(void);

// Walks the DSDT and every SSDT into the namespace. Idempotent: a second
// call rebuilds from scratch. Returns the node count.
int aml_build(void);

int aml_node_count(void);
const struct aml_node *aml_node_at(int index);

// Nodes that did not fit AML_MAX_NODES, and terms the walk REFUSED to
// step over because it could not compute their length. Both are counted
// rather than logged per occurrence: a firmware table this parser does
// not fully understand is a NUMBER worth reporting, not a log flood.
int aml_dropped(void);
int aml_refused(void);

// One table's term list, exported for the KTESTs -- which is how this is
// tested against real firmware rather than against a fixture written
// beside it. `parent` is the node new top-level objects hang under.
int aml_parse_table(const uint8_t *aml, uint32_t len, uint16_t parent);

// Writes `node`'s full path (`\_SB.PCI0.EC__`) into `buf`. Returns the
// length written, or 0 when it does not fit.
int aml_path(int index, char *buf, uint32_t cap);

// Dumps the namespace to the kernel log -- the debug console's `aml`.
void aml_dump(void);

#endif
