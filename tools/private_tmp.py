#!/usr/bin/env python3
"""A private TMPDIR for one test tool, deleted when it returns.

systemd's PrivateTmp=, for a runner. Tools copy a disk image or keep a
directory of screenshots under tempfile.mkdtemp() and mostly never
remove it; /tmp is a RAM-backed tmpfs here, and the leftovers of many
runs filled it -- which ended an on-demand sweep at tool 83 of 91 with
ENOSPC. A runner hands each tool its own TMPDIR (tempfile and QEMU both
honour it) and removes the whole directory after, whatever the tool did.

A tool's OUTPUT is the evidence a runner keeps; anything a tool means to
outlive the run must go to a path its caller names (`--logs`, `--shot`).
"""

import contextlib
import os
import shutil
import tempfile


@contextlib.contextmanager
def private_tmp(name, base_env=None):
    """Yields an environment whose TMPDIR is a fresh directory for `name`."""
    d = tempfile.mkdtemp(prefix=f"toyos-{name}-")
    env = dict(base_env if base_env is not None else os.environ)
    env["TMPDIR"] = d
    try:
        yield env
    finally:
        shutil.rmtree(d, ignore_errors=True)
