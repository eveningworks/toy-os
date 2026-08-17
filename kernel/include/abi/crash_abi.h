#ifndef ABI_CRASH_H
#define ABI_CRASH_H

#include <stdint.h>

// The kernel's deliberate-fault table, as ring 3 sees it -- what
// `SYS_CRASHTEST` carries.
//
// WHY THIS EXISTS: the panic path is the one path a kernel cannot
// exercise by accident and must not get wrong, and until this there was
// no way to reach it at all. Validating the panic report meant editing
// a debug command to dereference a bad pointer, rebuilding, and taking
// the change out again. Linux has the same thing for the same reason
// (`lkdtm`, and sysrq-c).
//
// The kernel owns the LIST, not just the trigger, so `what faults can
// this kernel produce?` has one answer -- the same reason the settings
// registry replaced a Control Panel that carried its own list. A caller
// enumerates rather than hardcoding, so a kind added in the kernel
// appears in every front end with no edit.
//
// Note the table covers RING-0 faults only. A ring-3 program does not
// need the kernel's help to dereference NULL; it needs help only to
// make the KERNEL fault, which is the whole point.

#define CRASH_NAME_MAX 24
#define CRASH_DESC_MAX 64

#define CRASH_OP_LIST    0 // fill in count, and the kind at `index`
#define CRASH_OP_TRIGGER 1 // fault, on purpose. May not return.

// Set in `flags` when the kernel is willing to fault on request. Off
// unless `faultinject` is on the GRUB command line -- a deliberate hole
// has no business being open on an ordinary boot. A caller should show
// the kinds either way and report the refusal, so the gate is
// discoverable rather than mysterious.
#define CRASH_F_ARMED 0x01

struct crash_msg {
    uint32_t op;     // CRASH_OP_*
    int32_t  index;  // in: which kind (LIST/TRIGGER)
    int32_t  count;  // out: how many kinds exist
    uint32_t flags;  // out: CRASH_F_*
    char name[CRASH_NAME_MAX];
    char desc[CRASH_DESC_MAX];
};

#endif
