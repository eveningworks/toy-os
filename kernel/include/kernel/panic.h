#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

// WHAT A KERNEL PANIC DOES AFTER IT HAS REPORTED, shared by every site
// that used to end in `cli; hlt` forever: keep the log in the RAM store
// (panic_store.h), then restart after `panic=<seconds>` -- Linux's
// panic=N, Windows' automatic restart. Default 10; `panic=0` halts
// forever as every panic used to; a negative value restarts at once. A
// key on the PS/2 keyboard restarts early.
//
// Call it with everything REPORTED: whatever is logged after this point
// is not in the record.
void panic_finish(void) __attribute__((noreturn));

// `panic=`'s value, parsed: seconds, 0 = halt, < 0 = at once. A value
// that is not a number is REJECTED (returns 0) and the default stands.
#define PANIC_DEFAULT_SECS 10
int panic_parse_secs(const char *value, int *out);

// The reset alone, with no filesystem flush and no USB quiesce: the
// panic path must not touch a subsystem that may be the one that failed.
// system_reboot() ends here too. Defined in power.c.
void power_reset_hardware(void) __attribute__((noreturn));

#endif
