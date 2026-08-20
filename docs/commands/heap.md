# heap

**a `/bin` program.**

## Synopsis

    heap | heap debug [on|off] | heap check

## Description

Kernel heap stats. `heap debug on\ | off` red-zones new allocations and poisons freed ones; `heap check` sweeps for a use-after-free.
