#ifndef SETTING_TEXT_H
#define SETTING_TEXT_H

#include <stdint.h>

// The human-facing text for a setting: its description, and a display
// name per choice. Read from /etc/settings.d, one file per setting.
//
// WHY A FILE AND NOT A STRING IN struct setting. `label` is compiled in
// because a setting without one cannot be presented at all; prose is
// different -- it is the part somebody rewords, and eventually
// translates, and neither should need a kernel rebuild. It is also the
// part that can be MISSING without breaking anything, which is what
// makes a file safe here.
//
// THE FORMAT IS etc_config.c's, unchanged: `name=value` lines with `#`
// comments. No sections, no new parser -- one file per setting is what
// removes the need for them, and it is the convention this OS already
// teaches with /etc/services.d, /etc/config.d and /usr/wm/desktop.
//
//   /etc/settings.d/system.mouse_speed
//     Description=How far the pointer moves for a given hand movement
//     Choice.slow=Slow
//     Choice.normal=Normal
//     Choice.veryfast=Very fast
//
// The file is named `<namespace>.<name>` -- a setting's identity is the
// pair, so the file name is the qualified name and two programs owning
// a `theme` cannot collide.
//
// THE LABEL IS THE FLOOR. A missing or malformed file costs that ONE
// setting its extra text and nothing else: no description is shown and
// choices display as their raw values. Same containment as a cursor
// theme that fails to load, and the same consequence -- these files can
// be added one at a time, and a machine with an empty /etc/settings.d
// still has a completely usable settings UI.

#define SETTING_TEXT_DIR "/etc/settings.d"

// The keys a text file may carry:
//   Description=<one line>
//   Widget=auto | radio | dropdown | slider
//   Applies=now | reboot
//   Advanced=1 | 0
//   Order=<integer, lower first>
//   Choice.<value>=<display name>
//
// A PAGE (a category's group) has its own file, named
// `group.<category>.<group>`, carrying `Label` and `Description`. A
// second file shape in the same directory, because the text belongs to
// the group rather than to any setting in it -- putting it on each
// setting would mean four copies of one string.
#define SETTING_TEXT_KEY_DESC   "Description"
#define SETTING_TEXT_KEY_WIDGET "Widget"
#define SETTING_TEXT_KEY_APPLIES  "Applies"
#define SETTING_TEXT_KEY_ADVANCED "Advanced"
#define SETTING_TEXT_KEY_ORDER    "Order"
#define SETTING_TEXT_KEY_LABEL    "Label"
#define SETTING_TEXT_CHOICE_PREFIX "Choice."
#define SETTING_TEXT_GROUP_PREFIX  "group."

// Fills `out` with the description for `ns`.`name`, or leaves it empty
// when there is no text for it. Returns 1 if something was found.
int setting_text_description(const char *ns, const char *name,
                             char *out, uint32_t out_size);

// Fills `out` with the display name for the choice `value` of
// `ns`.`name`. Falls back to `value` itself, so a caller may always
// draw what this returns. Returns 1 if a display name was found.
int setting_text_choice(const char *ns, const char *name, const char *value,
                        char *out, uint32_t out_size);

// The presentation hint (SETTING_ABI_WIDGET_*), or AUTO when the file
// says nothing -- which is also what an unrecognised name gives, since a
// UI that cannot draw what was asked for must still draw something.
uint32_t setting_text_widget(const char *ns, const char *name);

// SETTING_ABI_SF_* for this setting, or 0 when the file says nothing.
uint32_t setting_text_sflags(const char *ns, const char *name);

// Position within its page; 0 (the default) leaves registration order.
int setting_text_order(const char *ns, const char *name);

// A PAGE's own label and description, from
// /etc/settings.d/group.<category>.<group>. Either output may be left
// empty. Returns 1 if the file existed.
int setting_text_group(const char *category, const char *group,
                       char *out_label, uint32_t label_size,
                       char *out_desc, uint32_t desc_size);

#endif
