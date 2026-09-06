// Tests for the mount table (kernel/fs/mount.c).
//
// THESE RUN AGAINST THE LIVE TABLE, unlike fat32_test.c's, and that is
// deliberate: the thing worth asserting is path RESOLUTION across real
// mounts, and a synthetic table would be testing a copy of the rules
// rather than the rules. What makes it safe is that every test mounts
// ramfs at /mnt -- a filesystem with no volume, on a mount point the
// layout pass creates and nothing else uses -- and unmounts it again.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: "a file written under /mnt
// reads back" is satisfied by a table that ignores /mnt entirely and
// sends everything to the root. So the checks that discriminate are the
// ones about the BOUNDARY -- that /mnt and /mnt-sibling are different
// filesystems, that the backend is handed a path with the mount point
// stripped, and that a file is invisible from the other side.
#include "ktest.h"
#include "mount.h"
#include "fs.h"
#include "string.h"
#include "tmppath.h"
#include "kfmt.h"

// A mount point NEXT TO /mnt whose name starts with the same letters.
// This is rule 1's whole point: a prefix match that does not stop at a
// component boundary hands "/mnt-sibling/x" to the filesystem mounted
// at "/mnt".
#define SIBLING "/mnt-sibling"

static int mnt_ramfs(const char *point) {
    const char *why = "";
    return mount_add(0, "ramfs", point, 0, 0, &why);
}

static const char *second_mount_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "second");
    return p;
}
#define SECOND_MOUNT second_mount_path()

// The mount point's own children. A path built at runtime cannot be
// concatenated with a literal, so the suffix is an argument.
static const char *second_under(const char *rel) {
    static char p[FS_PATH_MAX];
    char base[FS_PATH_MAX];
    k_snprintf(base, sizeof base, "second%s", rel);
    if (!tmppath(p, sizeof p, TMP_PERSISTENT, base)) p[0] = '\0';
    return p;
}

static const char *crossmount_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "crossmount.txt");
    return p;
}
#define CROSSMOUNT crossmount_path()

KTEST("mount", "the root answers for everything nothing else claims") {
    const struct mount *root = mount_root();
    KTEST_ASSERT(root != 0);
    KTEST_ASSERT_EQ(root->point_len, 1);

    char sub[64];
    const struct mount *m = mount_resolve("/etc/toyos.conf", sub, sizeof sub);
    KTEST_ASSERT(m == root);
    // The root's sub-path is the path UNCHANGED -- stripping a "/" off
    // it would hand every backend a relative path.
    KTEST_ASSERT_EQ(k_strcmp(sub, "/etc/toyos.conf"), 0);

    m = mount_resolve("/", sub, sizeof sub);
    KTEST_ASSERT(m == root);
    KTEST_ASSERT_EQ(k_strcmp(sub, "/"), 0);
}

KTEST("mount", "a mount claims its subtree, and the backend sees a root-relative path") {
    if (!mnt_ramfs("/mnt")) { KTEST_SKIP("could not mount ramfs at /mnt"); }

    char sub[64];
    const struct mount *m = mount_resolve("/mnt/a/b.txt", sub, sizeof sub);
    KTEST_ASSERT(m != 0);
    KTEST_ASSERT_EQ(k_strcmp(m->fs->name, "ramfs"), 0);
    KTEST_ASSERT_EQ(k_strcmp(sub, "/a/b.txt"), 0);

    // The mount point ITSELF resolves to that filesystem's root, not to
    // an empty string -- a backend handed "" has no path at all.
    m = mount_resolve("/mnt", sub, sizeof sub);
    KTEST_ASSERT_EQ(k_strcmp(m->fs->name, "ramfs"), 0);
    KTEST_ASSERT_EQ(k_strcmp(sub, "/"), 0);

    const char *why = "";
    KTEST_ASSERT(mount_remove("/mnt", &why));
}

// RULE 1, and the reason `under()` is not a k_strncmp.
KTEST("mount", "a mount point does not claim a sibling that shares its prefix") {
    fs_mkdir(SIBLING);
    if (!fs_is_dir(SIBLING)) { KTEST_SKIP("could not create " SIBLING); }
    if (!mnt_ramfs("/mnt")) { fs_delete(SIBLING); KTEST_SKIP("could not mount ramfs at /mnt"); }

    char sub[64];
    const struct mount *sib = mount_resolve(SIBLING "/x", sub, sizeof sub);
    KTEST_ASSERT(sib == mount_root());
    // The path arrives at the ROOT unchanged, mount point and all.
    KTEST_ASSERT_EQ(k_strcmp(sub, SIBLING "/x"), 0);

    const char *why = "";
    KTEST_ASSERT(mount_remove("/mnt", &why));
    fs_delete(SIBLING);
}

