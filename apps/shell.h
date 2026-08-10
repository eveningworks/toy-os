#ifndef SHELL_H
#define SHELL_H

#include "vga.h"

// The shell app's entry point. Registered in apps/apps.c as "shell" --
// this is what apps_start() launches after the kernel finishes hardware
// bring-up. Runs forever (it's a read-eval-print loop).
void shell_main(void);

// Runs a single command line through the real shell command dispatcher
// -- the exact same ~40 handlers shell_main()'s REPL uses -- with all
// console output (and vga_rows()'s answer) redirected through `sink`
// instead of the physical screen. Pass NULL for `sink` to run against
// the physical console, same as shell_main()'s own loop.
//
// `line` is mutated in place (dispatch() splits it in place at the
// first space to separate the command from its arguments), so pass a
// writable buffer, not a string literal.
//
// This is what lets a GUI terminal-emulator app (see apps/terminal.c)
// reuse the real shell instead of duplicating its command handlers --
// see vga.h's struct vga_sink comment for the mechanism, and
// vga_sink_active() for how the couple of commands that would
// otherwise block on keyboard input (cmd_timezone's interactive
// picker, help's pager) detect a sink is active and skip the blocking
// part instead of hanging whatever's driving this through a sink.
//
// Sinks don't nest via this call -- it saves and restores whatever
// sink (if any) was active before, so calling this from inside an
// already-sinked context is safe, but it always applies exactly the
// one `sink` passed in for the duration of the call.
void shell_dispatch(char *line, const struct vga_sink *sink);

// The shell's current working directory -- one shared piece of state
// across every caller (the physical console's own REPL and any
// shell_dispatch() caller alike; there's only ever one shell "session"
// in this kernel, same as there's only one of everything else here).
// Read-only: nothing outside shell.c ever sets this directly -- `cd`
// (via dispatch()/shell_dispatch()) is the only way it changes. Lets a
// caller printing its own prompt (see apps/terminal.c) show the same
// directory the physical shell would.
const char *shell_cwd(void);

#endif
