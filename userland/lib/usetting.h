#ifndef ULIB_USETTING_H
#define ULIB_USETTING_H

#include <stddef.h>
#include <stdint.h>
#include "setting_abi.h"

// One setting, by qualified name, for a ring-3 program: the GET and SET
// halves of `SYS_SETTING` with the message built here rather than in
// every caller.
//
// Seven programs carried their own copy of the same twelve lines -- a
// zeroed `struct setting_msg`, an op, two string copies, a syscall --
// and they disagreed about what a successful SET was: one demanded
// SETTING_SAVED, the rest accepted anything but SETTING_INVALID. Both
// are legitimate (a slider wants the LIVE change, a preference wants
// the persisted one), so usetting_set() returns the registry's own
// three-way answer and the caller states which it needs.
//
// Enumeration (COUNT / INFO / CHOICE) is not here: System Settings and
// `config` walk the registry by index and want the whole message.

// Reads `name` into `out`. Returns 1 on success, 0 if the setting does
// not exist or the syscall failed; `out` is "" then.
int usetting_get(const char *name, char *out, size_t cap);

// Reads `name` as a non-negative decimal. Returns 1 and sets *out, or
// returns 0 (missing, unreadable, or not a number) and leaves *out.
int usetting_get_int(const char *name, int *out);

// Writes `value` to `name`. Returns the registry's `enum setting_result`
// (SETTING_INVALID / SETTING_SAVED / SETTING_UNSAVED), or -1 when the
// syscall itself failed -- an unknown name, or no registry.
//
// `> 0` means the change is LIVE; `== SETTING_SAVED` means it also
// reached /etc. A caller that reports success must pick one, since
// SETTING_UNSAVED is "applied, and gone at the next boot".
int usetting_set(const char *name, const char *value);

// usetting_set() with a decimal.
int usetting_set_int(const char *name, int value);

// The INFO record for `name` -- the one op that carries the range, the
// unit and the `unavailable` sentence beside the value. Walks the
// registry by index (INFO is addressed that way) and returns that
// index, which is what SETTING_OP_CHOICE then takes; -1 when no setting
// has that qualified name. `out` is left zeroed on failure.
int usetting_find(const char *name, struct setting_msg *out);

#endif // ULIB_USETTING_H
