#!/usr/bin/env python3
"""Record which checks passed and rerun only the ones whose source paths changed.

  check-record.py record NAME STATUS SECONDS   (STATUS is pass, fail or skip)
  check-record.py line MAKEPID NAME STATUS SECONDS
                                               (called by tests/timed-shell.sh for each recipe line;
                                               a watcher records the target when make exits)
  check-record.py list [GLOB...]               what `make recheck` would run, and why
  check-record.py recheck [GLOB...]            run those, one `make NAME` each, and record

The record is build/check-passes.tsv, one line per run: name, commit, unix time, status
(pass, fail or skip), tree (clean, or the hash of the uncommitted changes), seconds. The last line for
a check counts. The path map is tests/check-paths.tsv. A check that isn't in the map reruns every
time. Uncommitted and untracked files count as changed.
"""
import fnmatch
import hashlib
import os
import re
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECORD = os.path.join(ROOT, "build", "check-passes.tsv")
MAP = os.path.join(ROOT, "tests", "check-paths.tsv")


def git(*args):
    r = subprocess.run(["git", "-C", ROOT, *args], capture_output=True)
    return r.returncode, r.stdout


def head():
    rc, out = git("rev-parse", "HEAD")
    return out.decode().strip() if rc == 0 else "unknown"


def untracked():
    return [p for p in git("ls-files", "-o", "--exclude-standard", "-z")[1].decode().split("\0") if p]


def tree_state():
    """'clean', or a hash of the uncommitted changes and untracked files."""
    diff = git("diff", "HEAD", "--binary")[1]
    files = untracked()
    if not diff and not files:
        return "clean"
    h = hashlib.sha1(diff)
    for p in sorted(files):
        h.update(p.encode() + b"\0")
        try:
            with open(os.path.join(ROOT, p), "rb") as f:
                h.update(hashlib.sha1(f.read()).digest())
        except OSError:
            pass
    return h.hexdigest()


def record(name, status, seconds):
    if not re.match(r"test-[A-Za-z0-9-]+$", name):
        return
    os.makedirs(os.path.dirname(RECORD), exist_ok=True)
    line = "\t".join([name, head(), str(int(time.time())), status, tree_state(), str(int(float(seconds)))]) + "\n"
    with open(RECORD, "a") as f:
        f.write(line)


STATE = os.path.join(ROOT, "build", "check-state")


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def line_result(makepid, name, status, seconds):
    """One recipe line finished. The first line of a target starts the watcher."""
    os.makedirs(STATE, exist_ok=True)
    path = os.path.join(STATE, "%s.%s" % (makepid, name))
    watcher = 0
    try:
        with open(path) as f:
            watcher = int(f.readline().split()[0])
    except (OSError, ValueError, IndexError):
        pass
    if not (watcher and alive(watcher)):
        with open(path, "w") as f:
            f.write("0\n")
        p = subprocess.Popen([sys.executable, os.path.abspath(__file__), "watch", str(makepid), name, path],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(path, "w") as f:
            f.write("%d\n" % p.pid)
    with open(path, "a") as f:
        f.write("%s %s\n" % (status, int(float(seconds))))


def watch(makepid, name, path):
    """Waits for make to exit, then records the target unless a line failed (already recorded) or
    Ctrl-C or a kill reached this process too (it shares make's process group)."""
    stopped = []
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *a: stopped.append(1))
    while alive(makepid):
        time.sleep(0.2)
    time.sleep(0.2)
    lines = []
    try:
        with open(path) as f:
            lines = [l.split() for l in f.read().splitlines()[1:]]
        os.unlink(path)
    except OSError:
        pass
    results = [l[0] for l in lines if len(l) == 2]
    if stopped or not results or "fail" in results:
        return
    record(name, "skip" if "skip" in results else "pass", sum(int(l[1]) for l in lines if len(l) == 2))


def last_records():
    last = {}
    try:
        with open(RECORD) as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if len(p) >= 5:
                    last[p[0]] = p
    except OSError:
        pass
    return last


