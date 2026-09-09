#ifndef DIAG_H
#define DIAG_H

#include "diag_abi.h"

// The diagnostic registry: named ring-3 services the kernel can ask a
// question on behalf of either ring. See abi/diag_abi.h for the protocol
// and why the name table is the kernel's.

// Handles one message against `pid`. `msg` is a KERNEL copy -- never the
// caller's own page, which it could change between validation and use.
//
// Returns 1 on success, 0 when refused (no such provider, not the
// provider, nothing pending), and -EBUSY when another caller holds the
// channel mid-drain.
int diag_request(int pid, struct diag_msg *msg);

// `pid` died: drop any name it held, and fail a drain it was answering.
// Called from the one place a process's kernel state is torn down.
void diag_provider_gone(int pid);

// Whether anything has claimed `name`. For the console, which would
// otherwise report "no answer" identically for a wedged provider and one
// that was never running.
int diag_have_provider(const char *name);

// The registered names, for `diag` with no arguments. Writes at most
// `cap` bytes including the NUL and returns how many providers there
// are, which may exceed what fitted.
int diag_list(char *out, int cap);

#endif
