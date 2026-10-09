#!/bin/sh
# Regenerates kernel/include/api/version.h from VERSION (repo root, plain
# text, one line -- a semver-ish string like "0.1.0-dev" or "0.1.0"),
# run automatically as the first step of `make all`/`make iso` (see
# the Makefile). Not meant to be edited by hand -- kernel/include/
# version.h itself says so too -- but harmless to run directly if you
# just want to see what it'd produce.
#
# This script does NOT change VERSION itself -- it just embeds
# whatever VERSION currently holds. Changing VERSION is a separate,
# deliberate step (tools/set_version.sh), run only when you're
# starting a new round of dev work or cutting a real release -- not on
# every build, and not on every change either (unlike the retired
# per-change build-number scheme this replaced, see the git history and
# docs/decisions.md).
#
# Idempotent by design (only overwrites version.h if the content
# actually changed): this runs on literally every `make all`/`make
# iso`, and kernel/include/api/kapi.h includes version.h, so with the
# Makefile's -MMD/-MP header dependency tracking (see its top comment),
# an unconditional overwrite here would bump version.h's mtime on
# every single build and make every file that (transitively) includes
# kapi.h -- which is nearly everything -- look "out of date" and
# rebuild every time, defeating the entire point of that tracking. By
# only touching the file when VERSION actually changed, a build right
# after `set_version.sh` correctly recompiles everything that depends
# on version.h (same as before), but every other build in between
# leaves its mtime alone, same as any other untouched header.
set -e
cd "$(dirname "$0")/.."

VERSION_FILE="VERSION"
VERSION="0.0.0-dev"
if [ -f "$VERSION_FILE" ]; then
    VERSION=$(cat "$VERSION_FILE")
fi

# The commit the build came from, and whether the tree matched it.
#
# WHY THIS DOES NOT BREAK THE IDEMPOTENCE ABOVE: a commit id changes once
# per commit, and the dirty marker at most twice per working session, so
# version.h's mtime still moves only when something real changed. A build
# TIMESTAMP is the version of this idea to avoid -- it would differ on
# every single build and rebuild the whole tree every time, which is
# exactly what the comment above exists to prevent.
#
# `unknown` rather than an empty string when there is no git (a release
# tarball, a stripped CI checkout): an empty marker reads as a bug in
# this script, and a build that cannot say where it came from should say
# so.
BUILD_ID="unknown"
if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
    BUILD_ID=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
    if ! git diff --quiet HEAD 2>/dev/null; then
        BUILD_ID="$BUILD_ID-dirty"
    fi
fi

# WHERE THIS BUILD'S SOURCE IS -- shown by About and written to the
# image as /usr/share/licenses/SOURCE, so a copy handed on second-hand
# still says where its source lives (what the GPL calls passing the offer
# along, and an honest pointer under any licence). The repository is
# origin's when that is GitHub, so a fork's image names the fork; the
# full commit id goes in the link, since a short one can become ambiguous.
SOURCE_REPO="https://github.com/eveningworks/toy-os"
ORIGIN=$(git remote get-url origin 2>/dev/null || true)
case "$ORIGIN" in
    git@github.com:*)     SOURCE_REPO="https://github.com/${ORIGIN#git@github.com:}" ;;
    https://github.com/*) SOURCE_REPO="$ORIGIN" ;;
esac
SOURCE_REPO="${SOURCE_REPO%.git}"
SOURCE_URL="$SOURCE_REPO"
SOURCE_EXACT=0
case "$BUILD_ID" in
    unknown) ;;
    *-dirty) SOURCE_URL="$SOURCE_REPO/tree/$(git rev-parse HEAD)" ;;
    *)       SOURCE_URL="$SOURCE_REPO/tree/$(git rev-parse HEAD)"; SOURCE_EXACT=1 ;;
esac

