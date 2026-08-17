#ifndef SETTING_ABI_H
#define SETTING_ABI_H

#include <stdint.h>

// What ring 3 sees of the settings registry (kernel/include/api/setting.h).
//
// The point of exposing the registry rather than a fixed list of known
// settings: a client can ASK what settings exist, what each one is
// called, which values are legal and where it is stored. That is what
// lets the ring-3 Control Panel be GENERATED -- it carries no list of
// its own, so a setting registered anywhere in the kernel gains a row
// with no edit to Control Panel, the same way a `.desktop` file gains a
// Start-menu entry.
//
// Everything travels as ONE fixed-layout struct with no pointers in it,
// the same shape `struct win_request_msg` uses and for the same reason:
// the kernel copies it in, fills it, copies it back, and nothing in it
// has to be validated as an address.
//
// `file` is carried deliberately. Settings persist as ordinary
// hand-editable text under /etc, which is the point of them being files
// at all -- but Unix's own weak spot is then "which file is this in?".
// The registry knows, so it says.

// What a "change a setting and persist it" call actually managed to do
// -- returned by tz_set_index(), the three `*_config_save()`s and
// setting_set() kernel-side, and reported in `struct setting_msg`'s
// `result` to ring 3.
//
// It exists because those savers used to conflate "applied" with
// "saved": three returned void and tz_set_index() returned 1 for a
// valid index whether or not the write landed, so `timezone Helsinki`
// on a filesystem with no /etc printed "Timezone set to helsinki." and
// persisted nothing. Applying and persisting are two outcomes, and a
// caller that reports one as the other is lying to the user -- a
// setting silently not surviving a reboot is close to the worst way to
// find that out.
//
// It lives in the ABI rather than in api/etc_config.h (which includes
// this header) because ring 3 can change a setting now, so the outcome
// is part of the kernel<->userland contract.
//
// SETTING_UNSAVED is deliberately non-zero, so the pre-existing
// `if (!tz_set_index(i))` idiom still reads as "did it apply?" and only
// callers that want the finer answer have to look for it.
enum setting_result {
    SETTING_INVALID = 0, // bad argument -- nothing applied, nothing written
    SETTING_SAVED   = 1, // applied, and written to its /etc file
    SETTING_UNSAVED = 2, // applied in memory, but the write FAILED
};

// How many settings the registry can hold, so a client can size a
// fixed array for the whole list -- there is no allocator in ring 3.
// Mirrors the kernel's SETTING_MAX (api/setting.h); a client should
// still read the real count from SETTING_OP_COUNT rather than assume
// this many exist.
#define SETTING_ABI_MAX       16

#define SETTING_ABI_NAME_MAX  24 // the /etc key, e.g. "font_size"
#define SETTING_ABI_LABEL_MAX 40 // human-facing, e.g. "Font size"
#define SETTING_ABI_VALUE_MAX 64 // a value, as stored
#define SETTING_ABI_FILE_MAX  40 // e.g. "/etc/toyos.conf"

// Mirrors enum setting_type (setting.h). ENUM means `choice_count`
// options are listed through SETTING_OP_CHOICE; STRING means free text
// and `choice_count` is 0.
#define SETTING_ABI_TYPE_ENUM   0
#define SETTING_ABI_TYPE_STRING 1

enum setting_op {
    // No inputs. Fills `count` with the number of registered settings.
    SETTING_OP_COUNT  = 0,
    // In: `index`. Fills name/label/file/type/value, and `count` with
    // how many choices this setting has.
    SETTING_OP_INFO   = 1,
    // In: `index`, `choice`. Fills `value` with that choice's name.
    // Fails once `choice` is past the last one, which is also how a
    // caller that did not read `count` can walk to the end.
    SETTING_OP_CHOICE = 2,
    // In: `name`. Fills `value` with the current value.
    SETTING_OP_GET    = 3,
    // In: `name`, `value`. Validates, applies and persists. `result`
    // gets the `enum setting_result` -- and SETTING_UNSAVED (2) is a
    // real outcome a caller must not report as success, since it means
    // the change is live but will not survive a reboot.
    SETTING_OP_SET    = 4,

