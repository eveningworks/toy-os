#!/bin/sh
# Wait for a PID to exit, or for a file to appear -- with a MANDATORY
# bound, and without the failure mode that makes a hand-rolled waiter
# outlive the session.
#
# **A WAITER MUST NOT BE ABLE TO MATCH ITSELF.** The shape this replaces
# is `while ps aux | grep -q "[f]oo"; do sleep 15; done`, and it cannot
# exit: the waiting shell's own command line carries the word `foo`
# somewhere (the reporting `grep` after the loop is enough), so `ps` sees
# it and the condition is true forever. The `[f]oo` trick hides the GREP
# from itself and does nothing about the shell around it. Two of these
# ran for two hours in one session. This script matches no names at all:
# a PID is checked with kill -0 and a file with a test, neither of which
# can see this process.
#
# **AND IT ALWAYS STOPS.** A poll on an artifact that never comes to
# exist cannot tell "not yet" from "never" -- a superseded run's log is
# never -- so the timeout is not optional and expiry is exit 2, distinct
# from the thing being waited for failing.
#
# Prefer not needing it: a backgrounded command's own completion
# notification is the signal, and a waiter beside it is redundant even
# when it works (CLAUDE.md).
#
#   tools/wait_for.sh 12345                  # until that PID exits
#   tools/wait_for.sh --file out.log         # until out.log is non-empty
#   tools/wait_for.sh --file out.log --timeout 600 --interval 5
#
# Exit: 0 = the condition held, 2 = the timeout expired, 1 = bad usage.
set -eu

TIMEOUT=1800
INTERVAL=10
MODE=""
TARGET=""

usage() {
    echo "usage: wait_for.sh <pid> | --file PATH [--timeout SECONDS] [--interval SECONDS]" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --file)     MODE="file"; TARGET="${2:-}"; [ -n "$TARGET" ] || usage; shift 2 ;;
        --timeout)  TIMEOUT="${2:-}"; [ -n "$TIMEOUT" ] || usage; shift 2 ;;
        --interval) INTERVAL="${2:-}"; [ -n "$INTERVAL" ] || usage; shift 2 ;;
        -h|--help)  usage ;;
        -*)         usage ;;   # an unknown flag is usage, not "not a pid"
        *)
            [ -z "$MODE" ] || usage
            MODE="pid"; TARGET="$1"; shift ;;
    esac
done
[ -n "$MODE" ] || usage

case "$MODE" in
    pid)
        case "$TARGET" in
            ''|*[!0-9]*) echo "wait_for: '$TARGET' is not a pid" >&2; exit 1 ;;
        esac
        # NOT AN ERROR. A pid that is already gone is the condition
        # already true, which is the common case when a waiter is armed
        # a moment too late -- reporting it as a failure would send the
        # caller looking for a run that finished normally.
        kill -0 "$TARGET" 2>/dev/null || { echo "wait_for: pid $TARGET already gone"; exit 0; }
        ;;
esac

waited=0
while [ "$waited" -lt "$TIMEOUT" ]; do
    case "$MODE" in
        pid)  kill -0 "$TARGET" 2>/dev/null || { echo "wait_for: pid $TARGET exited after ${waited}s"; exit 0; } ;;
        file) [ -s "$TARGET" ] && { echo "wait_for: $TARGET appeared after ${waited}s"; exit 0; } ;;
    esac
    sleep "$INTERVAL"
    waited=$((waited + INTERVAL))
done

# THE EXPIRY IS LOUD AND ITS OWN EXIT CODE. A waiter that gave up
# silently would read as the thing it waited for having succeeded.
echo "wait_for: TIMEOUT after ${TIMEOUT}s waiting for $MODE $TARGET" >&2
exit 2
