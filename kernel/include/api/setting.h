#ifndef SETTING_H
#define SETTING_H

#include <stdint.h>
#include "etc_config.h"
#include "config_file.h" // a setting's NAMESPACE is its file's name

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

// Raising this means raising SETTING_ABI_MAX with it: a ring-3 client
// has no allocator, so it sizes its list from that constant, and the
// two disagreeing means a client silently showing only part of the
// registry. The assert below is what stops that being silent.
// 28 since `audio_device` landed, and the way it announced itself is
// the reason this comment exists: the KTESTs register scratch settings
// ON TOP of whatever the kernel already has, so ONE new kernel setting
// reddened two tests about something else entirely
// (`setting_register(&g_scratch2)` refused, registry full). If this is
// hit again, raise it -- the cost is a few KB of bss in System
// Settings, which sizes its arrays from the ABI twin below. Raised
// to 32 when `brightness` made 25, to 40 when `scaling` made 29 (the
// KTESTs' four scratch settings had filled the rest), to 48 when the
// three network-time settings made 32.
#define SETTING_MAX        48 // registered settings
_Static_assert(SETTING_MAX == SETTING_ABI_MAX,
               "SETTING_MAX and SETTING_ABI_MAX must agree -- a client sizes "
               "its array from the ABI one and would truncate the list");
#define SETTING_NAME_MAX   24 // the /etc key, e.g. "font_size"

// What a setting with no `category` is filed under. Named rather than
// spelled in three places -- a UI, the ABI default and a test would
// otherwise each carry the string.
#define SETTING_CATEGORY_DEFAULT "General"

// A setting with no `group` gets a page to itself. The UI substitutes
// the setting's own label, so this is only ever seen if something
// registers with neither a group nor a label.
#define SETTING_GROUP_DEFAULT ""

// QUALIFIED NAMES: a setting's identity is (namespace, name), not name.
//
// The namespace is the registered NAME OF THE FILE it persists to
// (api/config_file.h) -- `system` for /etc/toyos.conf, `desktop` for
// /etc/desktop.conf -- so it is DERIVED rather than declared, and no
// existing `struct setting` had to change. Written `system.font_size`.
//
// The problem it fixes: identity used to be a bare global name, and
// registering a second `theme` was refused silently, first-wins. That
// was fine with one compiled-in table of five kernel settings and stops
// being fine the moment two programs own configuration -- the loser had
// no way to know, and `config get theme` would never have mentioned it.
//
// Every real system namespaces this way: sysctl puts it in the path
// (`net.ipv4.ip_forward`), GSettings uses a schema id plus a key, macOS
// `defaults` takes a domain and REQUIRES it for a write. A flat global
// name with silent first-wins was the outlier.
//
// THE RULE FOR LOOKUPS: a qualified name is always exact; a bare name
// works when exactly one setting has it, and is refused as AMBIGUOUS
// when several do -- never resolved by order, which would make the
// answer depend on boot sequence. setting_matches() is how a caller
// tells "no such setting" from "say which one".
#define SETTING_QUALIFIED_MAX (CONFIG_NAME_MAX + SETTING_NAME_MAX + 1)
_Static_assert(SETTING_QUALIFIED_MAX == SETTING_ABI_QUALIFIED_MAX,
               "a qualified name must fit the ABI message's `name` field, or "
               "a client's `config set system.font_size` arrives truncated");
#define SETTING_LABEL_MAX  40 // what a settings UI shows, e.g. "Font size"
#define SETTING_VALUE_MAX  64 // a value, as written to its file

enum setting_type {
    SETTING_TYPE_ENUM   = 0, // one of `choice`'s named options
    SETTING_TYPE_STRING = 1, // free text; no choice enumerator
    // A BOUNDED INTEGER: `min`..`max` inclusive, moved by `step`.
    //
    // It exists because the alternative was making every numeric knob an
    // ENUM of named levels, and `mouse_config.c` said so in as many
    // words: speed was `slow`/`normal`/`fast` "because the setting is an
    // ENUM in the registry, which is what gives it a choice list -- and
    // a choice list is what lets a UI present it at all without
    // inventing a slider widget. It also bounds the value: a
    // hand-edited 0 would freeze the pointer." Both of those are what
    // this type provides directly, so the workaround can go.
    //
    // The VALUE IS STILL A STRING everywhere -- `get` writes a number
    // out, `apply` parses one in, and the /etc file holds text as it
    // always did. Only the bounds are new. That is what keeps `config`,
    // etc_config.c and every existing caller untouched.
    SETTING_TYPE_INT    = 2,
};

struct setting {
    const char *name;  // the key in `file`; also how ring 3 names it
    const char *label; // human-facing; a UI shows this, never `name`
    enum setting_type type;
    const char *file;  // which /etc file it persists to

    // Which section of a settings UI this belongs under -- "Appearance",
    // "Input", "System". NULL means SETTING_CATEGORY_DEFAULT, so an
    // existing setting needs no edit and a new one may ignore this.
    //
    // Declared HERE rather than mapped by the app, for the reason
    // System Settings has no list of settings at all: a table in the app
    // is a second source of truth that drifts the moment a subsystem
    // adds a key. A setting registered anywhere in the kernel gets a
    // sidebar home the same way it already gets a row.
    //
    // It is a free STRING, not an enum, so a ring-3 program declaring
    // its own config file can name a section the kernel has never heard
    // of. The UI groups by exact match and puts anything unrecognised
    // under the default -- which is a real answer, not an error.
    const char *category;

