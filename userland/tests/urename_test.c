// lib/urename.h: the names each rule gives, against a table worked out
// by hand, then the plan's two refusals on real files -- two of the set
// to one name, and a name something else already has.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/utest.h"
#include "lib/urename.h"

#define DIR "/home/urename-test"

static void check(const struct urename_rule *r, const char *in, int i, const char *want) {
    char out[URENAME_NAME];
    int ok = urename_name(r, in, i, out, sizeof out);
    if (want) utest_checkf(ok && !strcmp(out, want), "%s (#%d) -> %s (got %s)", in, i, want, ok ? out : "refused");
    else utest_checkf(!ok, "%s is refused (got %s)", in, ok ? out : "refused");
}

static void touch(const char *name) {
    char p[128];
    snprintf(p, sizeof p, "%s/%s", DIR, name);
    int fd = open(p, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
}

int main(void) {
    utest_begin("urename_test", "renaming many files at once", UTEST_VERDICT_FILE);

    struct urename_rule r = { .mode = URENAME_NUMBER, .pattern = "photo-##", .start = 1, .keep_ext = 1 };
    check(&r, "dusk.jpg", 0, "photo-01.jpg");
    check(&r, "slate.JPG", 9, "photo-10.JPG");          // the extension as it was
    r.digits = 3;
    check(&r, "dusk.jpg", 0, "photo-001.jpg");          // digits wider than the run
    strcpy(r.pattern, "photo");
    r.digits = 0;
    check(&r, "dusk.jpg", 4, "photo 5.jpg");            // no '#': " N" at the end
    r.keep_ext = 0;
    strcpy(r.pattern, "photo-#");
    check(&r, "dusk.jpg", 0, "photo-1");                // the extension goes with it

    struct urename_rule f = { .mode = URENAME_REPLACE, .find = "img_", .replace = "trip-", .keep_ext = 1 };
    check(&f, "IMG_0042.JPG", 0, "trip-0042.JPG");      // case-insensitive by default
    f.match_case = 1;
    check(&f, "IMG_0042.JPG", 0, "IMG_0042.JPG");       // ...and exact when asked
    struct urename_rule e = { .mode = URENAME_REPLACE, .find = "a", .replace = "", .keep_ext = 1 };
    check(&e, "a.txt", 0, 0);                           // nothing left of the name
    struct urename_rule s = { .mode = URENAME_REPLACE, .find = "-", .replace = "/", .keep_ext = 1 };
    check(&s, "a-b.txt", 0, 0);                         // a '/' is never a name

    struct urename_rule c = { .mode = URENAME_CASE, .casing = URENAME_TITLE, .keep_ext = 1 };
    check(&c, "my holiday photo.JPG", 0, "My Holiday Photo.JPG");
    c.casing = URENAME_UPPER;
    check(&c, "notes.txt", 0, "NOTES.txt");
    c.casing = URENAME_LOWER;
    check(&c, ".Profile", 0, ".profile");               // a leading dot is a name

    // THE PLAN, on real files.
    sys_mkdir(DIR);
    touch("A.txt");
    touch("a2.txt");
    touch("b.txt");
    static char names[3][URENAME_NAME] = { "A.txt", "a2.txt", "b.txt" }, news[3][URENAME_NAME];
    static int st[3];
    struct urename_rule lower = { .mode = URENAME_CASE, .casing = URENAME_LOWER, .keep_ext = 1 };
    int n = urename_plan(&lower, DIR, names, 2, news, st);
    utest_checkf(n == 1 && st[0] == URENAME_OK && st[1] == URENAME_SAME,
                 "lower-casing A.txt is a change, a2.txt is not (%d: %d %d)", n, st[0], st[1]);
    struct urename_rule ab = { .mode = URENAME_REPLACE, .find = "A", .replace = "b", .keep_ext = 1 };
    static char one[1][URENAME_NAME] = { "A.txt" };
    urename_plan(&ab, DIR, one, 1, news, st);
    utest_checkf(st[0] == URENAME_CLASH, "a name a file outside the set has is a CLASH (%d)", st[0]);
    // Two of the set to one name: "A.txt" and "a.TXT" both upper-case to A.TXT.
    static char twins[2][URENAME_NAME] = { "A.txt", "a.TXT" };
    struct urename_rule up = { .mode = URENAME_CASE, .casing = URENAME_UPPER, .keep_ext = 0 };
    urename_plan(&up, DIR, twins, 2, news, st);
    utest_checkf(st[0] == URENAME_CLASH && st[1] == URENAME_CLASH,
                 "two of the set to one name are both a CLASH (%d %d)", st[0], st[1]);
    unlink(DIR "/A.txt");
    unlink(DIR "/a2.txt");
    unlink(DIR "/b.txt");
    unlink(DIR);
    return utest_end();
}
