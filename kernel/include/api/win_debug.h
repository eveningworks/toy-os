#ifndef API_WIN_DEBUG_H
#define API_WIN_DEBUG_H

// Running a `gui ...` command from apps/ -- the ONE thing an app needs
// from the window transport, exposed without handing it the whole
// kernel-internal header (kernel/win_transport.h is not on apps/'s
// include path, and that boundary is enforced by the build).
//
// Its caller is the scripted demo (apps/demo.c), which performs a tour
// one `gui` subcommand per frame. That used to be a direct call into
// userland/wm/wm_debug.c, which only worked while the window manager was
// ring-0 code linked into this image; it is a process now, so the demo
// talks to it the same way the debug console and every GUI test tool do
// rather than through a second mechanism that could behave differently.
//
// FIRE AND FORGET: the reply is discarded. A demo is a performance, not
// a test -- a step that fails should not stop the tour, and
// tools/demo_test.py is what asserts the tour actually did anything.
// Returns 1 if a window manager took the command at all, 0 if none is
// running.
int win_debug_command(const char *cmd);

#endif
