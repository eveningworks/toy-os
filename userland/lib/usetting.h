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
// ENUMERATION GOES THROUGH usetting_dispatch(), which is the MERGED
// registry: the kernel's settings (SYS_SETTING) and the ones declared
// by schema files in /etc/settings.d (lib/usetting_schema.h), presented
// as one list in one message shape.
//
// It exists because the merge has to happen in exactly one place. With
// half the machine's settings owned by ring 3, a client calling
// sys_setting() directly sees only the kernel half -- and two clients
// each merging for themselves is the second source of truth this
// registry was built to remove. So System Settings and `config` call
// this instead, and neither had to change in any other way.
//
// SAME RETURN CONVENTION AS THE SYSCALL: 0 is success, non-zero is a
// bad op or an out-of-range index. That is what makes the substitution
// mechanical at ~30 call sites.

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

// Serves one setting_msg against the merged registry. `struct
// setting_msg` and `enum setting_op` are the ABI's (abi/setting_abi.h);
// everything a client could ask the kernel it may ask here.
//
// THE INDEX SPACE IS KERNEL FIRST, THEN SCHEMA. Registration order
// within the kernel half, directory order within the other -- so an
// index is usable as a row number for as long as nothing reloads,
// which is the contract the kernel's own index already carried.
int usetting_dispatch(struct setting_msg *m);

// WHERE A CATEGORY AND A PAGE SIT IN A SETTINGS SIDEBAR -- the `Order=`
// of `/etc/settings.d/category.<Category>` and of
// `group.<Category>.<Group>`. Lower first; 0 when no file says.
//
// **THE ORDER IS DATA BECAUSE IT WAS AN ACCIDENT BEFORE.** The sidebar
// was built in first-seen order, which was the kernel's boot sequence --
// so moving a setting between files, or out of the kernel entirely,
// silently rearranged a list people navigate by muscle memory. KDE and
// GNOME both give a panel an explicit weight for this reason; a
// registration order is not a design.
//
// It is answered HERE rather than by the kernel: these files are
// presentation for whatever draws the settings, and ring 0 has no stake
// in what order they appear. The page's LABEL still comes from
// SETTING_OP_GROUP_TEXT, which predates the split.
int usetting_category_order(const char *category);
int usetting_group_order(const char *category, const char *group);

#endif // ULIB_USETTING_H
