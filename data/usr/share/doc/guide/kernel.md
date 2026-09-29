# The kernel

**Category:** Getting started

## Description

The kernel relocates itself at boot, so it is not running where it was
linked. The relocation table is generated at build time, and there is a
`nokaslr` boot word for the times that matters. W^X is applied, SMEP and
SMAP are on, and the panic handler prints enough to diagnose from a
pasted log: registers, a symbolised backtrace, and the faulting address.

Kernel stacks are sixteen kilobytes, with a guard page below them and a
canary inside them. Linux had this exact bug before 4.9, and the guard
page here exists because of that history rather than in spite of it.

Every syscall is a row in one table. Its handler lives with the
subsystem that owns it, and the row also carries what `strace` should
print, so adding a syscall is three edits with no registry to forget.

## See also

`dmesg`, `strace`, `lsmod`, `ktest`
