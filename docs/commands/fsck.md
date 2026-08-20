# fsck

**a shell builtin.**

## Synopsis

    fsck [repair]

## Description

Walks every file's block tree against the free-block bitmap. On TFS3 it also verifies inode checksums, link counts and `.`/`..`, reclaims orphans, and on `repair` restores a damaged primary superblock from its backups. Read-only unless `repair` is passed.