# What the OS actually shows. The rule, and the reason for each half:
#
#   0.3.0-dev  ->  "0.3.0-dev (2034bb1)"
#       A dev build is not pinned by anything. The commit id is the only
#       way to know which one an ISO is, which is the whole point.
#   0.3.0      ->  "0.3.0", always, dirty tree or not.
#       A release IS pinned by its git tag, and the audience is people
#       running the OS, to whom "dirty" is meaningless jargon about a
#       repository they do not have.
#
# **The dirty-release case is a BUILD-time warning, not a display
# string.** Cutting a release from uncommitted changes is a real
# mistake, but the person who needs to hear about it is the one running
# the build, not the user reading an About window months later. So it is
# shouted here, where it can still be acted on, and the shipped string
# stays clean.
case "$VERSION" in
    *-dev) VERSION_FULL="$VERSION ($BUILD_ID)" ;;
    *)
        VERSION_FULL="$VERSION"
        case "$BUILD_ID" in
            *-dirty)
                echo "version: WARNING -- building RELEASE $VERSION from a DIRTY tree." >&2
                echo "version:   The image will claim to be $VERSION, but its source is not" >&2
                echo "version:   any commit. Commit (or stash) before cutting a release." >&2
                ;;
        esac
        ;;
esac

OUT="kernel/include/api/version.h"
TMP="$OUT.tmp.$$"

cat > "$TMP" << EOF
#ifndef VERSION_H
#define VERSION_H

// GENERATED by tools/gen_version.sh -- run automatically as the first
// step of \`make all\`/\`make iso\` (see the Makefile). Don't hand-edit
// this file, it gets overwritten on the very next build.
//
// TOYOS_VERSION is a semver-ish string ("0.1.0-dev" while in
// development, "0.1.0" once released -- see VERSION at the repo root
// and tools/set_version.sh). It's just whatever VERSION currently holds
// -- this script doesn't change it. See docs/decisions.md for why this
// replaced the earlier per-change build-number scheme.
#define TOYOS_VERSION "$VERSION"

// The commit this build came from, plus "-dirty" if the working tree
// did not match it -- i.e. if the source that produced this image
// exists nowhere in history. "unknown" when built without git.
#define TOYOS_BUILD_ID "$BUILD_ID"

// **What to display.** TOYOS_VERSION plus the build id on a dev build,
// the bare version on a release (its tag already pins it), and a loud
// "(dirty)" either way if the tree had uncommitted changes. Prefer this
// over TOYOS_VERSION anywhere a human reads the result; reach for the
// bare macros when something needs to PARSE the version.
#define TOYOS_VERSION_FULL "$VERSION_FULL"

// The source this build was made from (see tools/gen_version.sh), and
// whether it is EXACTLY that: 0 for a dirty tree, whose changes exist in
// no commit, or for a build made without git.
#define TOYOS_SOURCE_REPO "$SOURCE_REPO"
#define TOYOS_SOURCE_URL "$SOURCE_URL"
#define TOYOS_SOURCE_EXACT $SOURCE_EXACT

#endif
EOF

if [ -f "$OUT" ] && cmp -s "$TMP" "$OUT"; then
    rm -f "$TMP"
    echo "version: $VERSION_FULL (unchanged)"
else
    mv "$TMP" "$OUT"
    echo "version: $VERSION_FULL"
fi

# --- /usr/share/licenses/SOURCE: the same pointer, as a file on the image
mkdir -p build/gen
SRC_OUT="build/gen/SOURCE"
{
    echo "toy-os $VERSION_FULL"
    echo
    if [ "$BUILD_ID" = unknown ]; then
        echo "This build was made without git, so it cannot name its commit."
        echo "toy-os's source is at:"
        echo "  $SOURCE_URL"
    else
        echo "The source code this build was made from:"
        echo "  $SOURCE_URL"
        case "$VERSION" in
            *-dev) ;;
            *)  echo "and as one archive, beside this release's images:"
                echo "  $SOURCE_REPO/releases/tag/v$VERSION  (toy-os-$VERSION-source.tar.gz)" ;;
        esac
        if [ "$SOURCE_EXACT" = 0 ]; then
            echo
            echo "This build was made from uncommitted changes on top of that"
            echo "commit, so no published source matches it exactly."
        fi
    fi
    echo
    echo "toy-os's own code is under the licence in LICENSE at the top of the"
    echo "source tree; what is under another licence, and which, is listed"
    echo "there too and in this directory."
} > "$SRC_OUT.tmp.$$"
if [ -f "$SRC_OUT" ] && cmp -s "$SRC_OUT.tmp.$$" "$SRC_OUT"; then rm -f "$SRC_OUT.tmp.$$"
else mv "$SRC_OUT.tmp.$$" "$SRC_OUT"; fi

