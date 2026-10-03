#ifndef ULIB_USETTING_SCHEMA_H
#define ULIB_USETTING_SCHEMA_H

#include <stdint.h>
#include "setting_abi.h"
#include "lib/usetting_text.h"

// A setting DECLARED by a file, and owned by ring 3.
//
// The kernel's registry (kernel/include/api/setting.h) holds settings
// the KERNEL applies -- the font size it rasterises with, the mouse
// speed it accelerates by, the disk knobs. A desktop setting is not one
// of those: the compositor is a process, it reads /etc and applies the
// value itself, and the registry's entire job was to validate a string
// and write it down. Twelve C files in ring 0 existed to say what those
// strings were.
//
// So they say it here instead, as data. A file in /etc/settings.d that
// carries `Type=` DECLARES a setting; one without it only DESCRIBES a
// setting the kernel registered, which is what that directory already
// did (api/setting_text.h). One directory, two roles, told apart by one
// key -- rather than a second directory whose only difference is who
// reads it.
//
// This is GSettings' arrangement: a schema is an installed FILE, the
// store is somewhere else, and no code is written per key. The kernel
// keeps `sysctl` -- knobs it can actually turn.
//
// WHAT IS NOT HERE: an apply. Nothing in this library takes effect. It
// validates a value and writes it to the setting's /etc file; the
// program that owns the setting notices on its generation poll, exactly
// as it did when the kernel wrote the same bytes. That is why the whole
// conversion changes no behaviour.
//
//   /etc/settings.d/desktop.icon_size
//     Type=enum
//     File=/etc/desktop.conf
//     Label=Size
//     Category=Desktop
//     Group=Icons
//     Default=medium
//     Choices=small,medium,large
//     Description=How big the desktop icons are drawn
//     Choice.small=Small
//
// THE FILE NAME IS THE IDENTITY. `<namespace>.<name>`, as the text
// files already are -- so a lookup by qualified name opens one path and
// never walks the directory, and two programs may each own a `theme`.
// The namespace must match what `File=` is registered as in the config
// file registry (`desktop` for /etc/desktop.conf); nothing enforces the
// pair, and a mismatch shows up as a setting filed under the wrong
// namespace rather than as an error.

// A declaration, parsed. One of these on the stack is ~250 bytes, which
// is why nothing here caches a table of them: a GET or a SET opens one
// file, and only an enumerating client (System Settings, `config`) pays
// for the walk.
struct uschema {
    char ns[SETTING_ABI_NS_MAX];
    char name[SETTING_ABI_NAME_MAX];
    char file[SETTING_ABI_FILE_MAX];
    char label[SETTING_ABI_LABEL_MAX];
    char category[SETTING_ABI_CATEGORY_MAX];
    char group[SETTING_ABI_CATEGORY_MAX];
    char def[SETTING_ABI_VALUE_MAX];   // Default=, when the key is absent
    char unit[SETTING_ABI_UNIT_MAX];
    // The literal options, comma-separated as written. They come FIRST
    // in the choice list, so `Choices=none` beside a ChoiceDir is how
    // the wallpaper offers "none" ahead of the pictures.
    char choices[SETTING_ABI_VALUE_MAX * 4];
    char choice_dir[SETTING_ABI_FILE_MAX];
    uint32_t type;      // SETTING_ABI_TYPE_*
    uint8_t  dir_mode;  // USCHEMA_DIR_*
    int32_t  min, max, step;
    // Requires=<qualified name>=<value>: this setting only applies while
    // that other DECLARED setting has that value -- see uschema_unmet().
    // Otherwise= is the value it reads as meanwhile, or empty.
    char req_name[SETTING_ABI_NS_MAX + SETTING_ABI_NAME_MAX];
    char req_value[SETTING_ABI_VALUE_MAX];
    char otherwise[SETTING_ABI_VALUE_MAX];
};

