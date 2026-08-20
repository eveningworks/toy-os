# schedtest

**a shell builtin.**

## Synopsis

    schedtest

## Description

Runs two ring-3 processes concurrently, neither ever yielding, and
returns once both exit -- so the interleaving in the output is the
preemptive round-robin scheduler doing its job.

Like `ring3test`, it stays a builtin because it is the mechanism under
test rather than a user of it.
