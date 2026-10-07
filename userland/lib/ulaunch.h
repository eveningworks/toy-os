#ifndef ULIB_ULAUNCH_H
#define ULIB_ULAUNCH_H

// ulaunch -- is this file something to RUN, and how should it run?
//
// The question a double-click asks before uopen.h's "which app opens
// it": an ELF program or a `#!` script is started, not opened. Answered
// from the file's first bytes and its mode, never its name -- a script
// called `backup` is still a script.
//
// THE EXECUTE BIT IS ASKED, NOT ASSUMED. The kernel's loader does not
// check it, but dash does, and a desktop that ran a file the shell
// refuses would make the bit meaningless. A file without it is reported
// as such (`runnable` 0) and the caller offers to set it.
//
// What a double-click does per kind is a POLICY in /etc/mimeapps.conf,
// keyed by freedesktop's MIME names (application/x-shellscript,
// application/x-executable) beside the per-extension choices -- so the
// File Manager, the desktop and `open` all read the same answer.

#define ULAUNCH_MIME_SCRIPT  "application/x-shellscript"
#define ULAUNCH_MIME_PROGRAM "application/x-executable"

enum ulaunch_kind {
    ULAUNCH_NONE = 0,  // not something to run: a document, a library
    ULAUNCH_APP,       // an ELF program with a window: a desktop entry runs it, or it is under /bin/wm
    ULAUNCH_PROGRAM,   // an ELF program for a terminal
    ULAUNCH_SCRIPT,    // `#!` and an interpreter
};

// What to do with one. ASK is a policy only, never an act.
enum ulaunch_act {
    ULAUNCH_ASK = 0,
    ULAUNCH_TERMINAL,  // in a Terminal window that stays when it ends
    ULAUNCH_RUN,       // detached, no window of its own
    ULAUNCH_EDIT,      // a script, in Notepad
};

#define ULAUNCH_HEAD_LINES 4
#define ULAUNCH_HEAD_COLS  72

struct ulaunch_info {
    int kind;                 // enum ulaunch_kind
    int runnable;             // an execute bit is set
    int interp_found;         // a script's interpreter exists (1 for ELF)
    char interp[64];          // a script's `#!` program, its first word
    char app[48];             // an APP's desktop-entry Name=, else ""
    // A script's first lines, tabs as spaces, cut at ULAUNCH_HEAD_COLS:
    // what the ask card shows of it.
    char head[ULAUNCH_HEAD_LINES][ULAUNCH_HEAD_COLS + 1];
    int head_lines;
};

// Reads at most the first 512 bytes. 1 and `out` filled, or 0 when the
// file cannot be read or is a directory (kind ULAUNCH_NONE either way).
int ulaunch_classify(const char *path, struct ulaunch_info *out);

// The double-click's policy for a kind: ULAUNCH_ASK unless the user
// chose. An APP is never asked -- it opens. EDIT for a program reads as
// ASK: there is nothing to edit.
int ulaunch_policy(int kind);
// Remember `act` for `kind` (ASK forgets). 0 on a write failure.
int ulaunch_set_policy(int kind, int act);

// Give the file an execute bit wherever it has a read bit -- chmod
// a+x as `chmod +x` does it, honouring a umask of 022. 0 on failure
// (FAT32 stores no mode).
int ulaunch_allow(const char *path);

// The argv that does `act` to `path`, into `argv` (at least 4 slots,
// NULL-terminated); the strings point into `path` and constants. 0 for
// ASK or an act that does not apply to the kind.
#define ULAUNCH_TERMINAL_EXEC "/bin/wm/apps/uterm"
#define ULAUNCH_EDIT_EXEC     "/bin/wm/apps/notepad"
#define ULAUNCH_ASK_EXEC      "/bin/wm/system/runask"
int ulaunch_argv(const char *path, int kind, int act, char **argv);

#endif