// Is `s`'s Requires= unmet right now? Then 1, and `reason` (nullable)
// gets the RequiresReason= sentence, or one made from the other
// setting's label. ONLY ANOTHER DECLARED SETTING CAN BE REQUIRED: a
// kernel name is not found here and the requirement is ignored, so
// /etc still cannot disable a control the kernel owns.
int uschema_unmet(const struct uschema *s, char *reason, uint32_t cap);

// The value IN EFFECT: Otherwise= while the requirement is unmet, else
// what the file says, else the default. What GET and INFO answer; the
// file itself keeps the stored choice, so meeting the requirement again
// brings it back.
void uschema_effective(const struct uschema *s, char *out, uint32_t cap);

// How a ChoiceDir's entries become values.
//
//   NAME   every file, as named               -- the screensavers
//   STEM   every file, extension removed      -- the wallpapers
//   SUBDIR every DIRECTORY, as named          -- the cursor themes
//
// A mode rather than three keys, because they are exclusive: a
// directory is being read one way or another way, never two.
#define USCHEMA_DIR_NAME   0
#define USCHEMA_DIR_STEM   1
#define USCHEMA_DIR_SUBDIR 2

// The keys a declaring file adds to the ones in api/setting_text.h.
#define USCHEMA_KEY_TYPE        "Type"
#define USCHEMA_KEY_FILE        "File"
#define USCHEMA_KEY_LABEL       "Label"
#define USCHEMA_KEY_CATEGORY    "Category"
#define USCHEMA_KEY_GROUP       "Group"
#define USCHEMA_KEY_DEFAULT     "Default"
#define USCHEMA_KEY_CHOICES     "Choices"
#define USCHEMA_KEY_CHOICE_DIR  "ChoiceDir"
#define USCHEMA_KEY_DIR_MODE    "ChoiceDirMode"
#define USCHEMA_KEY_MIN         "Min"
#define USCHEMA_KEY_MAX         "Max"
#define USCHEMA_KEY_STEP        "Step"
#define USCHEMA_KEY_UNIT        "Unit"
#define USCHEMA_KEY_REQUIRES    "Requires"
#define USCHEMA_KEY_REQUIRES_REASON "RequiresReason"
#define USCHEMA_KEY_OTHERWISE   "Otherwise"

// Parses the declaration for `qualified` ("desktop.icon_size"). Returns
// 1, or 0 when there is no such file, when it carries no `Type=` (it is
// a text-only file describing a KERNEL setting), or when it is
// malformed. `out` is zeroed either way.
int uschema_find(const char *qualified, struct uschema *out);

// How many declarations /etc/settings.d holds, and the `index`th one.
//
// THE ORDER IS THE DIRECTORY'S, and it is stable only while the
// directory is: a client that reads INFO at index N and then asks for
// its choices must not have reloaded in between. That is the same
// contract the kernel registry's index already carries, where the order
// is registration rather than the filesystem.
int uschema_count(void);
int uschema_at(int index, struct uschema *out);

// Forgets the cached listing. Called by usetting_dispatch() on RELOAD;
// a file added to /etc/settings.d is otherwise invisible until then,
// which is the same rule `config reload` already sets for a hand edit.
void uschema_invalidate(void);

// The `index`th choice for `s`, written to `out`. Returns 1, or 0 once
// past the last one -- which is how a caller walks to the end without
// asking for a count first.
//
// The list is the literal `Choices=` first, then `ChoiceDir=`'s
// entries. Composing rather than choosing lets the wallpaper's "none"
// sit ahead of a directory it knows nothing about.
//
// THERE IS NO `ChoiceFile=` HERE, though `struct setting` has one. No
// declaration needs it -- the only setting whose options are a file is
// the timezone, and the kernel applies that one, so it stays
// registered. Adding it would also mean adding its CACHE: enumerating
// is O(choices) by construction, and the kernel's choice_file reads the
// whole document per row, which is the shape that made a nine-entry
// desktop reload cost 54 reads (docs/decisions.md, the timezone list).
// It comes back with its first real caller, cache included.
int uschema_choice(const struct uschema *s, int index, char *out, uint32_t cap);
int uschema_choice_count(const struct uschema *s);

