# Processes

**Category:** Getting started

## Description

There is an init, it holds pid 1, and it cannot be killed. It reads a
boot target and starts the services described in a directory of files,
in an order those files declare. A service can say when it is genuinely
ready rather than merely spawned, which is what lets one service wait
for another to become usable rather than merely to exist.

A process has a parent, orphans are reparented, and a dying process is
reaped by whoever is waiting for it. Signals exist, process groups
exist, and job control works: a job is a process group, and the job
table belongs to the shell rather than to the kernel, because nothing
in the kernel would be improved by learning what a job is.

## See also

`ps`, `kill`, `spawn`, `service`, `jobs`
