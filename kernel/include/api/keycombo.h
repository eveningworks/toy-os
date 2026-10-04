#ifndef API_KEYCOMBO_H
#define API_KEYCOMBO_H

#include <stdint.h>
#include <stddef.h>

// A KEY COMBINATION AS TEXT, both ways: "Ctrl+Alt+T" <-> (key, mods).
//
// It is here rather than in the compositor because THREE places need the
// same answer and any disagreement between them is a shortcut that can
// be set and never fires: the registry validates what `config set`
// writes (kernel/lib/shortcuts_config.c), the compositor matches live
// keys against it (userland/wm/wm_shortcut.c), and System Settings shows
// and captures it (userland/ui/uui_keycapture.c). One parser, one
// speller, one set of KTESTs.
//
// **THE SPELLING IS MODIFIERS THEN KEY, JOINED BY `+`, IN A FIXED
// ORDER** -- Ctrl, Alt, Shift, Super, then the key: "Ctrl+Alt+T",
// "Shift+Super+S". Writing them in another order PARSES (a person typing
// a shortcut should not have to know the canonical order), but
// keycombo_format() always emits this one, so a value that round-trips
// through the registry settles on a single spelling and two settings
// holding the same combination compare equal as strings. GNOME's
// `<Control><Alt>t` and KDE's `Ctrl+Alt+T` both normalise the same way;
// this follows KDE, because it is what a person reads on a menu.
//
// Matching is CASE-INSENSITIVE for the names, so "ctrl+alt+t" is
// accepted. The KEY is upper-cased when formatted, which is how every
// menu in this system already prints an accelerator.
//
// **WHAT A COMBINATION MAY NOT BE.** A bare modifier ("Ctrl") is
// refused: it would fire while someone was reaching for a real
// combination, and the one gesture of that shape this desktop has --
// Super alone -- is the compositor's own policy, deliberately not
// something this grammar can express. An empty string is not an error
// anywhere; it means UNBOUND, which is how a shortcut is switched off.

// Modifier bits are api/keyboard.h's KEY_MOD_*; the key is one of its
// KEY_* codes or a plain character.
struct keycombo {
    uint16_t key;  // 0 = unbound; a KEY_* special is above 0xFF
    uint8_t mods;  // KEY_MOD_*
};

#define KEYCOMBO_TEXT_MAX 32  // "Ctrl+Alt+Shift+Super+Print Screen" and room

// Parse `text` into `out`. Returns 1 on success, 0 when the text names
// no key this keyboard produces or is a bare modifier.
//
// An EMPTY or whitespace-only string succeeds with `out->key == 0`:
// unbound is a legal value, not a parse failure, and a caller that
// wants to refuse it tests the key.
int keycombo_parse(const char *text, struct keycombo *out);

// Spell `c` into `out` in the canonical order above. Returns 1, or 0
// when `out` is too small -- in which case NOTHING is written, the
// formatter rule this project already applies everywhere (CLAUDE.md).
// An unbound combination formats as the empty string.
int keycombo_format(const struct keycombo *c, char *out, size_t cap);

// Does a live key event match `c`? `key`/`mods` are what the compositor
// was handed.
//
// **IT COMPARES THE MODIFIERS EXACTLY**, so Ctrl+Alt+T does not fire on
// Ctrl+Alt+Shift+T. A subset test would make every binding shadow the
// ones above it, which is the bug KDE fixed by exact-matching too.
//
// THE ONE ASYMMETRY IS CTRL, and it is the terminal encoding's doing:
// by the time a letter reaches anyone, Ctrl+T is already 0x14 and the
// letter is gone (api/keyboard.h says so in as many words). So a
// combination naming Ctrl matches on the CONTROL CODE, and the caller
// does not have to know that happened.
int keycombo_matches(const struct keycombo *c, int key, uint8_t mods);

#endif
