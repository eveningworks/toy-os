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
// -- returned by the `*_config_save()`s and setting_set() kernel-side,
// and reported in `struct setting_msg`'s `result` to ring 3.
//
// It exists because those savers used to conflate "applied" with
// "saved": they reported success for a valid value whether or not the
// write landed, so setting a timezone on a filesystem with no /etc
// said it had and persisted nothing. Applying and persisting are two
// outcomes, and a
// caller that reports one as the other is lying to the user -- a
// setting silently not surviving a reboot is close to the worst way to
// find that out.
//
// It lives in the ABI rather than in api/etc_config.h (which includes
// this header) because ring 3 can change a setting now, so the outcome
// is part of the kernel<->userland contract.
//
// SETTING_UNSAVED is deliberately non-zero, so an `if (!save(...))`
// caller still reads as "did it apply?" and only callers that want the
// finer answer have to look for it.
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
#define SETTING_ABI_UNIT_MAX 8 // "%", "px", "ms" -- see struct setting_msg
#define SETTING_ABI_CATEGORY_MAX 24 // a UI section name, e.g. "Appearance"
#define SETTING_ABI_DESC_MAX 120 // one line of explanation, not a paragraph

// `widget` above.
#define SETTING_ABI_WIDGET_AUTO     0
#define SETTING_ABI_WIDGET_RADIO    1
#define SETTING_ABI_WIDGET_DROPDOWN 2
// Discrete stops, one per choice. For an ORDERED enum -- off/low/medium/
// high -- where "more" and "less" is what the user means and a radio
// list says nothing about the order.
#define SETTING_ABI_WIDGET_SLIDER   3

// `sflags` above.
//
// REBOOT is the one that matters: system.default_target takes effect at
// the next boot, and nothing else in this ABI can say so -- `result`
// reports whether the value PERSISTED, which is a different question. A
// UI that reported "saved" for a setting the user can see did nothing is
// telling the same kind of lie SETTING_UNSAVED exists to prevent.
#define SETTING_ABI_SF_REBOOT   (1u << 0) // takes effect at the next boot
#define SETTING_ABI_SF_ADVANCED (1u << 1) // a UI may keep it behind a disclosure
#define SETTING_ABI_MAX       56

#define SETTING_ABI_NAME_MAX  24 // the /etc key, e.g. "font_size"
#define SETTING_ABI_LABEL_MAX 40 // human-facing, e.g. "Font size"
#define SETTING_ABI_VALUE_MAX 64 // a value, as stored
#define SETTING_ABI_FILE_MAX  40 // e.g. "/etc/toyos.conf"
#define SETTING_ABI_NS_MAX    24 // e.g. "system" -- see `ns` below
// Room for "<ns>.<name>", which is what GET and SET accept in `name`.
// INFO still writes the BARE key there and puts the namespace in `ns`.
#define SETTING_ABI_QUALIFIED_MAX (SETTING_ABI_NS_MAX + SETTING_ABI_NAME_MAX + 1)

// Mirrors enum setting_type (setting.h). ENUM means `choice_count`
// options are listed through SETTING_OP_CHOICE; STRING means free text
// and `choice_count` is 0.
#define SETTING_ABI_TYPE_ENUM   0
#define SETTING_ABI_TYPE_STRING 1
// A BOUNDED INTEGER. Its value still travels as text in `value` -- a
// number written out is a string, and making it an int field would give
// the message two ways to carry a value that could disagree. What the
// type adds is `imin`/`imax`/`istep` below, which is what lets a UI
// offer steppers and a slider instead of a free-text box, and what lets
// the REGISTRY reject an out-of-range value before any apply() sees it.
#define SETTING_ABI_TYPE_INT    2

enum setting_op {
    // No inputs. Fills `count` with the number of registered settings.
    SETTING_OP_COUNT  = 0,
    // In: `index`. Fills name/label/file/type/value, and `count` with
    // how many choices this setting has.
    SETTING_OP_INFO   = 1,
    // In: `index`, `choice`. Fills `value` with that choice's name, and
    // `label` with its DISPLAY name if a text file gives one ("Los
    // Angeles" for `losangeles`) -- otherwise `label` repeats `value`,
    // so a client can always draw `label` and never has to decide.
    //
    // The VALUE is what gets stored and what `config set` takes; the
    // display name is only ever shown. Keeping them apart is what lets
    // the UI read well without /etc gaining prettified tokens that a
    // later parser would have to accept.
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
    // In: `name`, as "<category>/<group>". Out: `label` and
    // `description` for that PAGE, from /etc/settings.d/group.<category>.<group>.
    // Fails when no such file exists, which is normal -- a page with no
    // text of its own is titled by its group key.
    //
    // Its own op rather than a field on INFO because the text belongs to
    // the GROUP, not to any setting in it: putting it on INFO would mean
    // every setting in a group carrying a copy, and four copies of one
    // string is four chances to disagree.
    SETTING_OP_GROUP_TEXT = 9,

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

