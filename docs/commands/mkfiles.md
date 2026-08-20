# mkfiles

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    mkfiles [--verify] <dir> <count> [size | min-max]

## Description

min-max]` | `/bin/mkfiles` — fills a directory to test the filesystem at scale, reporting the rate per 250 files so a slowdown as the directory grows is visible. Content is derived from (index, offset) so `--verify` proves each file holds its own bytes; a constant fill could not tell two files sharing a block apart.