// The end-to-end version: a file written under a mount must be on THAT
// filesystem, which is only observable from the other side of the
// boundary -- it disappears when the mount goes.
KTEST("mount", "a file written under a mount is that filesystem's, and goes with it") {
    if (!mnt_ramfs("/mnt")) { KTEST_SKIP("could not mount ramfs at /mnt"); }

    KTEST_ASSERT(fs_write("/mnt/ontheramdisk.txt", "hello", 0));
    KTEST_ASSERT(fs_exists("/mnt/ontheramdisk.txt"));
    KTEST_ASSERT_EQ((int)fs_size("/mnt/ontheramdisk.txt"), 5);

    const char *why = "";
    KTEST_ASSERT(mount_remove("/mnt", &why));
    // Gone, because it was never on the root at all. A table that
    // ignored the mount would still have the file here.
    KTEST_ASSERT(!fs_exists("/mnt/ontheramdisk.txt"));
}

KTEST("mount", "the refusals: the root, a missing point, a double mount, a busy one") {
    const char *why = "";

    KTEST_ASSERT(!mount_remove("/", &why));
    KTEST_ASSERT(!mount_remove("/nothing-is-mounted-here", &why));

    // A mount point that does not exist is refused -- Linux's rule, and
    // the reason `ensure_layout()` creates /boot and /mnt.
    KTEST_ASSERT(!mount_add(0, "ramfs", "/definitely-not-a-directory", 0, 0, &why));
    // ...and so is a relative one, and one with a trailing slash.
    KTEST_ASSERT(!mount_add(0, "ramfs", "mnt", 0, 0, &why));
    KTEST_ASSERT(!mount_add(0, "ramfs", "/mnt/", 0, 0, &why));
    // ...and an unknown filesystem type.
    KTEST_ASSERT(!mount_add(0, "notafilesystem", "/mnt", 0, 0, &why));

    if (!mnt_ramfs("/mnt")) { KTEST_SKIP("could not mount ramfs at /mnt"); }
    // Something is already there.
    KTEST_ASSERT(!mount_add(0, "ramfs", "/mnt", 0, 0, &why));
    KTEST_ASSERT(mount_remove("/mnt", &why));
}

// THE POINT OF PER-MOUNT STATE, asserted at the level a user meets it:
// two mounts of ONE backend are two filesystems. Before struct
// ramfs_state they shared one node table, so this would have found
// /tmp/second/b.txt under /mnt as well -- and the second mount was
// refused outright to stop exactly that.
KTEST("mount", "two mounts of one backend are two filesystems") {
    const char *why = "";
    if (!mnt_ramfs("/mnt")) { KTEST_SKIP("could not mount ramfs at /mnt"); }

    fs_mkdir(SECOND_MOUNT);
    if (!fs_is_dir(SECOND_MOUNT)) {
        mount_remove("/mnt", &why);
        KTEST_SKIP("could not make a second mount point");
    }
    if (!mount_add(0, "ramfs", SECOND_MOUNT, 0, 0, &why)) {
        mount_remove("/mnt", &why);
        fs_delete(SECOND_MOUNT);
        KTEST_SKIP("could not mount a second ramfs");
    }

    KTEST_ASSERT(fs_write("/mnt/a.txt", "first volume", 0));
    KTEST_ASSERT(fs_write(second_under("/b.txt"), "second volume, and longer", 0));

    // Neither can see the other's file...
    KTEST_ASSERT(fs_exists("/mnt/a.txt"));
    KTEST_ASSERT(!fs_exists("/mnt/b.txt"));
    KTEST_ASSERT(fs_exists(second_under("/b.txt")));
    KTEST_ASSERT(!fs_exists(second_under("/a.txt")));
    // ...and the sizes differ, so a single shared table serving both
    // could not pass by coincidence.
    KTEST_ASSERT_EQ((int)fs_size("/mnt/a.txt"), 12);
    KTEST_ASSERT_EQ((int)fs_size(second_under("/b.txt")), 25);

    // Unmounting one leaves the other whole -- state_free() frees a
    // mount's own tree and nobody else's.
    KTEST_ASSERT(mount_remove(SECOND_MOUNT, &why));
    KTEST_ASSERT(fs_exists("/mnt/a.txt"));
    KTEST_ASSERT_EQ((int)fs_size("/mnt/a.txt"), 12);

    KTEST_ASSERT(mount_remove("/mnt", &why));
    fs_delete(SECOND_MOUNT);
}

