#!/usr/bin/env python3
"""Record which checks passed and rerun only the ones whose source paths changed.

  check-record.py record NAME STATUS SECONDS   (called by tests/timed-shell.sh)
  check-record.py list [GLOB...]               what `make recheck` would run, and why
  check-record.py recheck [GLOB...]            run those, one `make NAME` each, and record

The record is build/check-passes.tsv, one line per run: name, commit, unix time, status
(pass or fail), tree (clean, or the hash of the uncommitted changes), seconds. The last line for
a check counts. The path map is tests/check-paths.tsv. A check that isn't in the map reruns every
time. Uncommitted and untracked files count as changed.
"""
import fnmatch
import hashlib
import os
import re
import subprocess
import sys
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
            rc = subprocess.run(["make", "-C", ROOT, name]).returncode
            record(name, "pass" if rc == 0 else "fail", time.time() - start)
            if rc:
                failed.append(name)
        print("recheck: %d run, %d failed%s" % (len(todo), len(failed), ": " + " ".join(failed) if failed else ""))
        return 1 if failed else 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
