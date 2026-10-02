#ifndef ULIB_UAPPENTRY_H
#define ULIB_UAPPENTRY_H

// The desktop's application entries -- /usr/wm/applications/*.desktop --
// read one, walk them, or find the one that launches a given program.
//
// freedesktop's Desktop Entry format, read with uconf: `Name=`, `Exec=`,
// `Icon=` and `Category=`, from `[Desktop Entry]` first and then the top
// level (uopen.h, UOPEN_ENTRY_SECTION), so a .desktop copied off Linux
// reads unchanged. `exec` is Exec='s FIRST WORD -- the program path, which
// is what a process's exec path is compared with; any arguments are cut.

#define UAPPENTRY_DIR "/usr/wm/applications"

struct uappentry {
    char stem[32];       // the file name without ".desktop": what mimeapps.conf names
    char name[48];       // Name=, else the stem
    char exec[96];       // Exec='s first word; never empty in a returned entry
    char icon[32];       // Icon=, or ""
    char category[24];   // Category=, or ""
};

// One entry file. 1 when it parsed and names a program, else 0 -- an
// entry with no Exec= launches nothing, so it is not an application.
int uappentry_read(const char *path, struct uappentry *out);

// Every application entry, in directory order. Stops early when `fn`
// returns nonzero; returns how many entries `fn` saw.
int uappentry_each(int (*fn)(const struct uappentry *e, void *ctx), void *ctx);

// The entry whose Exec= program is `exec`. 1 and `out` filled, else 0.
int uappentry_find_exec(const char *exec, struct uappentry *out);

#endif