    // Which PAGE within that category this setting appears on, so
    // several related settings share one page: cursor theme, size,
    // speed and acceleration all declare group "Mouse". NULL means the
    // setting gets a page of its own, named by its label -- which is
    // what every setting did before groups existed, so nothing had to
    // be edited to keep working.
    //
    // The sidebar is category -> group; the settings themselves are not
    // tree rows. That is KDE System Settings' shape: a leaf opens a
    // MODULE with several controls, not a single control.
    const char *group;

    // INT only: the inclusive range and the stepper increment. A UI
    // reads them to bound its controls; the REGISTRY enforces them, so a
    // value arriving from `config set` or a hand-edited file is refused
    // in exactly the same way as one from a spinbox. `step` of 0 is
    // read as 1.
    //
    // `unit` is what the number means -- "%", "px", "ms" -- shown after
    // it by a UI. NULL when a bare number says enough.
    int min, max, step;
    const char *unit;

    // ENUM only: writes choice `index` into `out`, returning 1, or
    // returns 0 once `index` is past the last one. A callback rather
    // than an array so a choice list can be COMPUTED -- the keyboard
    // layouts are files in a directory, not a compiled-in enum.
    int (*choice)(int index, char *out, uint32_t out_size);

    // OPTIONAL, ENUM only: the DISPLAY name for choice `index` --
    // "Los Angeles" where `choice` gives `losangeles`. NULL (the
    // default, and what most settings want) means the value is already
    // presentable, or that /etc/settings.d names it.
    //
    // WHY A CALLBACK WHEN /etc/settings.d ALREADY DOES THIS.
    // `Choice.<value>=<name>` covers a list the file's author can see:
    // three mouse speeds, two boot targets. It cannot cover a list that
    // is COMPUTED -- the timezones come from /etc/timezones, the
    // keyboard layouts from a directory -- because the file would have
    // to be regenerated whenever the data changed, which is a second
    // source of truth for the same strings. A setting whose choices are
    // data supplies their names from the same data.
    //
    // /etc/settings.d STILL WINS where it says something, so an
    // installation can rename or translate one choice without the
    // subsystem knowing. See setting_text.h.
    int (*choice_label)(int index, char *out, uint32_t out_size);

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

    // WHY THIS SETTING CANNOT BE CHANGED ON THIS MACHINE, or NULL when
    // it can. NULL is also the default, so no existing setting had to
    // be edited and most new ones may ignore this.
    //
    // A REASON RATHER THAN A BOOLEAN, deliberately. A control that is
    // simply dead tells the user nothing and reads as a bug; the whole
    // value of disabling one is the sentence next to it. GNOME and KDE
    // both show a lock or a hint beside a control policy has taken
    // away, and Windows' greyed-out settings without one are the
    // counter-example everybody has sworn at. So the registry carries
    // the sentence and every client shows it -- there is nowhere for a
    // UI to invent its own.
    //
    // The registry ENFORCES it: setting_set() refuses while this
    // returns non-NULL, so `config set` and a hand-edited /etc file are
    // refused exactly as the UI's control is. A disabled widget is a
    // courtesy, never the gate -- the same split the INT bounds above
    // already make.
    //
    // Returns a static string; it is copied at the ABI boundary, so
    // nothing here may hand back a caller's buffer.
    const char *(*unavailable)(void);
};

// The reason `s` cannot be changed right now, or NULL when it can.
// Every caller asks through this rather than testing the callback, so
// "no callback means available" is written down once.
const char *setting_unavailable(const struct setting *s);

// 1 if this setting has a file to persist to; 0 for a TUNABLE.
//
// A tunable is a setting whose `apply` writes a live kernel variable
// and which deliberately does NOT survive a reboot -- `heap_debug`,
// `ata_nodma`, `kstack_track`. It declares CONFIG_PATH_RUNTIME as its
// `file`, which gives it the "kernel" namespace (so it is
// `kernel.heap_debug`, the way sysctl spells `kernel.printk`) while
// telling every persistence site here that there is nothing to write.
//
// Persistence is OPTIONAL for a tunable, not forbidden -- see
// docs/settings-and-queries.md's vocabulary table. One that should
// survive a reboot simply names a real file instead, and needs nothing
// else; that is the sysctl.conf model, and it is why this is a
// predicate on the file rather than a flag on the setting.
//
// A tunable MUST have an `apply`: with no apply and no file, a value
// would be neither held nor stored. Registration refuses that.
int setting_persists(const struct setting *s);

// Registers the kernel's runtime tunables (kernel/lib/tunables.c).
// Called from settings_init() beside the persisted ones.
void tunables_register(void);

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
// Finds a setting by bare or qualified name. NULL if there is no such
// setting OR if a bare name is ambiguous -- ask setting_matches() which
// it was before reporting to a user.
const struct setting *setting_find(const char *name);

// How many settings a bare or qualified name matches: 0 (unknown), 1
// (usable), or more (ambiguous -- the caller must qualify).
int setting_matches(const char *name);

// The namespace for a setting: the registered name of the file it
// persists to, or "" if that file has no descriptor. Never NULL.
const char *setting_namespace(const struct setting *s);

// Writes "<namespace>.<name>" into `out` (just "<name>" when the
// setting has no namespace). Returns 1, or 0 if it would not fit.
int setting_qualified(const struct setting *s, char *out, uint32_t out_size);

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
