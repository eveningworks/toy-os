# reboot

**a `/bin` program.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

    reboot [--poweroff]

## Description

`/bin/reboot` — one program for both, with the destructive one not the default. It does not sync: the kernel flushes on the way down, and a second place that has to remember is the one that gets forgotten.