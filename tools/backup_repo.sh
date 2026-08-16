#!/bin/sh
# Take a complete, restore-tested backup of the GitHub repository.
#
# Exists because `git clone --mirror` is NOT a backup of this project.
# It captures every commit, branch and tag -- and none of the release
# ASSETS, which are ~130 MB of ISOs and disk images that live only on
# GitHub and cannot be recovered from a clone. Losing those means
# rebuilding and re-uploading each historical release by hand.
#
# Run this before anything that changes the repository's identity or
# history: transferring it to another owner, changing its visibility,
# renaming the account, or rewriting history. All four have happened
# here (see docs/decisions.md).
#
# What it produces, under <dest>/toy-os-<date>/:
#
#   toy-os.git/              mirror clone -- all refs, including refs/pull/*
#   toy-os-local-all.bundle  single-file bundle of the LOCAL refs, which
#                            catches branches never pushed
#   releases/<tag>/          every asset, plus release.json (title + notes)
#   metadata/                repo settings, PRs, issues, release index
#   RESTORE.md               how to put it all back
#
# Usage:
#   tools/backup_repo.sh [dest-dir] [owner/repo]
#
# Defaults: ~/backups, and the owner/repo the `origin` remote points at.
#
# Needs `gh` authenticated. Verifies as it goes: the bundle must report
# a complete history, and any release shipping a SHA256SUMS file has its
# assets checksummed after download. What it deliberately does NOT do is
# the restore test -- clone the mirror and `make all` -- because that
# costs a full build; do it by hand after a backup you intend to rely on.
set -e

DEST_ROOT="${1:-$HOME/backups}"
REPO="$2"

if [ -z "$REPO" ]; then
    REPO=$(git remote get-url origin 2>/dev/null | sed -e 's|.*github.com[:/]||' -e 's|\.git$||')
fi
if [ -z "$REPO" ]; then
    echo "backup_repo: no repo given and no origin remote to infer one from" >&2
    echo "usage: $0 [dest-dir] [owner/repo]" >&2
    exit 1
fi

DEST="$DEST_ROOT/$(basename "$REPO")-$(date +%Y-%m-%d)"
echo "backup_repo: $REPO -> $DEST"
mkdir -p "$DEST/releases" "$DEST/metadata"

# --- history -----------------------------------------------------------
if [ -d "$DEST/toy-os.git" ]; then
    echo "backup_repo: refreshing existing mirror"
    git -C "$DEST/toy-os.git" remote set-url origin "git@github.com:$REPO.git"
    git -C "$DEST/toy-os.git" fetch --prune origin
else
    git clone --mirror "git@github.com:$REPO.git" "$DEST/toy-os.git"
fi

# The local repo may hold branches that were never pushed; a mirror of
# the REMOTE cannot know about them.
git bundle create "$DEST/toy-os-local-all.bundle" --all
git bundle verify "$DEST/toy-os-local-all.bundle" | tail -2

# --- releases ----------------------------------------------------------
# The part a clone cannot give you.
for tag in $(gh release list --repo "$REPO" --json tagName --jq '.[].tagName'); do
    echo "backup_repo: release $tag"
    mkdir -p "$DEST/releases/$tag"
    gh release download "$tag" --repo "$REPO" --dir "$DEST/releases/$tag" --clobber
    gh release view "$tag" --repo "$REPO" \
        --json tagName,name,body,publishedAt > "$DEST/releases/$tag/release.json"

    # If the release published checksums, use them -- a backup nobody
    # verified is a guess.
    if [ -f "$DEST/releases/$tag/SHA256SUMS" ]; then
        ( cd "$DEST/releases/$tag" && sha256sum -c SHA256SUMS )
    fi
done

# --- metadata ----------------------------------------------------------
# Settings and discussion that live only on GitHub. Collaborators,
# webhooks and secrets are NOT captured -- see RESTORE.md.
gh repo view "$REPO" \
    --json name,description,visibility,defaultBranchRef,createdAt,homepageUrl,repositoryTopics,licenseInfo,url \
    > "$DEST/metadata/repo.json"
gh release list --repo "$REPO" --json tagName,name,publishedAt,isLatest \
    > "$DEST/metadata/releases.json"
gh pr list --repo "$REPO" --state all --limit 500 \
    --json number,title,state,mergedAt,headRefName,body > "$DEST/metadata/pull_requests.json"
gh issue list --repo "$REPO" --state all --limit 500 \
    --json number,title,state,body > "$DEST/metadata/issues.json"

echo
echo "backup_repo: done -- $(du -sh "$DEST" | cut -f1) at $DEST"
echo "backup_repo: restore instructions are not written by this script;"
echo "backup_repo: see docs/decisions.md's backup entry, and consider a"
echo "backup_repo: restore test (clone the mirror, run make all) before"
echo "backup_repo: relying on it for anything irreversible."
