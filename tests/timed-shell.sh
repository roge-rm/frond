#!/bin/sh
# Runs a command under a time limit in its own process group. make uses it as SHELL for the
# test targets, so every recipe line gets the limit; python checks can run it directly:
#   tests/timed-shell.sh python3 tests/foo.py
# TEST_TIMEOUT is the limit in seconds, TEST_NAME the name printed on a timeout (make sets it
# to the target), TEST_KILL_GRACE the seconds between TERM and KILL. On a timeout the whole
# group is killed, and so is anything the line started in a session of its own (found by a
# marker in its environment); the exit status is 124. A test-* target's result goes to
# build/check-passes.tsv (tools/check-record.py) unless FROND_NO_RECORD is set: a failing line
# at once, and the target's pass (or skip) when its last recipe line passes.
# A check says it skipped by printing SKIP or SKIPPED, or a lower case status like
# "NAME: skipped, no ROM" (see skip_words below).
limit=${TEST_TIMEOUT:-300}
name=${TEST_NAME:-test}
grace=${TEST_KILL_GRACE:-10}
start=$(date +%s)
tool="$(dirname "$0")/../tools/check-record.py"
recording=
if [ -z "$FROND_NO_RECORD" ]; then
    case "$name" in test-*) recording=1 ;; esac
fi
# The first line of a target notes the commit and tree it starts from.
[ -n "$recording" ] && python3 "$tool" begin "$PPID" "$name" >/dev/null 2>&1
# Output goes through tee into a scratch file so a skip line can be seen afterwards. The tees
# ignore INT so the test's own cleanup output after Ctrl-C still shows, and -p keeps them
# writing the log when the terminal goes away.
scratch=$(mktemp -d "${TMPDIR:-/tmp}/timed-shell.XXXXXX")
mkfifo "$scratch/out" "$scratch/err"
(trap '' INT; exec tee -p "$scratch/out.log" < "$scratch/out") &
teeout=$!
(trap '' INT; exec tee -p "$scratch/err.log" < "$scratch/err" >&2) &
teeerr=$!
# Every process the line starts carries this variable, even one in a session of its own.
marker=FROND_TS_$$_$start
export PYTHONUNBUFFERED=1
# timeout runs in the background so INT, TERM and HUP reaching this script (Ctrl-C at the
# terminal) are forwarded to it, and on to the test; timeout leaves the terminal's group, so
# --foreground would lose the group kill on a timeout.
if [ "$1" = -c ]; then
    env "$marker=1" timeout -k "$grace" "$limit" /bin/sh "$@" > "$scratch/out" 2> "$scratch/err" &
else
    env "$marker=1" timeout -k "$grace" "$limit" "$@" > "$scratch/out" 2> "$scratch/err" &
fi
pid=$!
stopped=
trap 'stopped=1; kill -INT $pid 2>/dev/null' INT
trap 'stopped=1; kill -TERM $pid 2>/dev/null' TERM HUP
wait $pid
status=$?
while kill -0 $pid 2>/dev/null; do
    wait $pid
    status=$?
done
timed_out=
if [ "$status" = 124 ] || { [ "$status" = 137 ] && [ $(( $(date +%s) - start )) -ge "$limit" ]; }; then
    timed_out=1
fi
# What the line left running with the marker, like a guest in its own session holding a slot.
leftovers() {
    grep -lzx "$marker=1" /proc/[0-9]*/environ 2>/dev/null | sed 's#^/proc/\([0-9]*\)/environ$#\1#'
}
if [ -n "$timed_out$stopped" ]; then
    left=$(leftovers)
    if [ -n "$left" ]; then
        echo "timed-shell: stopping what $name left running: $(echo $left)" >&2
        kill -TERM $left 2>/dev/null
        n=0
        while [ $n -lt 30 ] && [ -n "$(leftovers)" ]; do sleep 0.1; n=$((n + 1)); done
        left=$(leftovers)
        [ -n "$left" ] && kill -KILL $left 2>/dev/null
    fi
fi
# The tees end when every writer has closed the fifos. Something the line left behind can keep
# them open, so wait at most 5 s and then stop them.
n=0
while { kill -0 $teeout || kill -0 $teeerr; } 2>/dev/null; do
    if [ $n -ge 50 ]; then
        echo "timed-shell: $name left a process holding its output, the rest of it isn't shown" >&2
        kill $teeout $teeerr 2>/dev/null
        break
    fi
    sleep 0.1
    n=$((n + 1))
done
wait $teeout $teeerr 2>/dev/null
# A skip is an upper case SKIP or SKIPPED word, or a lower case one in a status form:
# "NAME: skipped, no ROM", "no NDK, skipped", "NAME skipped: no session", "(skipped frond)",
# "OK (skipped=2)", "taps skipped, no pen probe", "3 skipped". Counts like "12 samples skipped"
# are not skips.
skip_words='\bSKIP(PED)?\b|(: |, )skipped\b|\bskipped:|\(skipped[ =]|\bskipped, (no|needs)\b|\bskipped, [^ ,]+ has no\b|(^|[ (])[1-9][0-9]* skipped\b'
skipped=
grep -qsE "$skip_words" "$scratch/out.log" "$scratch/err.log" && skipped=1
rm -rf "$scratch"
secs=$(( $(date +%s) - start ))
record() {
    [ -n "$recording" ] && python3 "$tool" line "$PPID" "$name" "$1" "$secs" >/dev/null 2>&1
}
if [ -n "$timed_out" ]; then
    echo "TEST TIMEOUT: $name ran over ${limit}s (TEST_TIMEOUT), its process group was killed" >&2
    record fail
    exit 124
fi
if [ "$status" != 0 ]; then record fail
elif [ -n "$skipped" ]; then record skip
else record pass; fi
if [ -n "$FROND_SKIP_FILE" ] && [ -n "$skipped" ]; then echo "$name" >> "$FROND_SKIP_FILE"; fi
exit "$status"
