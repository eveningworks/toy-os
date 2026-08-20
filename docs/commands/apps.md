# apps

**a shell builtin.**

**Category:** Processes and programs

## Synopsis

    apps

## Description

Lists the console apps registered in `apps/apps.c` -- kernel-space
programs, as distinct from the ELF binaries in `/bin`.

The list is short and getting shorter: `Exec=builtin:` is gone and ring
0 contains no applications, so what remains here are the in-kernel
demos that cannot be processes (see `ring3test`, `schedtest`).