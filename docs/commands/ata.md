# ata

**a `/bin` program.**

**Category:** System information

## Synopsis

    ata | ata nodma [on|off]

## Description

`/bin/ata`, over `QUERY_ATA`. **Three states, not two** — "no Bus-Master DMA on this controller" and "DMA available but forced off" look identical from a throughput number and mean different things. The forcing is the `kernel.ata_nodma` tunable, and it can legitimately refuse while a non-blocking transfer is in flight.

With DMA on it also prints **`waits that slept`**: how many DMA waits parked their caller so something else could run. Only a scheduled process sleeps. The kernel context and a legacy `run` still poll, so a count that stays at 0 while a program writes means the sleep path is not being reached (`docs/blocking-design.md`).
