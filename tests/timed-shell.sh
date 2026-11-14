#!/bin/sh
# Runs a command under a time limit in its own process group. make uses it as SHELL for the
# test targets, so every recipe line gets the limit; python checks can run it directly:
#   tests/timed-shell.sh python3 tests/foo.py
# TEST_TIMEOUT is the limit in seconds, TEST_NAME the name printed on a timeout (make sets it
# to the target), TEST_KILL_GRACE the seconds between TERM and KILL. On a timeout the whole
# group is killed and the exit status is 124.
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
if [ "$status" = 124 ] || { [ "$status" = 137 ] && [ $(( $(date +%s) - start )) -ge "$limit" ]; }; then
    echo "TEST TIMEOUT: $name ran over ${limit}s (TEST_TIMEOUT), its process group was killed" >&2
    exit 124
fi
exit "$status"
