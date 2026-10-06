#ifndef ULIB_UWALK_H
#define ULIB_UWALK_H

// A DIRECTORY TREE WALK, one directory at a time, for a caller that has
// to stop, report progress or share a thread: the File Manager's search
// into subfolders and its folder sizes. find(1)'s traversal, as a
// resumable state machine instead of a recursion.
//
// **DEPTH-FIRST OVER AN EXPLICIT STACK**, not breadth-first over a queue:
// a queue holds every directory found and not yet read, which grows with
// the tree's WIDTH and has no bound worth naming; a stack holds one
// frame per LEVEL, and a level is bounded by the path length. Each frame
// is a directory and how far into its listing the walk has got, so a
// listing is paged (SYS_LISTDIR_MAX) and a huge directory costs no more
// memory than a small one.
//
// The state is the caller's (several KB -- static, or a heap), and it
// touches nothing else, so a worker thread may own one.

#include "rt/sys.h"

#define UWALK_PATH  256
#define UWALK_DEPTH 48     // levels; a deeper subtree is skipped, and counted
#define UWALK_PAGE  64     // entries listed per call

// Called for every entry below the root, in walk order. `path` is the
// entry's full path. Return 0 to leave a DIRECTORY unvisited (or to skip
// nothing for a file), 1 to go on, -1 to stop the whole walk.
typedef int (*uwalk_fn)(void *ctx, const char *path, const struct sys_dirent *e);

struct uwalk {
    struct { char path[UWALK_PATH]; int offset; } frame[UWALK_DEPTH];
    int depth;                       // frames in use; 0 = finished
    struct sys_dirent page[UWALK_PAGE];
    unsigned long long entries;      // seen so far
    unsigned dirs;                   // directories entered
    unsigned too_deep;               // subtrees past UWALK_DEPTH, skipped
    unsigned unreadable;             // directories that could not be listed
    int stopped;                     // the callback said -1
};

// Start at `root` (a directory). 0 when it is not one.
int uwalk_begin(struct uwalk *w, const char *root);

// Read ONE page of the current directory, calling `fn` for each entry
// and descending where it says to. 1 while there is more to walk, 0 when
// the walk is over (finished, or stopped).
int uwalk_step(struct uwalk *w, uwalk_fn fn, void *ctx);

#endif
