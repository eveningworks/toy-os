#ifndef RAMMETER_H
#define RAMMETER_H

// A live physical-frame + kernel-heap readout in the top-right corner,
// enabled with `rammeter` on the GRUB command line. Debug instrument, not
// a UI element -- see rammeter.c for why it is drawn as an uncomposited
// overlay and what that rules out.

// Reads the command line and decides whether the meter is on. Call once
// from kernel_main(), after multiboot parsing.
void rammeter_init(void);

int  rammeter_enabled(void);
void rammeter_set_enabled(int on);

// Redraws at most once a second, and does nothing when disabled. Safe to
// call from any drawing path as often as you like -- the rate limit is
// the point, since redrawing costs framebuffer traffic and that is the
// scarce resource on real hardware.
void rammeter_tick(void);

// Redraws unconditionally, ignoring both the rate limit and the clock.
// For the shell's `rammeter` command, so toggling it on shows something
// immediately instead of after a wait.
void rammeter_draw(void);

#endif
