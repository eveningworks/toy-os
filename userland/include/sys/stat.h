#ifndef ULIB_SYS_STAT_H
#define ULIB_SYS_STAT_H

// Directory creation, and the permission bits that come with the
// signature.
//
// **stat() IS HERE NOW, and this header used to explain at length why it
// could not be.** The old argument was that `struct stat` is the union
// of everything a filesystem might report, TFS3 has almost none of it,
// and a struct of invented zeroes lets ported code compile and then take
// wrong branches on `st_mode`.
//
// The objection was right about ZEROES and wrong about invention. Most
// of this struct is not invented: TFS3 carries the type, the link count,
// the size, and both timestamps, and it grew real permission bits at
// inode offset 92. What is genuinely made up is `st_uid`/`st_gid`, and
// they are 0 because this is a single-user system -- which is a FACT,
// not a placeholder. Linux's own FAT and NTFS drivers synthesise
// uid/gid/mode from mount options for exactly this reason: a consistent
// documented value is usable and a zero is not.
//
// What is NOT here: `st_blocks`, `st_blksize`, `st_rdev` and the
// nanosecond timestamp fields. Nothing in this system can answer them
// and no caller has asked.

#include <sys/types.h>

// **THE MODE IS ACCEPTED AND IGNORED, and that is not the same as being
// dishonoured.** There are no permission bits on this filesystem at all,
// so 0755 does not fail to be applied -- there is nothing for it to
// mean. That is why this does not follow CLAUDE.md's usual rule of
// REFUSING what it cannot honour (a non-empty `sa_mask` is `EINVAL`,
// <termios.h>'s VMIN/VTIME are undefined on purpose): those refuse
// because obeying halfway would change behaviour a caller depends on.
// Here the caller depends on a directory existing, and it does.
//
// Returns 0, or -1 with errno set -- EEXIST if the name is taken,
// ENOENT if a parent directory is missing. Creates ONE level, as POSIX
// says: `mkdir("/a/b/c")` fails with ENOENT if `/a/b` does not exist.
int mkdir(const char *path, mode_t mode);

// **ACCEPTED, RECORDED, AND APPLIED TO NOTHING.** A umask subtracts bits
// from the mode a creation asks for -- but nothing here passes a mode to
// creation: a file gets the default for its type (kernel/fs/tfs3_internal.h's
// T3_MODE_DEFAULT) and there is no chmod to change it afterwards. The
// value is stored and returned so the get-and-restore idiom every shell
// uses (`old = umask(0); umask(old)`) behaves, and so a caller can read
// back what it set. When creation learns to honour a mode, the mask is
// already here.
//
// Returns the PREVIOUS mask, as POSIX says.
mode_t umask(mode_t mask);

// Change `path`'s permission bits. Returns 0, or -1 with errno --
// ENOENT, or ENOTSUP on a filesystem whose format has nowhere to keep
// them (ramfs, FAT32; ask stat's SYS_STAT_MODE flag if you want to know
// first).
//
// **THE TYPE BITS ARE IGNORED**, as POSIX requires: chmod changes
// permissions, and one that could turn a file into a directory would be
// a corruption primitive rather than a convenience.
int chmod(const char *path, mode_t mode);

// **NOT HERE, AND DELIBERATELY: fchmod().** It would need the kernel to
// carry a path back from a descriptor, which this fd layer does not do
// for anything else either -- an open file knows its name only for the
// duration of the call that opened it. chmod() by path is what every
// caller here has.

// The mode bits themselves, so that a caller writing 0755 in symbols
// compiles. Standard octal values; nothing here reads them.
#include <time.h>

// The type bits of st_mode, in the usual octal. Values are Unix's, so
// code that masks with 0170000 by hand rather than using S_ISDIR still
// works.
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFSOCK 0140000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
// **THE LAST FOUR ARE ALWAYS FALSE, and that is an answer rather than a
// gap.** This system has no device nodes, no FIFOs on the filesystem and
// no Unix-domain sockets -- a path names a file, a directory or a
// symlink and nothing else. They exist so that ported code branching on
// them compiles and takes the branch that is true here.
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

struct stat {
    dev_t     st_dev;      // always 0 -- one volume per path tree
    ino_t     st_ino;      // the filesystem's own, where it has them
    mode_t    st_mode;     // type bits | permission bits
    nlink_t   st_nlink;
    uid_t     st_uid;      // 0: a single-user system, not a placeholder
    gid_t     st_gid;      // 0, likewise
    dev_t     st_rdev;     // always 0 -- there are no device nodes
    off_t     st_size;
    // **st_atime IS st_mtime.** Nothing records an access time, and
    // reporting a made-up one would be the invented zero this header
    // spent years refusing. Linux's `noatime` produces the same shape on
    // purpose and nobody notices.
    time_t    st_atime;
    time_t    st_mtime;
    time_t    st_ctime;    // TFS3's `created`, which is what it stores
    blksize_t st_blksize;  // the filesystem's block size
    blkcnt_t  st_blocks;   // 512-byte units, derived from st_size
};

// Fill *st for `path`, or for an open descriptor. Returns 0, or -1 with
// errno -- ENOENT for a path that does not exist, EBADF for a bad fd.
//
// **lstat() IS stat(), and says so rather than pretending.** It differs
// only for a symlink, and this system's path resolution has no way to
// ask for the link rather than its target (api/fs.h). A separate name
// that behaved identically without saying so would be the more
// dangerous of the two options.
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);

#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

#endif
