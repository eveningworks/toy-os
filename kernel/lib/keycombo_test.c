// KTESTs for the key-combination parser (api/keycombo.h).
//
// **THE ROUND TRIP IS THE PROPERTY**, not any single spelling: parse a
// text, format it back, and the result must parse to the same pair. That
// is what makes it safe for the registry to store whatever a person
// typed and for System Settings to show it -- three readers agreeing is
// the whole reason this code is shared.
#include "ktest.h"
#include "keycombo.h"
#include "keyboard.h"
#include "string.h"

static int parses_to(const char *text, uint16_t key, uint8_t mods) {
    struct keycombo c;
    if (!keycombo_parse(text, &c)) return 0;
    return c.key == key && c.mods == mods;
}

KTEST("keycombo", "the three bindings this exists for") {
    KTEST_ASSERT(parses_to("Super+E", 'E', KEY_MOD_SUPER));
    KTEST_ASSERT(parses_to("Ctrl+Alt+T", 'T', KEY_MOD_CTRL | KEY_MOD_ALT));
    KTEST_ASSERT(parses_to("Shift+Super+S", 'S', KEY_MOD_SHIFT | KEY_MOD_SUPER));
    KTEST_ASSERT(parses_to("Print Screen", KEY_PRINT_SCREEN, 0));
}

KTEST("keycombo", "order and case are free on the way IN") {
    // A person typing a shortcut should not have to know the canonical
    // order, and every one of these is the same combination.
    KTEST_ASSERT(parses_to("alt+ctrl+t", 'T', KEY_MOD_CTRL | KEY_MOD_ALT));
    KTEST_ASSERT(parses_to("CTRL+ALT+T", 'T', KEY_MOD_CTRL | KEY_MOD_ALT));
    KTEST_ASSERT(parses_to(" Ctrl + Alt + T ", 'T', KEY_MOD_CTRL | KEY_MOD_ALT));
    // Win/Meta/Cmd are accepted so a binding copied off another desktop
    // works -- and none of them is ever written back out.
    KTEST_ASSERT(parses_to("Win+E", 'E', KEY_MOD_SUPER));
    KTEST_ASSERT(parses_to("Meta+E", 'E', KEY_MOD_SUPER));
}

KTEST("keycombo", "and FIXED on the way out") {
    char buf[KEYCOMBO_TEXT_MAX];
    struct keycombo c;
    KTEST_ASSERT(keycombo_parse("alt+ctrl+t", &c));
    KTEST_ASSERT(keycombo_format(&c, buf, sizeof buf));
    KTEST_ASSERT(k_strcmp(buf, "Ctrl+Alt+T") == 0);

    KTEST_ASSERT(keycombo_parse("win+e", &c));
    KTEST_ASSERT(keycombo_format(&c, buf, sizeof buf));
    KTEST_ASSERT(k_strcmp(buf, "Super+E") == 0);   // never "Win+E" back

    KTEST_ASSERT(keycombo_parse("Print Screen", &c));
    KTEST_ASSERT(keycombo_format(&c, buf, sizeof buf));
    KTEST_ASSERT(k_strcmp(buf, "Print Screen") == 0);
}

KTEST("keycombo", "the round trip is stable for every spelling") {
    static const char *const TEXTS[] = {
        "Super+E", "Ctrl+Alt+T", "Shift+Super+S", "Print Screen",
        "F5", "Ctrl+Shift+F12", "Alt+Space", "Super+Up", "Ctrl+Page Down",
        "alt+ctrl+shift+super+q", "Esc", "Tab", "Enter",
    };
    for (unsigned i = 0; i < sizeof TEXTS / sizeof TEXTS[0]; i++) {
        struct keycombo a, b;
        char buf[KEYCOMBO_TEXT_MAX];
        KTEST_ASSERT(keycombo_parse(TEXTS[i], &a));
        KTEST_ASSERT(keycombo_format(&a, buf, sizeof buf));
        KTEST_ASSERT(keycombo_parse(buf, &b));
        KTEST_ASSERT(a.key == b.key && a.mods == b.mods);
    }
}

KTEST("keycombo", "unbound is a VALUE, not a parse failure") {
    struct keycombo c;
    char buf[KEYCOMBO_TEXT_MAX];
    KTEST_ASSERT(keycombo_parse("", &c) && c.key == 0);
    KTEST_ASSERT(keycombo_parse("   ", &c) && c.key == 0);
    // ...and it formats back to empty rather than to some placeholder.
    c.key = 0; c.mods = 0;
    KTEST_ASSERT(keycombo_format(&c, buf, sizeof buf) && buf[0] == '\0');
    // An unbound combination never matches anything.
    KTEST_ASSERT(!keycombo_matches(&c, 'e', KEY_MOD_SUPER));
}

