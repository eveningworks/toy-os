#!/bin/sh
# Sets VERSION (repo root, plain text, one line) to a new value. This
# is the ONLY sanctioned way to change VERSION -- don't hand-edit it.
# Replaces tools/bump_build.sh (retired) now that toy-os uses semantic
# versioning with a "-dev" suffix during development instead of a
# build number bumped on every change -- see docs/decisions.md for the
# full reasoning behind the switch.
#
# Two different things this script is used for, told apart by whether
# the new version ends in "-dev":
#
#   tools/set_version.sh 0.2.0-dev
#       Starting a new round of dev work (typically right after a
#       release). Rewrites VERSION.
#
#   tools/set_version.sh 0.2.0
#       Cutting a real release. Also just rewrites VERSION: it used to
#       stamp CHANGELOG.md's "## [Unreleased]" heading with the version
#       and date, and that file was deleted on 2026-08-18 (see
#       CLAUDE.md). Release notes come from `git log`, where every
#       commit body already lists its changed files, and
#       docs/release-notes-template.md gives the shape.
#       Doesn't touch git at all --
#       tagging (`git tag v0.2.0`) and pushing are still separate,
#       deliberate steps you run yourself (or via tools/device_git.sh),
#       same as before.
#
# Run this once, by hand, as part of finishing a change or cutting a
# release -- not on every build. kernel/include/api/version.h itself is
# NOT touched here -- it's still regenerated automatically from
# whatever VERSION currently holds by tools/gen_version.sh, which the
# Makefile's `version` target runs on every `make all`/`make iso`. Run
# this script first, then build.
set -e
cd "$(dirname "$0")/.."

NEW="$1"
if [ -z "$NEW" ]; then
    echo "usage: $0 <new-version>" >&2
    echo "  tools/set_version.sh 0.2.0-dev   start a new dev round (VERSION only)" >&2
    echo "  tools/set_version.sh 0.2.0       cut a release (VERSION only)" >&2
    exit 1
fi

VERSION_FILE="VERSION"
OLD="(none)"
if [ -f "$VERSION_FILE" ]; then
    OLD=$(cat "$VERSION_FILE")
fi

echo "$NEW" > "$VERSION_FILE"

case "$NEW" in
    *-dev)
        echo "version: $OLD -> $NEW (dev round started)"
        ;;
    *)
        echo "version: $OLD -> $NEW (released)"
        echo "next: write the release notes from 'git log' -- see"
        echo "      docs/release-notes-template.md for the shape,"
        echo "      then 'git tag v$NEW && git push origin main --tags' when ready"
        ;;
esac