def check_names():
    out = subprocess.run(["make", "-C", ROOT, "-pRrq", ":"], capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        m = re.match(r"(test-[A-Za-z0-9-]+):( |$)", line)
        if m and "=" not in line:
            names.add(m.group(1))
    return sorted(names)


def load_map():
    macros, rules = {}, []
    with open(MAP) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            key, _, rest = line.partition("\t")
            key, rest = key.strip(), rest.split()
            if key.startswith("@"):
                macros[key] = rest
            else:
                rules.append((key.split(), rest))
    return macros, rules


def watched(name, macros, rules):
    """The path patterns for a check, or None when the map has nothing for it."""
    def expand(tokens):
        out = []
        for t in tokens:
            bang = t.startswith("!")
            t2 = t[1:] if bang else t
            for x in macros[t2] if t2 in macros else [t2]:
                out.append(("!" if bang else "") + x)
        return out
    stem = name[len("test-"):]
    own = [f"tests/{stem}*"]
    shared = []
    chosen = None
    for names, paths in rules:
        if names == ["*"]:
            shared += expand(paths)
        elif chosen is None and any(fnmatch.fnmatchcase(name, n) for n in names):
            chosen = expand(paths)
    if chosen is None:
        return None
    return own + shared + chosen


def matches(path, pats):
    hit = False
    for p in pats:
        neg = p.startswith("!")
        p = p[1:] if neg else p
        ok = path.startswith(p) if p.endswith("/") else fnmatch.fnmatchcase(path, p)
        if ok and neg:
            return False
        hit = hit or (ok and not neg)
    return hit


def changed_since(commit, cache):
    if commit not in cache:
        rc, out = git("diff", "--name-only", "-z", commit)
        cache[commit] = None if rc else [p for p in out.decode().split("\0") if p] + untracked()
    return cache[commit]


def plan(patterns):
    macros, rules = load_map()
    last = last_records()
    state = tree_state()
    cache, todo = {}, []
    for name in check_names():
        if patterns and not any(fnmatch.fnmatchcase(name, p) for p in patterns):
            continue
        rec = last.get(name)
        if rec is None:
            todo.append((name, "never passed"))
        elif rec[3] == "skip":
            todo.append((name, "skipped last time"))
        elif rec[3] != "pass":
            todo.append((name, "failed last time"))
        elif rec[4] != "clean":
            if rec[4] != state:
                todo.append((name, "passed on a dirty tree that has changed since"))
        else:
            pats = watched(name, macros, rules)
            files = changed_since(rec[1], cache)
            if pats is None:
                todo.append((name, "no entry in tests/check-paths.tsv"))
            elif files is None:
                todo.append((name, "last pass commit %s not found" % rec[1][:8]))
            else:
                hit = [p for p in files if matches(p, pats)]
                if hit:
                    more = " and %d more" % (len(hit) - 1) if len(hit) > 1 else ""
                    todo.append((name, "changed: %s%s" % (hit[0], more)))
    return todo


def main(argv):
    if len(argv) >= 5 and argv[1] == "record":
        record(argv[2], argv[3], argv[4])
        return 0
    if len(argv) >= 6 and argv[1] == "line":
        line_result(int(argv[2]), argv[3], argv[4], argv[5])
        return 0
    if len(argv) >= 5 and argv[1] == "watch":
        watch(int(argv[2]), argv[3], argv[4])
        return 0
    if len(argv) >= 2 and argv[1] in ("list", "recheck"):
        todo = plan(argv[2:])
        for name, why in todo:
            print("%-40s %s" % (name, why))
        print("%d checks to run" % len(todo))
        if argv[1] == "list":
            return 0
        failed = []
        for name, _ in todo:
            start = time.time()
            fd, skipfile = tempfile.mkstemp(prefix="check-skip-")
            os.close(fd)
            env = dict(os.environ, FROND_NO_RECORD="1", FROND_SKIP_FILE=skipfile)
            rc = subprocess.run(["make", "-C", ROOT, name], env=env).returncode
            skipped = os.path.getsize(skipfile) > 0
            os.unlink(skipfile)
            record(name, "fail" if rc else "skip" if skipped else "pass", time.time() - start)
            if rc:
                failed.append(name)
        print("recheck: %d run, %d failed%s" % (len(todo), len(failed), ": " + " ".join(failed) if failed else ""))
        return 1 if failed else 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
