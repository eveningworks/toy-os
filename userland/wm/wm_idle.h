#ifndef WM_IDLE_H
#define WM_IDLE_H

// THE IDLE CLOCK, AND THE SCREENSAVER IT STARTS.
//
// The compositor owns the clock; something else owns the pixels. That
// is the split every system makes -- Wayland's compositor tracks idle
// and tells a client through `ext-idle-notify-v1`, X11's server only
// blanks while XScreenSaver draws in its own process, and Windows runs
// a `.scr` executable the shell launches. It is also the split this
// desktop already uses for everything else, so a saver is an ORDINARY
// FULLSCREEN CLIENT out of /bin/wm/savers and not compositor code.
//
// Two settings, both persist-only (lib/usaver.h): which
// saver, and the minutes of quiet before it starts, where zero means
// never.
//
// THERE IS NO LOCK SCREEN, deliberately. toy-os has no accounts and no
// passwords, so a screen that demanded one would be theatre -- it would
// stop nobody and cost a way back in. A saver goes away on the first
// key or the first mouse movement.

// Called once per frame from wm_run(), AFTER the input has been read.
// `active` is whether anything arrived this frame: a key, a button
// edge, a wheel notch or a pointer that moved.
void wm_idle_poll(int active);

// The running saver's pid, or 0. For `guictl` and the test tool -- a
// saver is a process, so "is it up?" is a question with a real answer
// rather than a guess from pixels.
int wm_idle_saver_pid(void);

// Seconds since the last input. Reported for the same reason.
uint32_t wm_idle_seconds(void);

// The configured saver's name, and the timeout in minutes (0 = never).
const char *wm_idle_saver_name(void);
int wm_idle_minutes(void);

// A CLIENT OUT OF /bin/wm/savers HAS JUST OPENED A WINDOW, whoever
// started it: this compositor's own idle clock, System Settings' Test
// button, or a person at a shell prompt. Adopting it is what makes all
// three behave the same -- fullscreen, and gone on the first key --
// rather than the last two leaving a saver on screen that nothing
// dismisses, which is a way to lose the machine.
//
// The compositor learns the program from the client's SPAWN PATH
// (QUERY_PROCPATH), never from anything the client said, so nothing can
// claim to be a screensaver by naming itself one.
void wm_idle_adopt_saver(int pid);

// Is `pid` a live process spawned from /bin/wm/savers? What recognises a
// saver's window (wm_client.c) and what tells an ADOPTED one is still
// running -- it is not this compositor's child, so waitpid() cannot.
int wm_pid_is_screensaver(int pid);

// START AND STOP IT NOW, for `gui idle start|stop`. The shortest
// timeout a person can configure is one minute, so a test that waited
// for the clock would cost a minute a check -- these skip the WAIT and
// nothing else. The clock is still what `gui idle` reports, and still
// what starts a saver on a real machine.
int wm_idle_force_start(void);
void wm_idle_force_stop(void);

#endif
