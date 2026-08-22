// The shell's job table -- see tosh_jobs.h for what a job is and why
// this is the shell's problem rather than the kernel's.
//
// A FIXED ARRAY WITH NO ALLOCATION, and eight of them, because a job
// table is bounded by what a person can keep track of. bash grows one
// dynamically and can hold thousands; nobody has ever wanted that, and
// a full table here says so out loud rather than dropping the job.
#include "lib/tosh_jobs.h"

static struct tosh_job g_jobs[TOSH_JOBS_MAX];
static int g_current;   // id, or 0
static int g_previous;  // id, or 0

static struct tosh_job *slot_of(int id) {
    if (id <= 0) return 0;
    for (int i = 0; i < TOSH_JOBS_MAX; i++)
        if (g_jobs[i].id == id) return &g_jobs[i];
    return 0;
}

// The lowest id nothing is using. Ids are REUSED once a job is gone,
// which is what keeps them small enough to type -- bash does the same,
// and an ever-increasing counter would have `fg 47` in a shell with two
// jobs in it.
static int next_id(void) {
    for (int id = 1; id <= TOSH_JOBS_MAX; id++)
        if (!slot_of(id)) return id;
    return 0;
}

int tosh_jobs_add(int pgid, const int *pids, int npid, const char *cmd,
                  int stopped) {
    int id = next_id();
    if (!id) return 0;

    struct tosh_job *j = 0;
    for (int i = 0; i < TOSH_JOBS_MAX; i++)
        if (!g_jobs[i].id) { j = &g_jobs[i]; break; }
    if (!j) return 0; // next_id() said there was room; belt for the braces

    j->id = id;
    j->pgid = pgid;
    j->npid = 0;
    for (int i = 0; i < npid && i < TOSH_JOB_PIDS_MAX; i++)
        if (pids[i] > 0) j->pid[j->npid++] = pids[i];
    j->stopped = stopped;
    int n = 0;
    for (; cmd && cmd[n] && n < TOSH_JOB_CMD_MAX - 1; n++) j->cmd[n] = cmd[n];
    j->cmd[n] = '\0';

    if (g_current != id) g_previous = g_current;
    g_current = id;
    return id;
}

struct tosh_job *tosh_jobs_get(int id) {
    return slot_of(id ? id : g_current);
}

void tosh_jobs_remove(int id) {
    struct tosh_job *j = slot_of(id);
    if (!j) return;
    j->id = 0;
    j->pgid = j->stopped = j->npid = 0;
    for (int i = 0; i < TOSH_JOB_PIDS_MAX; i++) j->pid[i] = 0;
    j->cmd[0] = '\0';

    // THE MARKERS HAVE TO BE REPAIRED, not just cleared. A `+` pointing
    // at a job that has exited is worse than no marker: a bare `fg`
    // would find nothing and report an empty shell while `jobs` still
    // listed one. So the previous job is promoted, and if there is no
    // previous, whatever is left takes the mark.
    if (g_current == id) {
        g_current = g_previous;
        g_previous = 0;
    }
    if (g_previous == id) g_previous = 0;
    if (!g_current) {
        for (int i = 0; i < TOSH_JOBS_MAX; i++)
            if (g_jobs[i].id) { g_current = g_jobs[i].id; break; }
    }
}

int tosh_jobs_any(void) {
    for (int i = 0; i < TOSH_JOBS_MAX; i++) if (g_jobs[i].id) return 1;
    return 0;
}

int tosh_jobs_current(void) { return g_current; }

void tosh_jobs_each(void (*fn)(void *ctx, const struct tosh_job *job, char marker),
                    void *ctx) {
    if (!fn) return;
    // BY ID, not by slot: the array's order is whichever slots happened
    // to be free, and a list that jumps about between runs reads as a
    // bug in the shell.
    for (int id = 1; id <= TOSH_JOBS_MAX; id++) {
        struct tosh_job *j = slot_of(id);
        if (!j) continue;
        char marker = id == g_current ? '+' : (id == g_previous ? '-' : ' ');
        fn(ctx, j, marker);
    }
}
