#ifndef KSIGNAL_H
#define KSIGNAL_H

// Signal NAMES: the string half of abi/signal_abi.h's numbers.
//
// Its own header, and compiled into both rings (kernel/lib/ksignal.c is
// on the shared-source list beside geom.c and klineedit.c), because both
// rings need the same table and there is no reason for two. The kernel
// prints a name when it terminates a process; `/bin/kill` parses one off
// a command line. A second table in userland would be a second thing to
// keep in step, and the first symptom of it drifting would be `kill
// -TERM` sending something else.
//
// The POLICY -- what each signal does, and when -- is kernel/signal.h,
// which ring 3 may not include and does not need to.

// The bare name of `sig` ("INT", "TERM"), with no SIG prefix. NEVER
// NULL: an unrecognised number comes back as "?" rather than being left
// to a caller to handle, the same convention sched_wait_reason_name()
// follows.
const char *signal_name(int sig);

// The signal `name` denotes, or 0 if it denotes none.
//
// Accepts "TERM", "SIGTERM", "term", "sigterm" and "15" alike, so no
// caller has to normalise first -- `kill -9` and `kill -SIGKILL` are
// both things people type, and a program that handled only one of them
// would be wrong in a way only discovered mid-emergency.
//
// REJECTS rather than guesses (CLAUDE.md): "TERMINATE" and "9x" are 0,
// not SIGTERM and SIGKILL. The argument names something to destroy.
int signal_from_name(const char *name);

#endif