KTEST("mount", "a read-only mount refuses every mutating call") {
    const char *why = "";
    if (!mount_add(0, "ramfs", "/mnt", MNT_RDONLY, 0, &why)) {
        KTEST_SKIP("could not mount ramfs read-only at /mnt");
    }
    KTEST_ASSERT(!fs_write("/mnt/nope.txt", "x", 0));
    KTEST_ASSERT(!fs_touch("/mnt/nope.txt"));
    KTEST_ASSERT(!fs_mkdir("/mnt/nope"));
    KTEST_ASSERT(!fs_delete("/mnt/nope.txt"));
    KTEST_ASSERT(!fs_truncate("/mnt/nope.txt", 0));
    // A READ still works, which is what makes it read-only rather than
    // unmounted.
    KTEST_ASSERT(fs_is_dir("/mnt"));
    KTEST_ASSERT(mount_remove("/mnt", &why));
}

// RULE 4: an operation naming two paths that land on different mounts is
// refused, not half-done. Unix's EXDEV.
KTEST("mount", "rename across a mount boundary is refused") {
    if (!mnt_ramfs("/mnt")) { KTEST_SKIP("could not mount ramfs at /mnt"); }

    KTEST_ASSERT(fs_write(CROSSMOUNT, "here", 0));
    KTEST_ASSERT(!fs_rename(CROSSMOUNT, "/mnt/crossmount.txt"));
    // Refused means UNCHANGED, not moved-and-failed.
    KTEST_ASSERT(fs_exists(CROSSMOUNT));
    KTEST_ASSERT(!fs_exists("/mnt/crossmount.txt"));

    fs_delete(CROSSMOUNT);
    const char *why = "";
    KTEST_ASSERT(mount_remove("/mnt", &why));
}

// RULE 5, which costs no code and is therefore the easiest to break by
// accident: every path reaching fs_* is already normalized, so `..`
// cannot walk out of a mount because it is gone before any mount is
// consulted.
KTEST("mount", "`..` cannot escape a mount root, because it never arrives") {
    if (!mnt_ramfs("/mnt")) { KTEST_SKIP("could not mount ramfs at /mnt"); }
    char sub[64];
    // A caller that DID pass ".." (against api/fs.h's contract) must not
    // be handed a path that leaves the mount -- it resolves inside it
    // and the backend rejects the component.
    const struct mount *m = mount_resolve("/mnt/../etc", sub, sizeof sub);
    KTEST_ASSERT_EQ(k_strcmp(m->fs->name, "ramfs"), 0);
    KTEST_ASSERT_EQ(k_strcmp(sub, "/../etc"), 0);
    KTEST_ASSERT(!fs_exists("/mnt/../etc"));

    const char *why = "";
    KTEST_ASSERT(mount_remove("/mnt", &why));
}

KTEST("mount", "df's per-mount records name every mount, root first") {
    if (!mnt_ramfs("/mnt")) { KTEST_SKIP("could not mount ramfs at /mnt"); }

    int n = mount_count();
    KTEST_ASSERT(n >= 2);
    const struct mount *first = mount_at(0);
    KTEST_ASSERT(first != 0);
    KTEST_ASSERT_EQ(first->point_len, 1); // the root is record 0, which /bin/df relies on

    int found = 0;
    for (int i = 0; i < n; i++) {
        const struct mount *m = mount_at(i);
        if (m && k_strcmp(m->point, "/mnt") == 0) found = 1;
    }
    KTEST_ASSERT(found);
    KTEST_ASSERT(mount_at(n) == 0); // past the end is NULL, not the last one again

    const char *why = "";
    KTEST_ASSERT(mount_remove("/mnt", &why));
}

// The size a mount was given must reach the backend. Worth its own test
// because the value crosses four layers to get there -- the ABI's
// size_mib, the syscall's unit conversion, mount_add() and fs_ops.init()
// -- and a drop anywhere in that chain looks exactly like "the default
// applied", which is a plausible number rather than a visible failure.
KTEST("mount", "a mount's size reaches the backend, and 0 means the default") {
    const char *why = "";
    uint64_t asked = 8 * 1024 * 1024;
    if (!mount_add(0, "ramfs", "/mnt", 0, asked, &why)) {
        KTEST_SKIP("could not mount a sized ramfs at /mnt");
    }
    uint64_t used = 0, total = 0;
    const struct mount *m = mount_resolve("/mnt", (char[64]){0}, 64);
    int got = m && fs_mount_usage(m, &used, &total);
    KTEST_ASSERT(mount_remove("/mnt", &why));
    KTEST_ASSERT(got);
    KTEST_ASSERT_EQ(total, asked);

    // ...and 0 is NOT "zero bytes". A backend that took the number
    // literally would mount a filesystem nothing could be written to.
    if (!mount_add(0, "ramfs", "/mnt", 0, 0, &why)) {
        KTEST_SKIP("could not mount a default-sized ramfs at /mnt");
    }
    uint64_t deflt = 0;
    m = mount_resolve("/mnt", (char[64]){0}, 64);
    got = m && fs_mount_usage(m, &used, &deflt);
    KTEST_ASSERT(mount_remove("/mnt", &why));
    KTEST_ASSERT(got);
    KTEST_ASSERT(deflt > asked);
}
