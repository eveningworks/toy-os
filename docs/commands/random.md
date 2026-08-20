# random

**a `/bin` program.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

    random [count]   (1..32 values, default 4)

## Description

`/bin/random` — **the entropy source first, then the values.** The numbers look equally random whatever produced them, so the source is the only part a reader can judge, and under QEMU it is usually TSC jitter (the weakest case), which it says. The source comes from `QUERY_RANDOM`, not `SYS_GETRANDOM` — that syscall deliberately refuses to report quality.