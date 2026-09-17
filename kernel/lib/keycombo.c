// Key combinations as text. See api/keycombo.h for the grammar and why
// this is shared code rather than the compositor's.
//
// COMPILED TWICE, like string.c and caltime.c beside it: ring 0
// validates what the registry stores, ring 3 matches and captures. So
// nothing here may name anything kernel-only.
#include "keycombo.h"
#include "keyboard.h"
#include "string.h"

// The keys a shortcut may name, beyond the printable characters the
// layout already gives. **THE NAME IS WHAT A MENU PRINTS**, so it is
// "Print Screen" and not "PRTSC" -- `userland/bin/kbd.c` has a terser
// table for a key TESTER, where the width of a column matters and the
// reader is looking at scancodes. Two audiences, two spellings; merging
// them would make one of the two worse.
static const struct { uint8_t key; const char *name; } NAMES[] = {
    { KEY_PRINT_SCREEN, "Print Screen" },
    { KEY_PAUSE,        "Pause" },
    { KEY_INSERT,       "Insert" },
    { KEY_DELETE,       "Delete" },
    { KEY_HOME,         "Home" },
    { KEY_END,          "End" },
    { KEY_PAGE_UP,      "Page Up" },
    { KEY_PAGE_DOWN,    "Page Down" },
    { KEY_ARROW_UP,     "Up" },
    { KEY_ARROW_DOWN,   "Down" },
    { KEY_ARROW_LEFT,   "Left" },
    { KEY_ARROW_RIGHT,  "Right" },
    { KEY_MENU,         "Menu" },
    { KEY_F1,  "F1" },  { KEY_F2,  "F2" },  { KEY_F3,  "F3" },
    { KEY_F4,  "F4" },  { KEY_F5,  "F5" },  { KEY_F6,  "F6" },
    { KEY_F7,  "F7" },  { KEY_F8,  "F8" },  { KEY_F9,  "F9" },
    { KEY_F10, "F10" }, { KEY_F11, "F11" }, { KEY_F12, "F12" },
    // The three printable keys whose CHARACTER is not a name anyone
    // would type into a shortcut box.
    { ' ',    "Space" },
    { '\t',   "Tab" },
    { '\n',   "Enter" },
    { 0x1B,   "Esc" },
    { 0x08,   "Backspace" },
};
#define NAME_COUNT ((int)(sizeof NAMES / sizeof NAMES[0]))

static const struct { const char *name; uint8_t bit; } MODS[] = {
    { "Ctrl",  KEY_MOD_CTRL },
    { "Alt",   KEY_MOD_ALT },
    { "Shift", KEY_MOD_SHIFT },
    { "Super", KEY_MOD_SUPER },
    { "AltGr", KEY_MOD_ALTGR },
    // ACCEPTED ON THE WAY IN, never written out: someone copying a
    // binding off a Windows or GNOME machine should not have to
    // translate it first.
    { "Win",   KEY_MOD_SUPER },
    { "Meta",  KEY_MOD_SUPER },
    { "Cmd",   KEY_MOD_SUPER },
    { "Control", KEY_MOD_CTRL },
};
#define MOD_COUNT ((int)(sizeof MODS / sizeof MODS[0]))

// The canonical output order, which is a SUBSET of the table above --
// the aliases must never be spelled back out.
static const uint8_t ORDER[] = {
    KEY_MOD_CTRL, KEY_MOD_ALT, KEY_MOD_SHIFT, KEY_MOD_SUPER, KEY_MOD_ALTGR
};
#define ORDER_COUNT ((int)(sizeof ORDER / sizeof ORDER[0]))

static const char *mod_name(uint8_t bit) {
    for (int i = 0; i < MOD_COUNT; i++)
        if (MODS[i].bit == bit) return MODS[i].name;   // the FIRST is canonical
    return 0;
}

static int eq_nocase(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (!b[i]) return 0;
        if (k_tolower((unsigned char)a[i]) != k_tolower((unsigned char)b[i])) return 0;
    }
    return b[n] == '\0';
}

