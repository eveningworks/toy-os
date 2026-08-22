#ifndef TOSH_JOBS_H
#define TOSH_JOBS_H

// The shell's JOB TABLE: what `jobs`, `fg` and `bg` are lists of.
//
// **A JOB IS A PROCESS GROUP, AND THE TABLE IS THE ONLY PLACE IT HAS A
// NAME.** The kernel knows about processes and groups; it has no idea
// that `cat big | grep x` is one thing a person started, which of its
// stages to report a status from, or what to print when they ask what
// is suspended. That is a shell's bookkeeping in every Unix -- bash,
// dash and zsh all keep exactly this table -- and it is a shell's here
// for the same reason: nothing in the kernel would be improved by
// learning what a command line looked like.
//
// ITS OWN FILE, not another section of tosh.c, because it is state that
// outlives a command line while everything in tosh.c serves one. The
// split is by lifetime, which is the honest seam.
//
// WHAT IT DELIBERATELY IS NOT: a process table. It never enumerates,
// never asks the kernel what exists, and holds no opinion about any
// process it did not start. A job that dies while nobody is looking is
// found by asking after ITS pid, not by scanning -- `ps` is the program
// that scans, and duplicating it here would be a second, worse one.

#define TOSH_JOBS_MAX 8   // how many suspended/background jobs at once
#define TOSH_JOB_CMD_MAX 64 // the command line as typed, for `jobs`
// Must be at least tosh.c's TOSH_STAGE_MAX -- a job holds every stage
// of its pipeline, and a stage left out of the table is a stage nobody
// waits for and therefore a permanent zombie.
#define TOSH_JOB_PIDS_MAX 4

struct tosh_job {
    int  id;      // 1-based, what `[1]` prints. 0 means a free slot
    int  pgid;    // the group -- what a signal is aimed at
    // **EVERY STAGE, NOT JUST THE LAST.** A job is a pipeline, and a
    // shell must reap all of it: `fg` on a resumed `a | b` that waited
    // only for `b` left `a` a zombie forever, holding a slot nothing
    // would ever free. The LAST one's status is the job's, as in sh --
    // so the order here is the pipeline's order and matters.
    int  pid[TOSH_JOB_PIDS_MAX];
    int  npid;
    int  stopped; // suspended, rather than running in the background
    char cmd[TOSH_JOB_CMD_MAX];
};

// Adds a job and returns its id, or 0 if the table is full (which is
// reported to the user rather than silently dropping the job: a job
// nobody can name is one nobody can resume).
//
// The new job becomes CURRENT, and whatever was current becomes
// previous -- the `+` and `-` markers `jobs` prints, and what a bare
// `fg` means. bash's rule, because a person typing `fg` means the one
// they just suspended.
int tosh_jobs_add(int pgid, const int *pids, int npid, const char *cmd,
                  int stopped);

// The job with `id`, or the CURRENT job when `id` is 0. NULL if there
// is no such job -- including "no jobs at all", which is the answer a
// bare `fg` gets in an empty shell.
struct tosh_job *tosh_jobs_get(int id);

// Forgets a job. Called when it exits, never when it merely stops.
void tosh_jobs_remove(int id);

// 1 if any job is in the table.
int tosh_jobs_any(void);

// Walks the table oldest id first, calling `fn` with each job and the
// marker character for it -- '+' for the current job, '-' for the
// previous, ' ' otherwise. That marker is the one piece of formatting
// this module owns, because it is the one piece derived from state only
// this module has.
void tosh_jobs_each(void (*fn)(void *ctx, const struct tosh_job *job, char marker),
                    void *ctx);

// The id of the current job, or 0.
int tosh_jobs_current(void);

#endif