// The current value: what the file says, or `Default=` when the key is
// absent. Never fails -- an unreadable file reads as "at its default",
// which is what the machine will actually behave as.
void uschema_get(const struct uschema *s, char *out, uint32_t cap);

// What the FILE says, or "" when the key is absent. `stored` in the
// ABI: the same question `config diff` asks.
int uschema_stored(const struct uschema *s, char *out, uint32_t cap);

// 1 if `value` is one this setting may take. The rules are the kernel
// registry's, kept deliberately identical: an INT must parse and fall
// within min..max, an ENUM must be one of its choices -- UNLESS the
// choice list is empty, where anything goes, because an empty list means
// a directory that has nothing in it yet rather than a setting with no
// legal values. A KEYCOMBO must spell a combination.
int uschema_validate(const struct uschema *s, const char *value);

// Validates and writes. Returns `enum setting_result`: INVALID when the
// value is refused, SAVED when the file was written (or already said
// this), UNSAVED when the write failed.
//
// It does NOT bump the generation -- usetting_dispatch() does that, with
// SETTING_OP_TOUCH, because the counter lives in the kernel and one
// place should own the announcement. `changed` is how the caller knows
// whether to: SETTING IT TO WHAT IT ALREADY IS SUCCEEDS AND CHANGES
// NOTHING, and announcing that wakes every consumer on the machine for
// no reason -- a UI applying on pointer motion then freezes it, which
// is measured rather than theoretical (api/setting.h's setting_set()).
// NULL when the caller does not care.
int uschema_write(const struct uschema *s, const char *value, int *changed);

// The DISPLAY name for `value` -- `Choice.<value>=` in the same file,
// which is the key api/setting_text.h already defines for a kernel
// setting. Falls back to the value itself, so a caller may always draw
// what this writes and never has to decide.
void uschema_choice_label(const struct uschema *s, const char *value,
                          char *out, uint32_t cap);

// Fills a reply's presentation fields -- description, widget, sflags,
// order -- from /etc/settings.d.
//
// **BY (ns, name), BECAUSE IT SERVES BOTH HALVES NOW.** The kernel read
// these files itself to answer for its own settings; it does not any
// more (lib/usetting_text.h), so this is the one reader for the
// directory. A setting the kernel registered and one declared by a file
// get their description and their widget from exactly the same code.
struct setting_msg;
void uschema_text_for(const char *ns, const char *name, struct setting_msg *m);
// One presentation word from <ns>.<name>'s text file -- a key no ABI
// field carries, read by the client that understands it (System
// Settings' `Preview=`). 1 with `out` filled, 0 when absent.
int uschema_text_word(const char *ns, const char *name, const char *key,
                      char *out, uint32_t cap);
void uschema_text(const struct uschema *s, struct setting_msg *m);

// The display name for one choice -- `Choice.<value>` -- and a page's
// own Label/Description from `group.<category>.<group>`. Both were the
// kernel's; both are here for the same reason.
//
// **IT WRITES ONLY WHEN THE FILE NAMES THE CHOICE, and returns whether
// it did.** /etc/settings.d is the most specific of three sources: an
// installation's rename wins over a label the setting COMPUTED (the
// timezone database's "Los Angeles") which wins over the raw value. A
// version of this that wrote the value as its own fallback would erase
// the computed name on every choice the file says nothing about.
int uschema_choice_label_for(const char *ns, const char *name,
                             const char *value, char *out, uint32_t cap);
int uschema_group_text(const char *category, const char *group,
                       struct setting_msg *m);

// Removes the key, so the setting falls back to `Default=` at the next
// read. SETTING_OP_UNSET's half.
int uschema_unset(const struct uschema *s);

#endif // ULIB_USETTING_SCHEMA_H
