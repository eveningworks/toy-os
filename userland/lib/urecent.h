#ifndef ULIB_URECENT_H
#define ULIB_URECENT_H

// RECENT FILES: what was opened lately, by any app, and in which --
// freedesktop's recently-used.xbel and Windows' Recent Items, as one
// plain-text list in /var/lib/recent. Written where files are OPENED for
// a person rather than by each app: lib/uopen.h's spawn (a double click,
// `open`) and the shared file dialog (Windows records in its shell and
// its common dialog for the same reason), plus Notepad's own Recent menu.
//
// One line per file, `<epoch> <app>\t<path>`, oldest first; opening a
// file again moves it to the end rather than adding a second line, and
// past URECENT_MAX the oldest go. Read whole and rewritten through a
// temporary file and a rename, under a lock file (urecent.c says why).

#define URECENT_FILE "/var/lib/recent"
#define URECENT_MAX  100
#define URECENT_APP  48
#define URECENT_PATH 256

struct urecent_item {
    long long when;              // seconds since the epoch, UTC
    char app[URECENT_APP];       // "Image Viewer"
    char path[URECENT_PATH];
};

// Record that `app` opened `path` (absolute) just now. 0, or -errno.
int urecent_add(const char *path, const char *app);

// The list, NEWEST FIRST, at most `cap`; files that no longer exist are
// left out (and kept in the file -- an unplugged disk comes back).
int urecent_list(struct urecent_item *out, int cap);

// Take `path` off the list / empty it. 0, or -errno.
int urecent_forget(const char *path);
int urecent_clear(void);

// The name a person knows `exec` by -- its desktop entry's Name=, else
// the program's file name. For a caller that has only the program.
void urecent_app_of_exec(const char *exec, char *out, int cap);

#endif
