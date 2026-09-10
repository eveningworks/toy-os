# lsmod

**a `/bin` program.**

**Category:** System information

## Synopsis

```
lsmod [-v]
```

## Options

- `-v` -- print each module's load address and the split between text
  and data, and how many drivers it declares, instead of what it holds.

## Description

Lists the kernel modules loaded right now, with the memory each takes
and what it is holding.

```
/$ lsmod
MODULE             SIZE  USED BY
e1000               8 K  1 device(s)
hello               8 K  -
```

A module holding a device it cannot release -- a driver with no
`remove()` -- shows `(cannot unload)`, which is what `modunload` will
say too. `pinned N` is a module that pinned itself (`module_get()`),
refused the same way until it lets go.

The question `lsdrv` answers is a different one: which drivers this
build has and what each bound, for modules and built-in drivers alike.
A module's drivers appear there the moment it loads and leave when it
unloads; this lists the modules themselves.

## See also

[`lsdrv`](lsdrv.md), [`modload`](modload.md), [`modunload`](modunload.md).
