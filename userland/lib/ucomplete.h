#ifndef UCOMPLETE_H
#define UCOMPLETE_H

// `/bin/tosh`'s completion ENVIRONMENT -- the ring-3 half of
// api/completion.h, whose engine is kernel/lib/completion.c compiled
// into libuapp.a.
//
// It is small on purpose. The kernel shell's env carries argument sets
// for a dozen commands that only exist at a `#` prompt (`color`,
// `debug`, `fontface`, `timezone`); tosh has six builtins and everything
// else is a program in PATH, so the only command-specific rule here is
// the one whose absence is felt immediately: `cd` offers DIRECTORIES
// ONLY, as bash and zsh do.

#include "completion.h"

// The env tosh hands to completion_run_env(). One instance, since a
// process has one current directory and one PATH.
const struct completion_env *ucomplete_env(void);

#endif