    // --- the config-FILE registry (api/config_file.h) ---------------
    //
    // A separate index from the settings above, and deliberately so:
    // these are the /etc DOCUMENTS, including ones holding no
    // registered setting at all (the timezone database, the keymap
    // tables, the desktop's icon positions). It is what answers "what
    // configuration does this machine even have?" -- the question a
    // pile of files in /etc cannot answer for itself.

    // No inputs. Fills `count` with the number of registered files.
    SETTING_OP_FILE_COUNT = 5,
    // In: `index`. Fills name/file/label with the umbrella name, the
    // path and the one-line description. `type` is 1 for a built-in
    // (registered in kernel code) and 0 for one declared by a
    // descriptor in /etc/config.d.
    SETTING_OP_FILE_INFO  = 6,

    // No inputs. Re-reads every setting from its file, re-applies it,
    // and rescans /etc/config.d. Fills `count` with how many values
    // were REJECTED by their owner -- a typo in a hand-edited file,
    // which must be reported rather than look like the setting simply
    // not working. Unprivileged, like SYS_KILL and for the same
    // reason: there is no user model here to gate it on.
    SETTING_OP_RELOAD     = 7,

    // In: `name`. Removes the key from its file, so the setting falls
    // back to its built-in default at the next boot. `result` gets an
    // `enum setting_result`: SAVED if the key was there and the file
    // was rewritten, INVALID for an unregistered name or a key that was
    // not present.
    //
    // The LIVE value is deliberately not changed -- a built-in default
    // is what the subsystem starts with, and this kernel has no way to
    // ask a subsystem to return to one. Say so rather than implying a
    // revert that only happens at reboot.
    SETTING_OP_UNSET      = 8,
};

struct setting_msg {
    uint32_t op;         // in: enum setting_op
    int32_t  index;      // in: which setting (INFO, CHOICE)
    int32_t  choice;     // in: which option (CHOICE)

    uint32_t type;       // out: SETTING_ABI_TYPE_*
    int32_t  count;      // out: settings (COUNT) or choices (INFO)
    uint32_t result;     // out: enum setting_result (SET)
    // Out on EVERY op, so a client can notice someone else changed a
    // setting -- including a hand edit to /etc followed by `settings
    // reload` -- with one integer compare and no re-read. Same trick as
    // fs_generation() and the desktop's live `.desktop` reload.
    uint32_t generation;

    char name[SETTING_ABI_NAME_MAX];   // in (GET/SET), out (INFO)
    char label[SETTING_ABI_LABEL_MAX]; // out (INFO)
    char file[SETTING_ABI_FILE_MAX];   // out (INFO)
    char value[SETTING_ABI_VALUE_MAX]; // in (SET), out (INFO/CHOICE/GET)

    // What the FILE currently says, filled by INFO alongside the live
    // `value`. Empty when the key is absent (the setting is at its
    // built-in default).
    //
    // The two differ exactly when someone hand-edited /etc and nothing
    // has reloaded yet -- which, since these are plain text files by
    // design, is a normal state rather than an error. Reporting both is
    // what lets `config diff` answer "I edited the file, why is nothing
    // happening?" without a client parsing the file itself and growing
    // a second, divergent copy of the name=value parser.
    char stored[SETTING_ABI_VALUE_MAX];
};

// Whole-machine facts a System Info page wants that no other syscall
// reports. CPU identity is SYS_CPU_INFO, the PCI count is
// SYS_PCI_COUNT and uptime is SYS_MONOTONIC_NS -- all of which already
// existed, so none of them are repeated here. What was missing is only
// memory and disk.
#define SYS_INFO_DISK_VALID 1u // `disk_*` are meaningful (a mounted fs)

struct sys_info {
    uint64_t mem_free_kb;
    uint64_t mem_total_kb;
    uint64_t disk_used_bytes;
    uint64_t disk_total_bytes;
    uint32_t flags; // SYS_INFO_*
    uint32_t reserved;
};

#endif
