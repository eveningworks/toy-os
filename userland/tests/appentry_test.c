// lib/uappentry: the desktop's application entries, read in ring 3.
//
// Against the real /usr/wm/applications (a shipped entry must resolve by
// its Exec=) and against entries written here, for the shapes the shipped
// ones do not have: freedesktop's [Desktop Entry] section, arguments in
// Exec=, no Name=, and no Exec= at all.
#include <stdio.h>
#include <string.h>
#include "lib/uappentry.h"
#include "lib/utest.h"

static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    int ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static int count(const struct uappentry *e, void *ctx) {
    int *n = ctx;
    if (e->exec[0] == '/') (*n)++;
    return 0;
}

int main(void) {
    utest_begin("appentry_test", "the desktop entry reader, in ring 3", 0);
    struct uappentry e;

    int hit = uappentry_find_exec("/bin/wm/apps/notepad", &e);
    utest_check(hit, "Notepad's entry is found by its Exec=");
    utest_checkf(hit && !strcmp(e.name, "Notepad") && !strcmp(e.stem, "notepad"),
                 "it reads Name= and the stem (name '%s', stem '%s')", e.name, e.stem);
    utest_check(!uappentry_find_exec("/bin/no/such/program", &e), "an unknown program has no entry");

    int good = 0;
    int seen = uappentry_each(count, &good);
    utest_checkf(seen >= 10 && good == seen, "every entry walked names a program (%d of %d)", good, seen);

    write_file("/tmp/ae_section.desktop",
               "[Desktop Entry]\nName=Sectioned\nExec=/bin/x --flag %f\nIcon=xicon\n");
    hit = uappentry_read("/tmp/ae_section.desktop", &e);
    utest_checkf(hit && !strcmp(e.exec, "/bin/x") && !strcmp(e.name, "Sectioned") &&
                 !strcmp(e.icon, "xicon"),
                 "a sectioned entry reads, and Exec= keeps only its program (exec '%s')", e.exec);

    write_file("/tmp/ae_noname.desktop", "Exec=/bin/y\n");
    hit = uappentry_read("/tmp/ae_noname.desktop", &e);
    utest_checkf(hit && !strcmp(e.name, "ae_noname"), "no Name= falls back to the stem ('%s')", e.name);

    write_file("/tmp/ae_noexec.desktop", "Name=Nothing\n");
    utest_check(!uappentry_read("/tmp/ae_noexec.desktop", &e), "an entry with no Exec= is refused");
    utest_check(!uappentry_read("/tmp/ae_missing.desktop", &e), "a missing file is refused");

    remove("/tmp/ae_section.desktop");
    remove("/tmp/ae_noname.desktop");
    remove("/tmp/ae_noexec.desktop");
    return utest_end();
}
