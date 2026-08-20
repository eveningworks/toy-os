# parttable

**a `/bin` program.**

## Synopsis

    parttable

## Description

`/bin/parttable` — the disk's MBR/GPT table, read-only. This repo's stock `disk.img` has **no** table (one raw filesystem volume), so "none" is the normal answer. Two query classes back it, because a list alone cannot tell "a table with no partitions" from "no table at all".
