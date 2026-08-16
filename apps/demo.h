#ifndef DEMO_H
#define DEMO_H

// A SCRIPTED TOUR of the running system, for showing toy-os to somebody
// without typing at it.
//
// The script is a file on the OS's own filesystem (`/usr/wm/demo.script`
// by default), so changing the tour is editing a text file on the live
// image -- no rebuild, no C to touch. That is the whole reason it is a
// script rather than a hardcoded sequence: a demo exists to be adjusted
// five minutes before it is shown.
//
// It runs only when `demo` appears on the kernel command line (see
// grub-demo.cfg / `make demo-iso`). Nothing about an ordinary boot
// changes, and the script sitting on a normal disk does nothing.
//
// THE FORMAT: one step per line, `verb argument`, `#` comments and
// blank lines ignored.
//
//   say <text>        print a line at the console, as a narrator
//   sh <command>      run a shell command and show its output
//   wait <ms>         pause, so a human can read what just happened
//   gui               enter the desktop; every step after this one is
//                     performed from inside the window manager's loop
//   open <App>        open a Start-menu app by name
//   click <x> <y>     click at a screen position
//   key <hex>         inject one key (api/keyboard.h codes)
//   end               stop performing steps and leave it running
//
// The split at `gui` is not cosmetic. Before it, steps run at the
// physical console with the shell available; after it, the desktop owns
// the machine, so those steps are performed one per WM iteration by
// wm_run() calling demo_gui_tick(). Trying to drive the GUI from
// outside its loop is what makes a scripted desktop demo hang.

// Loads a script. Returns the number of steps read (0 if the file is
// missing or empty, which is not an error -- an ordinary boot has none).
int demo_load(const char *path);

// Was `demo` asked for on the kernel command line?
int demo_requested(void);

// Runs every step up to `gui`, at the console. Returns 1 if the script
// asked for the desktop, so the caller knows to start it.
int demo_run_cli(void);

// One GUI step if its delay has elapsed. Called from wm_run()'s loop;
// a no-op when no demo is running or the script is finished.
void demo_gui_tick(void);

#endif
