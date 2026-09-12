// THE SCREENSAVER OPTION LIBRARY, against the descriptors this image
// actually ships (userland/lib/usaver.h).
//
// It reads /usr/wm/savers and /etc/savers rather than a fixture, which
// is the point: the format is a contract between a data file and two
// programs that never see each other, and a parser tested against
// strings written beside it would keep agreeing with itself while a
// shipped descriptor drifted out from under both callers.
//
// WHAT A BROKEN VERSION WOULD STILL PASS is the question the checks are
// picked against -- so the round trip below WRITES a value, reloads,
// and requires it back, rather than asking whether a default came out
// of a file nobody wrote.
#include "lib/usaver.h"
#include "lib/uconf.h"
#include "lib/utest.h"
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>

static struct usaver g_s;

int main(void) {
    utest_begin("usaver_test", "screensaver option descriptors", UTEST_VERDICT_FILE);

    // Every shipped descriptor parses, and declares what it says it
    // does. A saver whose file is malformed loses its options SILENTLY
    // (usaver.c drops an option it cannot read), so the count is the
    // check that notices.
    static const struct { const char *name; int opts; } WANT[] = {
        { "starfield", 3 }, { "matrix", 3 }, { "plasma", 3 },
        { "bounce", 2 },    { "solid", 3 },
    };
    for (unsigned i = 0; i < sizeof WANT / sizeof WANT[0]; i++) {
        int got = usaver_load(WANT[i].name, &g_s) ? g_s.opt_count : -1;
        utest_checkf(got == WANT[i].opts, "%s declares %d options (got %d)",
                     WANT[i].name, WANT[i].opts, got);
    }

    // A SAVER WITHOUT A DESCRIPTOR IS NOT AN ERROR -- blank ships with
    // none, and the load has to leave a usable empty record rather than
    // whatever was in the struct before.
    usaver_load("starfield", &g_s);
    int had = g_s.opt_count;
    int loaded = usaver_load("blank", &g_s);
    utest_checkf(!loaded && g_s.opt_count == 0 && had == 3,
                 "blank has no descriptor and loads empty (%d, %d)",
                 loaded, g_s.opt_count);

    // The declaration's own fields, not just its presence.
    usaver_load("starfield", &g_s);
    const struct usaver_opt *stars = usaver_find(&g_s, "stars");
    utest_check(stars && stars->type == USAVER_INT && stars->imin == 50 &&
                stars->imax == 2000 && stars->istep == 10,
                "starfield.stars is an int 50..2000 by 10");
    const struct usaver_opt *col = usaver_find(&g_s, "colour");
    utest_check(col && col->type == USAVER_ENUM && col->choice_count == 4 &&
                strcmp(col->choice[0], "white") == 0,
                "starfield.colour is a 4-way enum starting at white");
    utest_check(!usaver_find(&g_s, "nosuch"), "an unknown key is not found");
    utest_checkf(usaver_int(&g_s, "nosuch", 77) == 77,
                 "an unknown key falls back (%d)", usaver_int(&g_s, "nosuch", 77));

    char disp[32];
    usaver_display("amber", disp, sizeof disp);
    utest_checkf(strcmp(disp, "Amber") == 0, "a value displays capitalised (%s)", disp);

    // THE ROUND TRIP, which is the only check here a defaults-only
    // implementation could not pass: write a value through the same
    // call System Settings uses, reload, and require it back.
    char path[64];
    usaver_conf_path("starfield", path, sizeof path);
    utest_checkf(strcmp(path, "/etc/savers/starfield.conf") == 0,
                 "the conf path is derived (%s)", path);
    int wrote = uconf_set(path, "stars", "123");
    usaver_load("starfield", &g_s);
    utest_checkf(wrote && usaver_int(&g_s, "stars", -1) == 123,
                 "a written value is read back (%d)", usaver_int(&g_s, "stars", -1));
    utest_checkf(usaver_int(&g_s, "speed", -1) == 14,
                 "an untouched option keeps its default (%d)",
                 usaver_int(&g_s, "speed", -1));

    // A VALUE THE DESCRIPTOR DOES NOT ALLOW READS AS THE DEFAULT. These
    // files are ordinary text that `edit` can change, so this is the
    // only thing between a typo and a saver told to draw 9,000,000
    // stars -- or a colour it has no branch for.
    uconf_set(path, "stars", "9000000");
    uconf_set(path, "colour", "purple");
    usaver_load("starfield", &g_s);
    utest_checkf(usaver_int(&g_s, "stars", -1) == 420,
                 "an out-of-range int is refused (%d)", usaver_int(&g_s, "stars", -1));
    utest_checkf(strcmp(usaver_str(&g_s, "colour", "?"), "white") == 0,
                 "an undeclared choice is refused (%s)",
                 usaver_str(&g_s, "colour", "?"));

    // usaver_index() is what a saver switches on.
    uconf_set(path, "colour", "ice");
    usaver_load("starfield", &g_s);
    utest_checkf(usaver_index(&g_s, "colour", -1) == 2,
                 "a choice resolves to its index (%d)",
                 usaver_index(&g_s, "colour", -1));

    // LEAVE NOTHING BEHIND: /etc/savers is a real directory this image
    // ships empty, and a file left here would change what the machine
    // draws for every later boot -- and fail check_layout.py.
    sys_unlink(path);
    return utest_end();
}
