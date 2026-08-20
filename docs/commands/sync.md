# sync

**a `/bin` program.**

## Synopsis

    sync

## Description

Writes out anything the disk cache is still holding, and reports how many sectors it wrote — a `sync` that printed nothing would be indistinguishable from one that did nothing. Runs automatically at shutdown and reboot, and at the journal's barriers, so this is for "write it out NOW" rather than routine use. Says so loudly if a sector could not be written, since that data then exists in RAM only.