# --- build STAMP: the date AND time, isolated in ONE object -----------
#
# The DATE header below answers "yesterday's build?"; this answers
# "which of the three I built this afternoon?", which a day cannot.
#
# IT CHANGES ON EVERY BUILD, ON PURPOSE, and that is only affordable
# because exactly one translation unit includes it: kernel/core/kversion.c.
# So every build relinks one object rather than rebuilding the tree --
# which is the same trick Linux plays with init/version.c, and the
# reason the objection recorded below (a timestamp "would differ on
# every build and rebuild desktop.o every time") does not apply here.
#
# No idempotence check either: the whole point is that it moves.
# ANYTHING ELSE THAT INCLUDES THIS HEADER makes every build a full
# rebuild -- ask the kernel through QUERY_VERSION instead.
STAMP_OUT="kernel/include/api/build_stamp.h"
BUILD_STAMP=$(date "+%Y-%m-%d %H:%M:%S")

cat > "$STAMP_OUT" << EOF
#ifndef BUILD_STAMP_H
#define BUILD_STAMP_H

// GENERATED by tools/gen_version.sh on EVERY build. Don't hand-edit.
//
// When this kernel was compiled, to the second.
//
// **INCLUDED BY EXACTLY ONE FILE**, kernel/core/kversion.c. It changes
// on every build, so a second includer turns every build into a rebuild
// of whatever includes it -- and if that is a widely included header,
// of the whole tree. Ring 3 asks QUERY_VERSION; nothing else needs the
// macro.
#define TOYOS_BUILD_STAMP "$BUILD_STAMP"

#endif
EOF

# --- build DATE AND TIME, in its own header on purpose ----------------
#
# "Am I still running the build I just flashed?" is a real question and
# the version string cannot answer it: a whole day of dev builds share
# one commit-and-dirty marker.
#
# It is NOT in version.h, and that is the whole design. kapi.h includes
# version.h, so anything in there that moves rebuilds nearly the entire
# tree -- see this file's idempotence comment above, which names a build
# timestamp as precisely the thing to avoid. This header is included by
# THREE display sites (userland/wm/desktop.c's watermark and the two
# About programs), so a build relinks three small objects.
#
# TO THE MINUTE, not the day. A day cannot tell three builds of one
# afternoon apart, which is exactly the question asked on a machine
# being reflashed repeatedly -- and a session lost real time this week
# to two laptops running a kernel one flash behind. That costs the
# idempotence check below its effect on any day something is built,
# accepted deliberately: three objects is not the tree, and the check
# still spares a rebuild when nothing regenerates the header at all.
DATE_OUT="kernel/include/api/build_date.h"
DATE_TMP="$DATE_OUT.tmp.$$"
BUILD_DATE=$(date "+%Y-%m-%d %H:%M")

cat > "$DATE_TMP" << EOF
#ifndef BUILD_DATE_H
#define BUILD_DATE_H

// GENERATED by tools/gen_version.sh. Don't hand-edit.
//
// WHEN this image was built, to the minute, so "am I still running the
// build I just flashed?" has an answer on screen. In its own header
// rather than version.h so that changing it rebuilds the few objects
// that DISPLAY it instead of the tree -- see gen_version.sh.
//
// Include it ONLY where it is displayed. Pulling it into a widely
// included header would recreate exactly the problem it is shaped to
// avoid.
#define TOYOS_BUILD_DATE "$BUILD_DATE"

#endif
EOF

if [ -f "$DATE_OUT" ] && cmp -s "$DATE_TMP" "$DATE_OUT"; then
    rm -f "$DATE_TMP"
else
    mv "$DATE_TMP" "$DATE_OUT"
    echo "version: build date $BUILD_DATE"
fi
