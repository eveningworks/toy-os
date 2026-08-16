# Startup entries -- what the desktop launches at login

Same format as `../desktop/`, seeded to `/usr/wm/startup/`. Every entry
here is launched once when the desktop starts, in filename order.

Empty on purpose right now: nothing in toy-os wants to auto-start yet,
and an entry added "as an example" would be a process every boot pays
for. The directory exists so that adding one is dropping a file rather
than finding somewhere in `wm.c` to hardcode it -- which is the same
argument the `desktop/` directory makes about the Start menu.

A future service supervisor (docs/roadmap.md's Milestone 13) is the
system-wide version of this idea and should not be confused with it:
this one is per-desktop-session and starts GUI programs.