    // INT only, out on INFO: the inclusive bounds and the amount one
    // press of a stepper moves by. Zero on every other type, so a client
    // that ignores them is unaffected.
    //
    // THE BOUNDS ARE THE REGISTRY'S, NOT THE UI's. A client may use them
    // to disable a stepper at the end of the range, but it must not be
    // the only thing enforcing them: `config set` and a hand-edited
    // /etc file reach the same setting without passing through any UI,
    // so setting_set() clamps-or-refuses on its own. A UI that also
    // knows the range is a courtesy, not the gate.
    int32_t  imin, imax, istep;
    // What the number MEANS, shown after it -- "%", "px", "ms". Empty
    // when a bare number says it. Not a format string: a client prints
    // the value and then this, and nothing here may reorder them.
    char     unit[SETTING_ABI_UNIT_MAX];
    uint32_t result;     // out: enum setting_result (SET)
    // Out on EVERY op, so a client can notice someone else changed a
    // setting -- including a hand edit to /etc followed by `settings
    // reload` -- with one integer compare and no re-read. Same trick as
    // fs_generation() and the desktop's live `.desktop` reload.
    uint32_t generation;

    char name[SETTING_ABI_QUALIFIED_MAX]; // in (GET/SET), out (INFO)

    // The setting's NAMESPACE -- the registered name of the file it
    // persists to ("system" for /etc/toyos.conf). A setting's identity
    // is (ns, name), so two programs may both own a `theme`; see
    // api/setting.h.
    //
    // Its own field rather than baked into `name` on purpose: a client
    // displays, groups and sorts by it (Control Panel puts a heading on
    // each namespace), and re-splitting a joined string to do that is a
    // parser every client would grow its own copy of. `name` stays the
    // bare key, which is also what keeps every existing client correct
    // without an edit.
    //
    // GET/SET accept either form in `name`: bare when unambiguous,
    // "ns.name" always. A bare name matching more than one setting is
    // REFUSED rather than resolved by order.
    char ns[SETTING_ABI_NS_MAX];       // out (INFO)
    char label[SETTING_ABI_LABEL_MAX]; // out (INFO)
    char file[SETTING_ABI_FILE_MAX];   // out (INFO)
    // The UI section this setting belongs under (api/setting.h's
    // `category`), or "General" when it declared none. Out on INFO.
    // A client GROUPS by it -- System Settings' sidebar is built from
    // exactly this, so a setting registered anywhere in the kernel
    // appears under a heading with no edit to the app.
    char category[SETTING_ABI_CATEGORY_MAX];
    // The PAGE within that category (api/setting.h's `group`), or empty
    // when the setting declared none -- in which case a UI gives it a
    // page of its own. Out on INFO.
    char group[SETTING_ABI_CATEGORY_MAX];

    // A one-line explanation of what this setting DOES, from
    // /etc/settings.d/<ns>.<name> (api/setting_text.h). Empty when no
    // text file describes it, which is a normal state -- the label is
    // the floor and a UI shows nothing extra. Out on INFO.
    char description[SETTING_ABI_DESC_MAX];

    // How a UI should PRESENT this setting -- SETTING_ABI_WIDGET_*, from
    // the text file's `Widget=` key. AUTO (the default, and what every
    // setting with no text file gets) leaves the choice to the client,
    // which picks by how many options there are.
    //
    // A HINT, never an instruction: a client that has no such control
    // falls back to whatever it does have, because the setting must
    // still be changeable. It lives in the text file rather than in
    // struct setting because it is presentation, and the kernel's
    // descriptor says what a setting IS.
    uint32_t widget;

    // SETTING_ABI_SF_* -- when the change takes effect, and whether a UI
    // should keep it out of the way. From the text file's Applies= and
    // Advanced= keys.
    uint32_t sflags;

    // Position within its page. Lower first; settings with the same
    // order keep registration order between them, so a file that sets
    // none is unaffected by one that does. Default 0.
    int32_t order;
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

    // WHY THIS SETTING CANNOT BE CHANGED on this machine, or empty when
    // it can. Out on INFO. From the registry's own `unavailable()`
    // (api/setting.h) -- so it describes the MACHINE, not the file, and
    // it is not something /etc/settings.d can invent.
    //
    // A CLIENT MUST SHOW THE SENTENCE, not merely act on it. The reason
    // this is a string rather than a flag is that a control which is
    // dead with no explanation is indistinguishable from a broken one;
    // greying one out is only an improvement when the user can see why.
    //
    // Advisory as a control state, authoritative as an outcome: a
    // client may disable its widget, but SET is refused by the registry
    // regardless (result SETTING_INVALID), exactly as `config set` and
    // a hand-edited /etc file are. Same split the INT bounds make.
    char unavailable[SETTING_ABI_DESC_MAX];
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