// One `+`-separated token, with the surrounding blanks already gone.
static int token_key(const char *tok, int len, uint8_t *out_key, uint8_t *out_mod) {
    for (int i = 0; i < MOD_COUNT; i++) {
        if (eq_nocase(tok, MODS[i].name, len)) { *out_mod = MODS[i].bit; return 1; }
    }
    for (int i = 0; i < NAME_COUNT; i++) {
        if (eq_nocase(tok, NAMES[i].name, len)) { *out_key = NAMES[i].key; return 1; }
    }
    // A single printable character is itself. Stored UPPER-CASE so the
    // stored form and the formatted form agree; matching lower-cases
    // both ends anyway (keycombo_matches).
    if (len == 1 && tok[0] > 32 && (unsigned char)tok[0] < 127) {
        *out_key = (uint8_t)k_toupper((unsigned char)tok[0]);
        return 1;
    }
    return 0;
}

int keycombo_parse(const char *text, struct keycombo *out) {
    if (!text || !out) return 0;
    out->key = 0;
    out->mods = 0;

    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 1;                  // unbound, and that is legal

    int seen_key = 0;
    while (*p) {
        // `+` is the separator AND a legal key. A `+` that FOLLOWS a
        // separator is the key: "Ctrl++" is Ctrl and the plus key, which
        // is what a person typing it means.
        const char *start = p;
        if (*p == '+') p++;             // take one as content...
        while (*p && *p != '+') p++;    // ...then run to the next
        int len = (int)(p - start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
        while (len > 0 && (*start == ' ' || *start == '\t')) { start++; len--; }
        if (len <= 0) return 0;         // an empty token: "Ctrl++T", "A+"

        uint8_t key = 0, mod = 0;
        if (!token_key(start, len, &key, &mod)) return 0;
        if (mod) {
            if (seen_key) return 0;     // a modifier AFTER the key
            out->mods |= mod;
        } else {
            if (seen_key) return 0;     // two keys
            out->key = key;
            seen_key = 1;
        }
        if (*p == '+') p++;
    }
    // A BARE MODIFIER IS REFUSED, and api/keycombo.h says why.
    return seen_key;
}

int keycombo_format(const struct keycombo *c, char *out, size_t cap) {
    if (!c || !out || cap == 0) return 0;
    if (!c->key) { out[0] = '\0'; return 1; }

    const char *kname = 0;
    char one[2];
    for (int i = 0; i < NAME_COUNT; i++)
        if (NAMES[i].key == c->key) { kname = NAMES[i].name; break; }
    if (!kname) {
        one[0] = (char)k_toupper((unsigned char)c->key);
        one[1] = '\0';
        kname = one;
    }

    // MEASURED BEFORE ANYTHING IS WRITTEN: a formatter that does not fit
    // writes nothing rather than a truncated value (CLAUDE.md).
    size_t need = k_strlen(kname) + 1;
    for (int i = 0; i < ORDER_COUNT; i++) {
        if (!(c->mods & ORDER[i])) continue;
        const char *m = mod_name(ORDER[i]);
        if (m) need += k_strlen(m) + 1;   // the name and its `+`
    }
    if (need > cap) return 0;

    out[0] = '\0';
    for (int i = 0; i < ORDER_COUNT; i++) {
        if (!(c->mods & ORDER[i])) continue;
        const char *m = mod_name(ORDER[i]);
        if (!m) continue;
        k_strlcat(out, m, cap);
        k_strlcat(out, "+", cap);
    }
    k_strlcat(out, kname, cap);
    return 1;
}

int keycombo_matches(const struct keycombo *c, int key, uint8_t mods) {
    if (!c || !c->key || key < 0) return 0;
    // EXACTLY, not a subset -- api/keycombo.h says why.
    if (mods != c->mods) return 0;

    if (c->mods & KEY_MOD_CTRL) {
        // The letter is already gone by now: Ctrl+T IS 0x14. Nothing
        // else in this system has to know that, which is the point of
        // doing it here.
        //
        // **ONLY A LETTER FOLDS.** A special key keeps its own code --
        // the driver pushes F5, Delete and Print Screen before it ever
        // looks at Ctrl -- so Ctrl+Delete must compare directly. Bailing
        // out here instead, which is what this did first, made every
        // Ctrl combination with a non-letter unmatchable: Ctrl+Shift+Esc
        // was a default that could never fire.
        int lower = k_tolower((unsigned char)c->key);
        if (lower >= 'a' && lower <= 'z') return key == (lower - 'a' + 1);
        return key == c->key;
    }
    if (c->key < 127 && key < 127)
        return k_tolower((unsigned char)key) == k_tolower((unsigned char)c->key);
    return key == c->key;
}
