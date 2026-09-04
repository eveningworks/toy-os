// pmap -- one process's address space, region by region.
//
//   pmap          the calling process's own map
//   pmap <pid>    that process's map
//
// Every span is a RESERVATION: pages arrive on first touch (sbrk maps
// nothing, mmap maps nothing, the stack grows on fault), so the sizes
// here are address space, not memory. `ps`/Task Manager report the
// resident side.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static const char *kind_name(unsigned long long k) {
    switch (k) {
    case QUERY_PROCMAP_IMAGE: return "image";
    case QUERY_PROCMAP_HEAP:  return "heap";
    case QUERY_PROCMAP_STACK: return "stack";
    case QUERY_PROCMAP_ANON:  return "anon";
    case QUERY_PROCMAP_FILE:  return "file";
    }
    return "?";
}

// "r--", "rw-", "r-x" for a mapping; the synthesized kinds carry no
// prot and print blank rather than a guess.
static void prot_str(char *out, unsigned long long kind,
                     unsigned long long prot) {
    if (kind != QUERY_PROCMAP_ANON && kind != QUERY_PROCMAP_FILE) {
        strcpy(out, "   ");
        return;
    }
    out[0] = (prot & 0x1) ? 'r' : '-';
    out[1] = (prot & 0x2) ? 'w' : '-';
    out[2] = (prot & 0x4) ? 'x' : '-';
    out[3] = '\0';
}

static void size_str(char *out, int cap, unsigned long long b) {
    if (b >= (1ull << 30) && !(b & ((1ull << 30) - 1)))
        snprintf(out, cap, "%lluG", b >> 30);
    else if (b >= (1ull << 20) && !(b & ((1ull << 20) - 1)))
        snprintf(out, cap, "%lluM", b >> 20);
    else
        snprintf(out, cap, "%lluK", b >> 10);
}

int main(int argc, char **argv) {
    int pid;
    if (argc == 1) {
        pid = getpid();
    } else if (argc == 2 && (pid = atoi(argv[1])) > 0) {
        // fine
    } else {
        cmd_usage("pmap [pid]");
        return 1;
    }

    struct query_procmap q;
    char line[160], sz[24], prot[8];
    int rows = 0;

    QUERY_FOREACH(QUERY_PROCMAP, q, i) {
        if ((int)q.pid != pid) continue;
        if (!rows) {
            snprintf(line, sizeof line, "pid %d\n", pid);
            sys_print(line);
        }
        rows++;
        size_str(sz, sizeof sz, q.bytes);
        prot_str(prot, q.kind, q.prot);
        // Formatted THEN padded as a string -- numeric widths zero-pad
        // here, wrong for a column (CLAUDE.md).
        char addr[24];
        snprintf(addr, sizeof addr, "%#llx", (unsigned long long)q.base);
        snprintf(line, sizeof line, "  %14s %8s %s %-5s %s\n",
                 addr, sz, prot, kind_name(q.kind), q.path);
        sys_print(line);
    }

    if (!rows) {
        snprintf(line, sizeof line, "pmap: no such process: %d\n", pid);
        sys_print(line);
        return 1;
    }
    return 0;
}
