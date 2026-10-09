// The keyboard translator's SECOND compilation (kernel/lib/keyboard_layout.c
// in libuapp.a), against the shipped Finnish layout -- the on-screen
// keyboard types with it, so what it answers here is what the panel
// types. The KTESTs in kernel/drivers/input/input_test.c cover the logic
// and would pass whether or not ring 3 linked a byte of it.
//
// Finnish because it has every level US lacks: AltGr characters, the
// ISO key beside Shift, and dead keys with compose pairs.
#include <string.h>
#include "keyboard_layout.h"
#include "lib/ukeymap.h"
#include "lib/utest.h"

static struct ukeymap g_map;   // 1.3 KB: off the 2 KB frame

int main(void) {
    utest_begin("keymap_test", "the ring-3 keyboard translator, on the Finnish layout", 0);

    int loaded = ukeymap_load(&g_map, "fi");
    utest_check(loaded, "/usr/share/kbs/fi loads");
    if (!loaded) return utest_end();

    utest_check(keyboard_layout_translate(3, 0, 1) == '@', "AltGr+2 is @");
    utest_check(keyboard_layout_translate(9, 1, 0) == '(', "Shift+8 is (");
    utest_check(keyboard_layout_translate(86, 0, 0) == '<' &&
                keyboard_layout_translate(86, 0, 1) == '|',
                "the ISO key beside Shift is < and AltGr |");
    utest_check(keyboard_layout_translate(16, 0, 1) == 'q',
                "AltGr over a key with no AltGr character types the key");

    uint16_t kc = 0;
    int sh = -1, ag = -1;
    utest_check(keyboard_layout_find('@', &kc, &sh, &ag) && kc == 3 && !sh && ag,
                "the inverse finds @ on AltGr+2");

    // The snapshot the board draws from agrees with the tables typing uses.
    int dead = 0;
    int acute = ukeymap_char(&g_map, 13, UKEYMAP_BASE, &dead);
    utest_check(dead && acute == 0xB4, "the snapshot marks the acute key dead, with its accent");
    utest_check(ukeymap_char(&g_map, 3, UKEYMAP_ALTGR, 0) == '@', "...and shows @ on AltGr+2");

    // Dead acute, then e: the compose pairs the old ring-3 parser skipped.
    keyboard_layout_set_dead_keys(1);
    keyboard_layout_compose_reset();
    uint8_t out[2];
    int n1 = keyboard_layout_compose(keyboard_layout_translate(13, 0, 0), out);
    int pending = keyboard_layout_dead_pending() != 0;
    int n2 = keyboard_layout_compose('e', out);
    utest_check(n1 == 0 && pending && n2 == 1 && out[0] == 0xE9,
                "dead acute then e composes to e-acute");
    n1 = keyboard_layout_compose(keyboard_layout_translate(13, 0, 0), out);
    n2 = keyboard_layout_compose(' ', out);
    utest_check(n2 == 1 && out[0] == 0xB4, "dead acute then Space types the accent alone");

    // The compiled-in table, for a machine with no layout files.
    keyboard_layout_use_fallback();
    ukeymap_snapshot(&g_map);
    utest_check(keyboard_layout_translate(16, 0, 0) == 'q' &&
                ukeymap_char(&g_map, 16, UKEYMAP_BASE, 0) == 'q',
                "the fallback table types and shows US");
    return utest_end();
}
