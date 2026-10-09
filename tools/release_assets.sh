#!/bin/sh
# The assets of a GitHub Release, made only from a build that IS the tag.
#
#   make clean-disk && make live-iso usb-image DEBUGCON=0
#   sh tools/release_assets.sh 0.5.0            # -> build/release/0.5.0/
#   gh release create v0.5.0 build/release/0.5.0/* --title ... --notes ...
#
# REFUSES, before writing anything, unless every one of these holds --
# because a release's images must be built from exactly the source it
# offers, and each of these is a way they silently were not:
#
#   - VERSION says <version>              (set_version.sh was run)
#   - the tree is clean, UNTRACKED FILES INCLUDED -- every .c under
#     kernel/ and userland/ compiles, tracked or not, and gen_version.sh's
#     "-dirty" only sees tracked changes
#   - tag v<version> exists and is HEAD
#   - origin has that tag, at the same commit (it was pushed, and not
#     re-pointed since)
#   - version.h was generated from HEAD, and both images are newer than
#     it: gen_version.sh rewrites version.h whenever the commit changes,
#     so an image older than it was built from some earlier commit
#
# THE SOURCE ARCHIVE travels beside the images as an asset of its own
# (`git archive` of the tag). GitHub's automatic "Source code" links
# follow the tag and the repository, and this one does not: it stays the
# source of these exact images whatever later happens to the history. It
# is in SHA256SUMS with everything else, and gzip -n keeps it byte-stable
# for one tag.
set -eu
cd "$(dirname "$0")/.."

[ $# -ge 1 ] || { echo "usage: $0 <version> [outdir]" >&2; exit 2; }
VER="$1"
TAG="v$VER"
OUT="${2:-build/release/$VER}"
LIVE="toy-os-live.iso"
USB="toyos-usb.img"

die() { echo "release_assets: REFUSING -- $*" >&2; exit 1; }

[ "$(cat VERSION)" = "$VER" ] ||
    die "VERSION says $(cat VERSION), not $VER (tools/set_version.sh $VER first)"
[ -z "$(git status --porcelain)" ] ||
    die "the tree has uncommitted or untracked files (git status) -- they would be in the images and not in the source"
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null || die "there is no tag $TAG"
HEAD=$(git rev-parse HEAD)
[ "$(git rev-parse "$TAG^{commit}")" = "$HEAD" ] || die "$TAG is not HEAD"
REMOTE=$(git ls-remote origin "refs/tags/$TAG^{}" | cut -f1)
[ -n "$REMOTE" ] || die "origin has no $TAG (git push origin $TAG)"
[ "$REMOTE" = "$HEAD" ] || die "origin's $TAG is $REMOTE, not HEAD"
BUILT=$(sed -n 's/^#define TOYOS_BUILD_ID "\(.*\)"$/\1/p' kernel/include/api/version.h)
[ "$BUILT" = "$(git rev-parse --short HEAD)" ] ||
    die "the build is of $BUILT, not of HEAD ($(git rev-parse --short HEAD)) -- rebuild"
for f in "$LIVE" "$USB"; do
    [ -f "$f" ] || die "$f is missing (make live-iso usb-image DEBUGCON=0)"
    [ "$f" -nt kernel/include/api/version.h ] ||
        die "$f is older than this commit's build -- rebuild it"
done

mkdir -p "$OUT"
rm -f "$OUT"/*
git archive --format=tar --prefix="toy-os-$VER/" "$TAG" | gzip -9n > "$OUT/toy-os-$VER-source.tar.gz"
cp "$LIVE" "$OUT/"
gzip -9nc "$USB" > "$OUT/$USB.gz"
cp tools/run_release.sh "$OUT/"
(cd "$OUT" && sha256sum -- * > SHA256SUMS)

echo "release_assets: $OUT, all from $TAG ($HEAD):"
(cd "$OUT" && ls -l)
echo "release_assets: gh release create $TAG $OUT/* --title ... --notes ..."
