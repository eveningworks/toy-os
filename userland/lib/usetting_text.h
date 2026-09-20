#ifndef ULIB_USETTING_TEXT_H
#define ULIB_USETTING_TEXT_H

#include <stdint.h>

// The human-facing text for a setting: its description, and a display
// name per choice. Read from /etc/settings.d, one file per setting.
//
// **THE KEYS ONLY -- THE READER IS lib/usetting_schema.h.** This was
// api/setting_text.h, and ring 0 read these files to fill a reply's
// description, widget and choice labels. It does not any more: the
// kernel's registry says what a setting IS, and everything about how it
// READS is ring 3's, which is where the settings UI lives and where the
// declared settings' text was already being parsed. One parser for the
// directory instead of two that could disagree.
//
// WHY A FILE AND NOT A STRING IN struct setting. `label` is compiled in
// because a setting without one cannot be presented at all; prose is
// different -- it is the part somebody rewords, and eventually
// translates, and neither should need a kernel rebuild. It is also the
// part that can be MISSING without breaking anything, which is what
// makes a file safe here.
//
// THE FORMAT IS etc_config.c's: `name=value` lines with `#` comments.
// These files use NO section, even though the parser has them now -- one
// file per setting is what removes the need, and it is the convention
// this OS already teaches with /etc/services.d, /etc/config.d and
// /usr/wm/applications.
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

#endif
