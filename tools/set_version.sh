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
#       release). Just rewrites VERSION; CHANGELOG.md is untouched.
#
#   tools/set_version.sh 0.2.0
#       Cutting a real release. Rewrites VERSION AND stamps
#       CHANGELOG.md: the current "## [Unreleased]" heading is renamed
#       to "## [0.2.0] - <today's date>", with a fresh empty one above.
#       NOTE: CHANGELOG.md is closed (see CLAUDE.md) -- no new entries
#       are written to it, so that stamp only labels the backlog that
#       accumulated before the freeze, and is a no-op afterwards.
#       Release notes come from `git log`. Doesn't touch git at all --
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
    echo "  tools/set_version.sh 0.2.0       cut a release (VERSION + CHANGELOG.md stamp)" >&2
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
        echo "version: $OLD -> $NEW (dev round started, CHANGELOG.md untouched)"
        ;;
    *)
        # CHANGELOG.md is CLOSED (see CLAUDE.md's delivery section) --
        # no new entries are written to it. This stamp is kept because
        # its "## [Unreleased]" section still holds everything that
        # accumulated before it was frozen, and that backlog deserves a
        # version heading when the release is finally cut. Once that has
        # happened the section stays empty and this becomes a harmless
        # no-op; release notes come from `git log` instead, where every
        # commit body already lists its changed files.
        CHANGELOG="CHANGELOG.md"
        if [ ! -f "$CHANGELOG" ]; then
            echo "version: $OLD -> $NEW (no CHANGELOG.md found, skipped the stamp)"
            exit 0
        fi
        if ! grep -q '^## \[Unreleased\]$' "$CHANGELOG"; then
            echo "warning: no '## [Unreleased]' heading found in $CHANGELOG -- VERSION was still updated, but you'll need to add the release section by hand." >&2
            exit 0
        fi
        TODAY=$(date +%Y-%m-%d)
        TMP="$CHANGELOG.tmp.$$"
        awk -v new="$NEW" -v today="$TODAY" '
            /^## \[Unreleased\]$/ && !done {
                print "## [Unreleased]"
                print ""
                print "## [" new "] - " today
                done = 1
                next
            }
            { print }
        ' "$CHANGELOG" > "$TMP"
        mv "$TMP" "$CHANGELOG"
        echo "version: $OLD -> $NEW (released)"
        echo "CHANGELOG.md: \"## [Unreleased]\" stamped as \"## [$NEW] - $TODAY\", fresh Unreleased section added above it"
        echo "next: write the release notes from 'git log' (CHANGELOG.md is frozen -- see CLAUDE.md),"
        echo "      then 'git tag v$NEW && git push origin main --tags' when ready"
        ;;
esac
