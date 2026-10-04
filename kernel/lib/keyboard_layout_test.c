// KTESTs for kernel/lib/keyboard_layout.c: the generated layouts, the
// four levels, Latin-1 Caps Lock and dead-key composition.
//
// **EVERY TEST REPLACES THE LIVE LAYOUT, SO EVERY TEST PUTS IT BACK.**
// The tables are the machine's keyboard; a test that returned early on
// a failed assertion would leave the console typing German. So the
// checks run in a helper that returns the failing LINE (0 = pass) and
// the KTEST body restores the saved layout before asserting on it.
#include "ktest.h"
#include "keyboard_layout.h"
#include "fs.h"
#include "string.h"

#define CHECK(cond) do { if (!(cond)) return __LINE__; } while (0)

// Evdev keycodes (abi/input_keys.h names them; the numbers are the ABI).
enum { KC_2 = 3, KC_MINUS_ROW_END = 13, KC_Q = 16, KC_E = 18, KC_O = 24,
       KC_P_RIGHT = 26, KC_X = 45, KC_SEMI = 39, KC_APOS = 40, KC_SPACE = 57,
       KC_BKSP = 14 };

// Feeds `sym` through the composer; returns how many characters came
// out, with them in out[].
static int feed(int sym, uint8_t out[2]) {
    out[0] = out[1] = 0;
    return keyboard_layout_compose(sym, out);
}

struct saved { char name[KB_LAYOUT_NAME_MAX]; };
static void save(struct saved *s) {
    k_strlcpy(s->name, keyboard_layout_current(), sizeof s->name);
    keyboard_layout_compose_reset();
}
static void restore(const struct saved *s) {
    keyboard_layout_load(s->name);
    keyboard_layout_compose_reset();
}

// Every shipped layout loads from its own file (precondition: an image
// seeded with xkbcli present, which `make iso` and CI both are).
static int have(const char *name) {
    char path[32] = "/etc/kbs/";
    k_strlcat(path, name, sizeof path);
    return fs_exists(path);
}

// --- a small fixture: the parser and the composer, independent of XKB --

static const char FIXTURE[] =
    "# comment\n"
    "kc_16=q\nkc_16_shift=Q\nkc_16_altgr=@\nkc_16_shift_altgr=0xAE\n"
    "kc_18=e\nkc_18_shift=E\n"
    "kc_45=x\n"
    "kc_57= \n"
    "kc_14=0x08\n"
    "kc_26=0xE5\nkc_26_shift=0xC5\n"
    "kc_13=dead:acute\nkc_13_shift=dead:grave\n"
    "dead:acute=0xB4\n"
    "dead:acute:e=0xE9\ndead:acute:E=0xC9\n"
    "dead:grave=`\n"
    "dead:grave:e=0xE8\n";

static int fixture_checks(void) {
    CHECK(keyboard_layout_load_text(FIXTURE, sizeof FIXTURE - 1));
    // The four levels, and level 4 falling back to 3 to Shift/base.
    CHECK(keyboard_layout_translate(KC_Q, 0, 0) == 'q');
    CHECK(keyboard_layout_translate(KC_Q, 1, 0) == 'Q');
    CHECK(keyboard_layout_translate(KC_Q, 0, 1) == '@');
    CHECK(keyboard_layout_translate(KC_Q, 1, 1) == 0xAE);
    CHECK(keyboard_layout_translate(KC_E, 1, 1) == 'E');
    // A Latin-1 value is POSITIVE -- the signed-char trap.
    CHECK(keyboard_layout_translate(KC_P_RIGHT, 0, 0) == 0xE5);
    // Caps Lock capitalises a Latin-1 letter, as it does a-z.
    CHECK(keyboard_layout_translate_caps(KC_P_RIGHT, 0, 0, 1) == 0xC5);
    CHECK(keyboard_layout_translate_caps(KC_P_RIGHT, 1, 0, 1) == 0xE5);

    int acute = keyboard_layout_translate(13, 0, 0);
    int grave = keyboard_layout_translate(13, 1, 0);
    CHECK(KB_SYM_IS_DEAD(acute) && KB_SYM_IS_DEAD(grave) && acute != grave);
    uint8_t o[2];
    // dead + composable -> the composed letter, and nothing else.
    CHECK(feed(acute, o) == 0 && keyboard_layout_dead_pending() == acute);
    CHECK(feed('e', o) == 1 && o[0] == 0xE9 && !keyboard_layout_dead_pending());
    CHECK(feed(acute, o) == 0 && feed('E', o) == 1 && o[0] == 0xC9);
    // dead + Space -> the spacing accent (Windows' rule, not XKB's ').
    CHECK(feed(acute, o) == 0 && feed(' ', o) == 1 && o[0] == 0xB4);
    // dead + itself -> the accent, and nothing pending after.
    CHECK(feed(acute, o) == 0 && feed(acute, o) == 1 && o[0] == 0xB4);
    CHECK(!keyboard_layout_dead_pending());
    // dead + a key it does not compose with -> accent, then the key.
    CHECK(feed(acute, o) == 0 && feed('x', o) == 2 && o[0] == 0xB4 && o[1] == 'x');
    // dead + another dead -> the first accent; the second waits.
    CHECK(feed(acute, o) == 0 && feed(grave, o) == 1 && o[0] == 0xB4);
    CHECK(keyboard_layout_dead_pending() == grave);
    CHECK(feed('e', o) == 1 && o[0] == 0xE8);
    // dead + Backspace -> the accent is taken back, nothing typed.
    CHECK(feed(acute, o) == 0 && feed('\b', o) == 0 && !keyboard_layout_dead_pending());
    // Nothing pending: a character is itself.
    CHECK(feed(0xE5, o) == 1 && o[0] == 0xE5);
    return 0;
}

