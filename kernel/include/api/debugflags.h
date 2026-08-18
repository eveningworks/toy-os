#ifndef DEBUGFLAGS_H
#define DEBUGFLAGS_H

// Lightweight per-subsystem debug-logging switches, OFF by default,
// flippable at runtime via the shell's `debug` command
// (apps/shell_sys.c) -- no rebuild needed.
//
// Why this exists: every prior debugging session hand-rolled its own
// temporary klog_write() calls at the point of suspicion, confirmed
// the bug, then had to remember to delete them again before shipping
// (see the file picker's redraw_pending bug this same session --
// added twice, removed twice). This gives every subsystem a standing,
// named, off-by-default gate instead: wrap a klog_write() in
// `if (dbgflag_enabled(DBGFLAG_WM)) { ... }` and leave it in the tree
// permanently. It costs one bitmask read when off, and flips on with
// `debug wm on` at the shell (or `debug` alone to see what's on)
// without touching source or rebuilding. Distinct from
// kernel/core/debug_console.c's serial-only `dbg>` REPL (lsdev/lsfs/
// meminfo commands) -- that's an interactive inspector, this is a
// silent always-there gate other code checks before logging.
//
// Add a new subsystem by extending the enum AND its name in
// debugflags.c's DBGFLAG_NAMES (kept in lockstep, same convention as
// gui_flow.py's APP_ORDER needing to match gui_app_registry[]) --
// `debug` (no args) and `debug <name>` both already read the names
// table generically, nothing else needs updating.
enum dbgflag_subsys {
    DBGFLAG_FS = 0,   // kernel/fs/tfs.c and fs.h callers
    DBGFLAG_WM,       // userland/wm/* -- window manager, dialogs, widgets
    DBGFLAG_ATA,      // kernel/drivers/ata.c -- disk I/O, DMA/PIO, retries
    DBGFLAG_SUBSYS_COUNT
};

extern const char *const DBGFLAG_NAMES[DBGFLAG_SUBSYS_COUNT];

int dbgflag_enabled(enum dbgflag_subsys s);
void dbgflag_set(enum dbgflag_subsys s, int on);

// Case-sensitive exact match against DBGFLAG_NAMES (same "reject
// rather than guess" spirit as shell_sys.c's parse_decimal()) --
// returns 1 and sets *out on match, 0 otherwise.
int dbgflag_parse(const char *name, enum dbgflag_subsys *out);

#endif
