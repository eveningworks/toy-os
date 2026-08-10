#!/bin/sh
# Bumps BUILD_NUMBER (repo root, plain text, one line -- just the
# integer) by a fixed amount depending on how big the change being
# built actually is. This is the piece the date-based scheme
# (tools/gen_version.sh, retired -- see CHANGELOG.md) couldn't do: a
# plain `make` invocation has no way to tell "fixed a typo" apart from
# "added a whole feature", so nothing about *this* number is automatic.
# Run it once, by hand (or by whoever/whatever is making the change),
# as part of finishing a real change -- not on every build.
#
# Usage: tools/bump_build.sh <fix|feature|major>
#   fix      +1   a bug fix, small tweak, or docs/cleanup change
#   feature  +10  a new command, app, or capability
#   major    +50  something that reshapes a subsystem
#
# These three tiers are a deliberately coarse approximation -- see
# CHANGELOG.md's entry on this scheme for the tradeoff (consistent and
# easy to sanity-check later, vs. a freeform number that's more nuanced
# but less predictable). Judgment call on which tier a change fits
# belongs in the CHANGELOG entry alongside the new number, e.g.
# "Build 110 (feature, +10): ...".
#
# kernel/include/version.h itself is NOT touched here -- it's still
# regenerated automatically from whatever BUILD_NUMBER currently holds
# by tools/gen_version.sh, which the Makefile's `version` target runs
# on every `make all`/`make iso`. Run this script first, then build.
set -e
cd "$(dirname "$0")/.."

TIER="$1"
case "$TIER" in
    fix)     DELTA=1  ;;
    feature) DELTA=10 ;;
    major)   DELTA=50 ;;
    *)
        echo "usage: $0 <fix|feature|major>" >&2
        echo "  fix      +1   bug fix, small tweak, docs/cleanup" >&2
        echo "  feature  +10  new command, app, or capability" >&2
        echo "  major    +50  reshapes a subsystem" >&2
        exit 1
        ;;
esac

COUNTER_FILE="BUILD_NUMBER"
CURRENT=0
if [ -f "$COUNTER_FILE" ]; then
    CURRENT=$(cat "$COUNTER_FILE")
fi

NEW=$((CURRENT + DELTA))
echo "$NEW" > "$COUNTER_FILE"
echo "build: $CURRENT -> $NEW ($TIER, +$DELTA)"
