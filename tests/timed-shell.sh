#!/bin/sh
# Runs a command under a time limit in its own process group. make uses it as SHELL for the
# test targets, so every recipe line gets the limit; python checks can run it directly:
#   tests/timed-shell.sh python3 tests/foo.py
# TEST_TIMEOUT is the limit in seconds, TEST_NAME the name printed on a timeout (make sets it
# to the target), TEST_KILL_GRACE the seconds between TERM and KILL. On a timeout the whole
# group is killed and the exit status is 124. A test-* target's result goes to
# build/check-passes.tsv (tools/check-record.py) unless FROND_NO_RECORD is set: a failing line
# at once, and a target's pass (or skip, when its output has a SKIP or SKIPPED word) once make
# has run all its lines, unless Ctrl-C stopped it. A check says it skipped by printing SKIP.
limit=${TEST_TIMEOUT:-300}
name=${TEST_NAME:-test}
start=$(date +%s)
# timeout runs in the background so INT, TERM and HUP reaching this script (Ctrl-C at the
# terminal) are forwarded to it, and on to the test; timeout leaves the terminal's group, so
# --foreground would lose the group kill on a timeout.
# Output goes through tee into a scratch file so a SKIP line can be seen afterwards.
scratch=$(mktemp -d "${TMPDIR:-/tmp}/timed-shell.XXXXXX")
mkfifo "$scratch/out" "$scratch/err"
tee "$scratch/out.log" < "$scratch/out" &
teeout=$!
tee "$scratch/err.log" < "$scratch/err" >&2 &
teeerr=$!
if [ "$1" = -c ]; then
    timeout -k "${TEST_KILL_GRACE:-10}" "$limit" /bin/sh "$@" > "$scratch/out" 2> "$scratch/err" &
else
    timeout -k "${TEST_KILL_GRACE:-10}" "$limit" "$@" > "$scratch/out" 2> "$scratch/err" &
fi
pid=$!
trap 'kill -INT $pid 2>/dev/null' INT
trap 'kill -TERM $pid 2>/dev/null' TERM HUP
wait $pid
status=$?
if kill -0 $pid 2>/dev/null; then
    wait $pid
    status=$?
fi
wait $teeout $teeerr
skipped=
grep -qsw 'SKIP\|SKIPPED' "$scratch/out.log" "$scratch/err.log" && skipped=1
rm -rf "$scratch"
# The first recipe line of a target starts a watcher that records the target once make exits.
record() {
    [ -n "$FROND_NO_RECORD" ] && return
    case "$name" in test-*) ;; *) return ;; esac
    tool="$(dirname "$0")/../tools/check-record.py"
    secs=$(( $(date +%s) - start ))
    case "$1" in
    fail) python3 "$tool" record "$name" fail "$secs" 2>/dev/null || true ;;
    esac
    python3 "$tool" line "$PPID" "$name" "$1" "$secs" >/dev/null 2>&1 || true
}
if [ "$status" = 124 ] || { [ "$status" = 137 ] && [ $(( $(date +%s) - start )) -ge "$limit" ]; }; then
    echo "TEST TIMEOUT: $name ran over ${limit}s (TEST_TIMEOUT), its process group was killed" >&2
    record fail
    exit 124
fi
if [ "$status" != 0 ]; then record fail
elif [ -n "$skipped" ]; then record skip
else record pass; fi
if [ -n "$FROND_SKIP_FILE" ] && [ -n "$skipped" ]; then echo "$name" >> "$FROND_SKIP_FILE"; fi
exit "$status"
