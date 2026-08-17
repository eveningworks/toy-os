#ifndef SETTING_H
#define SETTING_H

#include <stdint.h>
#include "etc_config.h"

// The settings registry: one place that knows what settings exist.
//
// A subsystem that owns a persistent setting REGISTERS it here at boot,
// the same way a graphics card registers a `display_driver` and a disk
// registers a `block_device` -- the subsystem announces itself instead
// of a central table listing it. What registration adds over the
// hand-rolled `*_init()`/`*_save()` pairs this replaced is the sentence
// none of them held: "a setting called `font_size` exists, it is one of
// these named choices, and here is how to apply one".
//
// THE INVARIANT: a setting's value is validated, applied and persisted
// by ONE function (`apply`), so "it applied" and "it reached the disk"
// cannot drift apart -- that is what `enum setting_result` (etc_config.h)
// exists to say, and it is now said in one place per setting rather than
// re-decided by each caller.
//
// WHY IT MATTERS BEYOND TIDINESS: nothing could previously answer "what
// settings exist?", so a Control Panel had to carry its own list of them
// -- a second source of truth that drifts the moment a subsystem adds a
// key. With a registry, `SYS_SETTING` (abi/setting_abi.h) hands ring 3
// the whole list, and the ring-3 Control Panel is GENERATED from it: a
// setting registered anywhere in the kernel gains a Control Panel row
// with no edit to Control Panel. Same move the Start menu already made
// when it started reading `.desktop` files instead of a C table.
//
// THE TRAP: `struct setting` is stored by POINTER, not copied, so
// anything registered must have static storage duration. Registering a
// stack local leaves the registry holding a dangling pointer that reads
// as plausible garbage rather than crashing.

#define SETTING_MAX        16 // registered settings; raise freely
#define SETTING_NAME_MAX   24 // the /etc key, e.g. "font_size"
#define SETTING_LABEL_MAX  40 // what a settings UI shows, e.g. "Font size"
#define SETTING_VALUE_MAX  64 // a value, as written to its file

enum setting_type {
    SETTING_TYPE_ENUM   = 0, // one of `choice`'s named options
    SETTING_TYPE_STRING = 1, // free text; no choice enumerator
};

struct setting {
    const char *name;  // the key in `file`; also how ring 3 names it
    const char *label; // human-facing; a UI shows this, never `name`
    enum setting_type type;
    const char *file;  // which /etc file it persists to

    // ENUM only: writes choice `index` into `out`, returning 1, or
    // returns 0 once `index` is past the last one. A callback rather
    // than an array so a choice list can be COMPUTED -- the keyboard
    // layouts are files in a directory, not a compiled-in enum.
    int (*choice)(int index, char *out, uint32_t out_size);

    // The current value, as the string that would be written to `file`.
    // Reads the subsystem's live state, not the file: the two agree
    // only until someone edits the file by hand, and the live one is
    // the truth a UI should show.
    void (*get)(char *out, uint32_t out_size);

    // Validate, apply live, and persist -- in that order, and all three
    // or none. Returns `enum setting_result`. NULL means PERSISTED-ONLY:
    // the registry writes `file` itself and nothing is applied, which is
    // what a setting owned by a process outside the kernel needs (the
    // desktop's icon positions today; the ring-3 window manager after
    // Milestone 41's stage 4).
    int (*apply)(const char *value);
};

// Adds `s` to the registry. Returns 1, or 0 if the registry is full, if
// `s` is malformed (no name, no label, no `get`, or an ENUM with no
// `choice`), or if that name is already registered -- a duplicate is
// refused rather than shadowing, since which one won would depend on
// boot order.
int setting_register(const struct setting *s);

// How many settings are registered, and the one at `index` (NULL if out
// of range). Registration order is stable, so an index is usable as a
// row number for as long as a caller holds it.
// Removes a setting by name. Returns 1 if it was there. Exists so a
// KTEST can register a scratch setting, exercise the registry against
// it and take it away again -- without it, a `ktest` run would leave a
// fake row in every settings UI for the rest of the boot. Nothing in
// the boot path calls it: a subsystem that registers does so once.
int setting_unregister(const char *name);

int setting_count(void);
const struct setting *setting_at(int index);
const struct setting *setting_find(const char *name);

// Writes the named setting's current value into `out`. Returns 1, or 0
// (leaving `out` empty) if no such setting is registered.
int setting_get(const char *name, char *out, uint32_t out_size);

// Validates, applies and persists. Returns SETTING_INVALID for an
// unregistered name or a value the owner rejects, SETTING_SAVED when it
// applied and reached the disk, SETTING_UNSAVED when it applied but the
// write failed -- which is a real outcome on a filesystem with no /etc,
// and reporting it as success is how a setting silently fails to
// survive a reboot.
enum setting_result setting_set(const char *name, const char *value);

// Bumped on every SET that applied. A process holding cached settings
// polls this to learn it needs to re-read them -- one integer compare,
// no I/O, the same trick `fs_generation()` uses for the desktop's live
// `.desktop` reload. It is deliberately not a callback list: the
// interested parties live in other address spaces.
uint32_t setting_generation(void);

// Called from kernel_main() after the filesystem is up and each
// subsystem's own `*_init()` has run. Registers the settings the kernel
// itself owns, each through its subsystem's `*_setting_register()`.
void settings_init(void);

// Re-reads every registered setting from its file and re-applies it.
// Returns how many values were REJECTED by their owner (a typo in a
// hand-edited file), leaving the previous working value in place for
// each. Bumps the generation unconditionally.
//
// This is the price of settings being ordinary text files: /etc is
// editable in `edit`, which is the point, but a hand edit and the
// subsystem's live copy disagree until something re-reads. `settings
// reload` is the shell's handle on it.
int settings_reload(void);

// Serves one SYS_SETTING message (abi/setting_abi.h) against the
// registry. Returns 1, or 0 for a bad op or an out-of-range index --
// note that a REFUSED VALUE is not a failure here: the syscall
// succeeded in asking, and the three-way outcome lands in msg->result.
// Split out from the syscall handler so a KTEST can drive the exact
// path ring 3 drives, with no process to spawn.
struct setting_msg;
int setting_dispatch(struct setting_msg *msg);

#endif
