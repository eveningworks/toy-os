# kstack

**a `/bin` program.**

**Category:** System information

## Synopsis

    kstack | kstack slots | kstack syscalls | kstack track [on|off]

## Description

syscalls] [track on\ | off]` | `/bin/kstack`, over `QUERY_KSTACK` (per stack) and `QUERY_KSTACK_SYSCALL` (per syscall, and legitimately EMPTY while tracking is off). `used` is a HIGH-WATER MARK — how deep a stack has ever been, not how deep it is now. `slots` is the frame view: what each stack would resume into.