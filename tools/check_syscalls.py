#!/usr/bin/env python3
"""No two syscalls share a number.

WHY THIS EXISTS
---------------
The syscall rows (kernel/include/abi/syscall_rows.h, an X-macro list
kernel/proc/syscall_table.c expands) are DESIGNATED INITIALIZERS, so a
row's index IS its syscall number -- which is what makes the table
unorderable by accident and is otherwise a good property. Its one hole
is that C lets the same index be written twice and keeps the LAST one,
silently.

That happened on 2026-09-12. SYS_SIGPROCMASK was given 108 by reading
the bottom of abi/syscall_abi.h and adding one -- but the numbers there
are not in file order, and 108 was already SYS_FORK. The build was
clean, the new syscalls worked, and `fork()` dispatched to
sigprocmask(): every fork returned 0, so every caller believed it was
the child. The only symptom was fork_test dying with no fault and no
log, and the only way to see it was `strace` printing the wrong name.

WHAT IT CHECKS: no two rows of syscall_rows.h resolve to the same
number. The row NAMES are read from the table and their values from
abi/syscall_abi.h, so nothing here has to guess which SYS_* constants
are syscall numbers -- having a row IS the definition, and the flags,
limits and sentinels that share the prefix are simply not rows.

WHAT IT DOES NOT CHECK: whether a number is sensible, or whether every
defined number has a row. A number with no row already answers -ENOSYS
through the same path as any other failure, which is designed.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ABI = os.path.join(REPO, "kernel", "include", "abi", "syscall_abi.h")
TABLE = os.path.join(REPO, "kernel", "include", "abi", "syscall_rows.h")

def main():
    abi = open(ABI).read()
    values = {m.group(1): int(m.group(2)) for m in
              re.finditer(r"^#define\s+(SYS_[A-Z0-9_]+)\s+(-?\d+)", abi, re.M)}

    rows = re.findall(r"^SYSCALL_ROW\(\s*(SYS_[A-Z0-9_]+|\d+)\s*,", open(TABLE).read(), re.M)
    if not rows:
        print("check_syscalls: no rows found in syscall_rows.h -- has it moved?")
        return 1

    seen, problems = {}, []
    for name in rows:
        if name.isdigit():          # a retired number's row, written as a number
            values[name] = int(name)
        if name not in values:
            problems.append(f"{name} has a table row but no number in {os.path.basename(ABI)}")
            continue
        v = values[name]
        if v in seen:
            problems.append(
                f"{name} and {seen[v]} are both {v} -- the table's designated "
                f"initializers keep only the LAST row, so one of these "
                f"silently dispatches to the other")
        else:
            seen[v] = name

    if problems:
        print(f"check_syscalls: {len(problems)} problem(s)\n")
        for p in problems:
            print(f"  {p}")
        return 1
    print(f"check_syscalls: ok -- {len(rows)} syscall(s) in the table, "
          f"all on distinct numbers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
