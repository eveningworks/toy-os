# spawn

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    spawn <path> [args...]    (typing the name instead waits for it)

## Description

`/bin/spawn` — start a program and do **not** wait; the child reparents to init and survives. What `nohup`/`setsid` are for elsewhere. It also matters for correctness: the legacy `run` loader has no scheduler slot, so `SYS_SLEEP` fails under it and anything that paces itself misbehaves — a spawned program is a real scheduled process.