KTEST("kblayout", "the fixture: four levels, Latin-1 caps, every dead-key rule") {
    struct saved s;
    save(&s);
    int line = fixture_checks();
    restore(&s);
    KTEST_ASSERT_EQ(line, 0);
}

// --- the generated layouts --------------------------------------------

static int generated_checks(void) {
    uint8_t o[2];
    // German: AltGr+Q is @, and the key right of 0 is sharp s.
    CHECK(keyboard_layout_load("de") == 1);
    CHECK(keyboard_layout_translate(KC_Q, 0, 1) == '@');
    CHECK(keyboard_layout_translate(12, 0, 0) == 0xDF);
    // German's acute is the dead key left of Backspace.
    int acute = keyboard_layout_translate(KC_MINUS_ROW_END, 0, 0);
    CHECK(KB_SYM_IS_DEAD(acute));
    CHECK(feed(acute, o) == 0 && feed('e', o) == 1 && o[0] == 0xE9);

    // French: the unshifted 2 key is e-acute.
    CHECK(keyboard_layout_load("fr") == 1);
    CHECK(keyboard_layout_translate(KC_2, 0, 0) == 0xE9);

    // Spanish: the dead acute (right of n-tilde), then e, Space, x.
    CHECK(keyboard_layout_load("es") == 1);
    acute = keyboard_layout_translate(KC_APOS, 0, 0);
    CHECK(KB_SYM_IS_DEAD(acute));
    CHECK(feed(acute, o) == 0 && feed('e', o) == 1 && o[0] == 0xE9);
    CHECK(feed(acute, o) == 0 && feed(' ', o) == 1 && o[0] == 0xB4);
    CHECK(feed(acute, o) == 0 && feed('x', o) == 2 && o[0] == 0xB4 && o[1] == 'x');
    CHECK(keyboard_layout_translate(KC_SEMI, 0, 0) == 0xF1);   // n-tilde

    // Swedish still types its three letters, both cases.
    CHECK(keyboard_layout_load("se") == 1);
    CHECK(keyboard_layout_translate(KC_P_RIGHT, 0, 0) == 0xE5);
    CHECK(keyboard_layout_translate(KC_P_RIGHT, 1, 0) == 0xC5);
    CHECK(keyboard_layout_translate(KC_APOS, 0, 0) == 0xE4);
    CHECK(keyboard_layout_translate(KC_SEMI, 0, 0) == 0xF6);
    CHECK(keyboard_layout_translate_caps(KC_SEMI, 0, 0, 1) == 0xD6);
    return 0;
}

KTEST("kblayout", "generated de/fr/es/se: AltGr, Latin-1 base keys, dead acute") {
    if (!have("de") || !have("fr") || !have("es") || !have("se"))
        KTEST_SKIP("/etc/kbs not seeded (no xkbcli on the build host)");
    struct saved s;
    save(&s);
    int line = generated_checks();
    restore(&s);
    KTEST_ASSERT_EQ(line, 0);
}

static int all_load_checks(void) {
    static const char *const names[] = {
        "al", "at", "be", "br", "ca", "ch", "de", "dk", "es", "fi", "fo", "fr",
        "gb", "is", "it", "latam", "lv", "nl", "no", "pl", "pt", "ro", "se", "us",
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        CHECK(keyboard_layout_load(names[i]) == 1);
        CHECK(k_strcmp(keyboard_layout_current(), names[i]) == 0);
        // Every layout types Space and Backspace.
        CHECK(keyboard_layout_translate(KC_SPACE, 0, 0) == ' ');
        CHECK(keyboard_layout_translate(KC_BKSP, 0, 0) == '\b');
    }
    return 0;
}

KTEST("kblayout", "every shipped layout loads from its own file") {
    if (!have("us") || !have("latam"))
        KTEST_SKIP("/etc/kbs not seeded (no xkbcli on the build host)");
    struct saved s;
    save(&s);
    int line = all_load_checks();
    restore(&s);
    KTEST_ASSERT_EQ(line, 0);
}