KTEST("keycombo", "what is refused") {
    struct keycombo c;
    KTEST_ASSERT(!keycombo_parse("Ctrl", &c));        // a bare modifier
    KTEST_ASSERT(!keycombo_parse("Ctrl+Alt", &c));    // ...however many
    KTEST_ASSERT(!keycombo_parse("A+B", &c));         // two keys
    KTEST_ASSERT(!keycombo_parse("E+Super", &c));     // a modifier after the key
    KTEST_ASSERT(!keycombo_parse("Ctrl+", &c));       // an empty token
    KTEST_ASSERT(!keycombo_parse("Hypr+E", &c));      // no such modifier
    KTEST_ASSERT(!keycombo_parse("Frobnicate", &c));  // no such key
}

KTEST("keycombo", "a formatter that does not fit writes NOTHING") {
    struct keycombo c;
    KTEST_ASSERT(keycombo_parse("Ctrl+Alt+Shift+Super+Print Screen", &c));
    char small[8];
    k_memset(small, 'x', sizeof small);
    KTEST_ASSERT(!keycombo_format(&c, small, sizeof small));
    // Untouched -- not truncated, which is this project's rule for every
    // formatter (CLAUDE.md).
    for (unsigned i = 0; i < sizeof small; i++) KTEST_ASSERT(small[i] == 'x');
}

KTEST("keycombo", "matching is EXACT, so bindings do not shadow") {
    struct keycombo c;
    KTEST_ASSERT(keycombo_parse("Super+E", &c));
    KTEST_ASSERT(keycombo_matches(&c, 'e', KEY_MOD_SUPER));   // the layout gives lower case
    KTEST_ASSERT(keycombo_matches(&c, 'E', KEY_MOD_SUPER));
    // A third modifier held makes it a DIFFERENT combination, which is
    // what lets Shift+Super+S exist beside Super+S.
    KTEST_ASSERT(!keycombo_matches(&c, 'e', KEY_MOD_SUPER | KEY_MOD_SHIFT));
    KTEST_ASSERT(!keycombo_matches(&c, 'e', 0));
}

KTEST("keycombo", "Ctrl with a NON-letter compares directly") {
    // **ONLY A LETTER FOLDS.** The driver pushes Delete, F5 and Print
    // Screen before it ever looks at Ctrl, so those keep their own code
    // -- and the first version of this bailed out on anything that was
    // not a letter, which made Ctrl+Shift+Esc a default that could never
    // fire. Nothing here covered it, which is why it shipped.
    struct keycombo c;
    KTEST_ASSERT(keycombo_parse("Ctrl+Alt+Delete", &c));
    KTEST_ASSERT(keycombo_matches(&c, KEY_DELETE, KEY_MOD_CTRL | KEY_MOD_ALT));
    KTEST_ASSERT(!keycombo_matches(&c, KEY_INSERT, KEY_MOD_CTRL | KEY_MOD_ALT));

    KTEST_ASSERT(keycombo_parse("Ctrl+F5", &c));
    KTEST_ASSERT(keycombo_matches(&c, KEY_F5, KEY_MOD_CTRL));
}

KTEST("keycombo", "Ctrl matches the CONTROL CODE the driver actually sends") {
    // By the time a key arrives, Ctrl+T is 0x14 and the letter is gone
    // (api/keyboard.h). Nothing outside this file should have to know.
    struct keycombo c;
    KTEST_ASSERT(keycombo_parse("Ctrl+Alt+T", &c));
    KTEST_ASSERT(keycombo_matches(&c, 0x14, KEY_MOD_CTRL | KEY_MOD_ALT));
    KTEST_ASSERT(!keycombo_matches(&c, 'T', KEY_MOD_CTRL | KEY_MOD_ALT));
    // The control code alone, without Alt, is a different binding.
    KTEST_ASSERT(!keycombo_matches(&c, 0x14, KEY_MOD_CTRL));
}

KTEST("keycombo", "a Latin-1 key round-trips through format and parse") {
    // A capture of Ctrl+e-acute formats as one Latin-1 byte; parse must
    // read that byte back, or the setting can be recorded and never fire.
    struct keycombo c = { .key = 0xE9, .mods = KEY_MOD_CTRL }, back;
    char text[KEYCOMBO_TEXT_MAX];
    KTEST_ASSERT(keycombo_format(&c, text, sizeof text));
    KTEST_ASSERT(keycombo_parse(text, &back));
    KTEST_ASSERT_EQ(back.key, 0xC9);           // stored upper-case, like ASCII
    KTEST_ASSERT_EQ(back.mods, KEY_MOD_CTRL);
    // ...and the live key (lower-case, as the keyboard sends it) matches.
    KTEST_ASSERT(keycombo_matches(&back, 0xE9, KEY_MOD_CTRL));
    KTEST_ASSERT(keycombo_matches(&back, 0xC9, KEY_MOD_CTRL));
    KTEST_ASSERT(!keycombo_matches(&back, 'e', KEY_MOD_CTRL));
    // A special with no name cannot be spelled, so it is refused.
    struct keycombo nameless = { .key = KEY_SHIFT_END, .mods = 0 };
    KTEST_ASSERT(!keycombo_format(&nameless, text, sizeof text));
}
