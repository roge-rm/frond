#!/bin/sh
# Runs a command under a time limit in its own process group. make uses it as SHELL for the
# test targets, so every recipe line gets the limit; python checks can run it directly:
#   tests/timed-shell.sh python3 tests/foo.py
# TEST_TIMEOUT is the limit in seconds, TEST_NAME the name printed on a timeout (make sets it
# to the target), TEST_KILL_GRACE the seconds between TERM and KILL. On a timeout the whole
# group is killed and the exit status is 124. A test-* target's result goes to
# build/check-passes.tsv (tools/check-record.py) unless FROND_NO_RECORD is set.
limit=${TEST_TIMEOUT:-300}
name=${TEST_NAME:-test}
start=$(date +%s)
# timeout runs in the background so INT, TERM and HUP reaching this script (Ctrl-C at the
# terminal) are forwarded to it, and on to the test; timeout leaves the terminal's group, so
# --foreground would lose the group kill on a timeout.
if [ "$1" = -c ]; then
    timeout -k "${TEST_KILL_GRACE:-10}" "$limit" /bin/sh "$@" &
else
    timeout -k "${TEST_KILL_GRACE:-10}" "$limit" "$@" &
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
record() {
    [ -z "$FROND_NO_RECORD" ] && case "$name" in test-*)
        python3 "$(dirname "$0")/../tools/check-record.py" record "$name" "$1" $(( $(date +%s) - start )) 2>/dev/null || true ;;
    esac
}
if [ "$status" = 124 ] || { [ "$status" = 137 ] && [ $(( $(date +%s) - start )) -ge "$limit" ]; }; then
    echo "TEST TIMEOUT: $name ran over ${limit}s (TEST_TIMEOUT), its process group was killed" >&2
    record fail
    exit 124
fi
if [ "$status" = 0 ]; then record pass; else record fail; fi
exit "$status"
