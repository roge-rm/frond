#!/usr/bin/env python3
"""Record which checks passed and rerun only the ones whose source paths changed.

  check-record.py record NAME STATUS SECONDS   (STATUS is pass, fail or skip)
  check-record.py begin MAKEPID NAME           (tests/timed-shell.sh, before each recipe line)
  check-record.py line MAKEPID NAME STATUS SECONDS
                                               (tests/timed-shell.sh, after each recipe line; the
                                               target is recorded when its last line passes)
  check-record.py audit                        files the Makefile or #include names for a check
                                               that tests/check-paths.tsv doesn't cover
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


def record(name, status, seconds, commit=None, tree=None):
    if not re.match(r"test-[A-Za-z0-9-]+$", name):
        return
    os.makedirs(os.path.dirname(RECORD), exist_ok=True)
    line = "\t".join([name, commit or head(), str(int(time.time())), status, tree or tree_state(),
                      str(int(float(seconds)))]) + "\n"
    with open(RECORD, "a") as f:
        f.write(line)


STATE = os.path.join(ROOT, "build", "check-state")


def run_id(makepid):
    """make's pid and start time, so a reused pid is a different run."""
    try:
        with open("/proc/%d/stat" % makepid) as f:
            return "%d-%s" % (makepid, f.read().rsplit(")", 1)[1].split()[19])
    except (OSError, IndexError):
        return str(makepid)


def make_database(makepid=None):
    """make's database for the Makefile here, or for the one a running make read: same folder,
    same -f files and command line variables."""
    env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL")}
    cwd, extra = ROOT, []
    if makepid:
        try:
            cwd = os.readlink("/proc/%d/cwd" % makepid)
            with open("/proc/%d/cmdline" % makepid, "rb") as f:
                args = [a.decode(errors="replace") for a in f.read().split(b"\0")[1:] if a]
        except OSError:
            args = []
        i = 0
        while i < len(args):
            a = args[i]
            if a in ("-f", "--file", "--makefile") and i + 1 < len(args):
                extra += ["-f", args[i + 1]]
                i += 1
            elif a.startswith(("--file=", "--makefile=")):
                extra += ["-f", a.split("=", 1)[1]]
            elif a.startswith("-f") and len(a) > 2:
                extra += ["-f", a[2:]]
            elif "=" in a and not a.startswith("-"):
                extra.append(a)
            i += 1
    return subprocess.run(["make", "-pRrq", *extra, ":"], cwd=cwd, capture_output=True, text=True,
                          env=env).stdout


def parse_database(text):
    """{target: (prerequisites, recipe commands)} for the rules with a recipe or prerequisites, and
    {variable: value}. A command with backslash continuations is one entry."""
    rules, variables = {}, {}
    lines = text.splitlines()
    i = 0
    files = False
    while i < len(lines):
        line = lines[i]
        if line.startswith("# Files"):
            files = True
        if not files:
            m = re.match(r"([A-Za-z_][A-Za-z0-9_.-]*) :?= (.*)$", line)
            if m and i > 0 and lines[i - 1].startswith("# makefile"):
                variables[m.group(1)] = m.group(2)
            i += 1
            continue
        m = re.match(r"([^\s#:=][^:=]*?):(?!=)(:?)(.*)$", line)
        if m and not line.startswith("\t") and "=" not in m.group(3).split(";")[0]:
            target, prereqs = m.group(1), m.group(3).split("|")[0].split()
            commands, j = [], i + 1
            while j < len(lines) and (lines[j].startswith("#") or lines[j].startswith("\t")):
                if lines[j].startswith("\t"):
                    if not commands or not commands[-1].endswith("\\"):
                        if not lines[j][1:].strip():
                            j += 1
                            continue
                    if commands and commands[-1].endswith("\\"):
                        commands[-1] += "\n" + lines[j][1:]
                    else:
                        commands.append(lines[j][1:])
                j += 1
            old = rules.get(target)
            if old is None or (commands and not old[1]):
                rules[target] = (prereqs, commands)
            i = j
            continue
        i += 1
    return rules, variables


def recipe_lines(makepid, name):
    """How many recipe lines make runs for a target, or 0 when it can't tell."""
    rules, _ = parse_database(make_database(makepid))
    return len(rules.get(name, ((), ()))[1])


def state_path(makepid, name):
    return os.path.join(STATE, "%s.%s" % (run_id(makepid), name))


def begin(makepid, name):
    """A recipe line of a target starts. The first one notes the commit, the tree state and how many
    lines the target has."""
    os.makedirs(STATE, exist_ok=True)
    path = state_path(makepid, name)
    if os.path.exists(path):
        return
    now = time.time()
    for old in os.listdir(STATE):
        try:
            if now - os.path.getmtime(os.path.join(STATE, old)) > 2 * 86400:
                os.unlink(os.path.join(STATE, old))
        except OSError:
            pass
    with open(path + ".new", "w") as f:
        f.write("%s %s %d\n" % (head(), tree_state(), recipe_lines(makepid, name)))
    os.replace(path + ".new", path)


def line_result(makepid, name, status, seconds):
    """A recipe line finished. A failing line records the target's failure at once. The target's
    pass or skip is recorded only when every one of its lines has passed in this make run."""
    path = state_path(makepid, name)
    try:
        with open(path) as f:
            first = f.readline().split()
            lines = [l.split() for l in f.read().splitlines()]
        commit, tree, count = first[0], first[1], int(first[2])
    except (OSError, ValueError, IndexError):
        commit = tree = None
        lines, count = [], 0
    lines.append([status, str(int(float(seconds)))])
    if status == "fail":
        record(name, "fail", seconds, commit, tree)
        try:
            os.unlink(path)
        except OSError:
            pass
        return
    if commit is None:
        return
    if count and len(lines) >= count:
        results = [l[0] for l in lines]
        total = sum(int(l[1]) for l in lines)
        os.unlink(path)
        if len(lines) == count and all(r in ("pass", "skip") for r in results):
            record(name, "skip" if "skip" in results else "pass", total, commit, tree)
        return
    with open(path, "a") as f:
        f.write("%s %s\n" % tuple(lines[-1]))


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


def check_names(rules=None):
    """The test-* targets, without the ones that only list other checks (those run on their own)."""
    if rules is None:
        rules, _ = parse_database(make_database())
    names = []
    for name, (prereqs, commands) in rules.items():
        if not re.match(r"test-[A-Za-z0-9-]+$", name):
            continue
        if not commands and prereqs and all(p.startswith("test-") for p in prereqs):
            continue
        names.append(name)
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
    state, commit = tree_state(), head()
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
            if rec[4] != state or rec[1] != commit:
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


SOURCE = re.compile(r"\.(c|cc|cpp|cxx|h|hh|hpp|inc|S)$")
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
INCLUDE_DIRS = ["core/src/uarm/uarm", "core/src/uarm", "core/src/common", "vendor/dynarmic/src",
                "vendor/dynarmic/src/dynarmic", "desktop"]


def tracked():
    return set(p for p in git("ls-files", "-z")[1].decode().split("\0") if p)


def include_closure(start, files):
    """start plus every tracked file they include, and the .c/.cpp next to each included header."""
    by_suffix = {}
    for p in files:
        by_suffix.setdefault(os.path.basename(p), []).append(p)
    seen, todo = set(), [p for p in start if p in files]
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        seen.add(p)
        if not SOURCE.search(p):
            continue
        try:
            with open(os.path.join(ROOT, p), errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for inc in INCLUDE.findall(text):
            found = None
            for d in [os.path.dirname(p)] + INCLUDE_DIRS:
                c = os.path.normpath(os.path.join(d, inc))
                if c in files:
                    found = c
                    break
            if found is None:
                cands = [c for c in by_suffix.get(os.path.basename(inc), []) if c.endswith("/" + inc)]
                found = cands[0] if len(cands) == 1 else None
            if found is None:
                continue
            todo.append(found)
            stem = os.path.splitext(found)[0]
            for ext in (".c", ".cc", ".cpp"):
                if stem + ext in files:
                    todo.append(stem + ext)
    return seen


def expand_vars(text, variables, depth=0):
    if depth > 8:
        return text
    def sub(m):
        v = m.group(1) or m.group(2)
        return expand_vars(variables.get(v, ""), variables, depth + 1) if v in variables else m.group(0)
    return re.sub(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)|\$\{([A-Za-z_][A-Za-z0-9_]*)\}", sub, text)


def function_args(text):
    """The top level arguments of the make function call text starts with."""
    text = text.lstrip("@+-")
    depth, args, cur = 0, [], ""
    for ch in text[text.index(" ") + 1:]:
        if ch in "({":
            depth += 1
        elif ch in ")}":
            if depth == 0:
                break
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur)
            cur = ""
        else:
            cur += ch
    return args + [cur]


def named_files(name, rules, variables, files):
    """Tracked files a check's recipe and its prerequisites' recipes in this Makefile name."""
    out, seen, todo = set(), set(), [name]
    while todo:
        t = todo.pop()
        if t in seen:
            continue
        seen.add(t)
        if t in files:
            out.add(t)
        prereqs, commands = rules.get(t, ((), ()))
        todo += prereqs
        for c in commands:
            for tok in re.split(r"[\s'\"<>|;&()=,]+", expand_vars(c, variables)):
                tok = tok.lstrip("@").removeprefix("-I")
                tok = os.path.normpath(tok) if tok else tok
                if tok in files:
                    out.add(tok)
    return out


def audit():
    """Lists files a check depends on that its tests/check-paths.tsv rule doesn't cover: those its
    recipe (or its prerequisites' recipes) name and what they #include, and what each device
    family's own source files #include. Also recipe lines make might not run as written, which
    would stop a pass ever being recorded. Exit status 1 when anything is listed."""
    macros, maprules = load_map()
    db = make_database()
    rules, variables = parse_database(db)
    defines = re.findall(r"^define (\S+)", db, re.M)
    files = tracked()
    problems = 0
    for name in check_names(rules):
        pats = watched(name, macros, maprules)
        if pats is None:
            continue
        named = named_files(name, rules, variables, files)
        need = include_closure(named, files) | named
        missing = sorted(p for p in need if not matches(p, pats))
        for p in missing:
            print("%s: %s is not covered" % (name, p))
        problems += len(missing)
        for c in rules.get(name, ((), ()))[1]:
            if (re.match(r"[@+-]*\$\((foreach|call|eval|and|or) ", c) or
                    re.match(r"[@+-]*\$\(if ", c) and len(function_args(c)) < 3) or \
                    any(re.search(r"\$[({]%s[)}]" % re.escape(d), c) for d in defines):
                print("%s: a recipe line may expand to no line or several: %s" % (name, c.split("\n")[0][:60]))
                problems += 1
    # Each family's own files: those its rule matches and no other family rule does.
    families = [(n, p) for n, p in maprules if n != ["*"] and n != ["test-*"]]
    shared = [x for n, p in maprules if n == ["*"] for x in p]
    def expanded(paths):
        out = []
        for t in paths:
            bang = t.startswith("!")
            t2 = t[1:] if bang else t
            out += [("!" if bang else "") + x for x in macros.get(t2, [t2])]
        return out
    for n, paths in families:
        own = expanded(paths)
        others = [expanded(p) for m, p in families if m != n]
        start = [f for f in files if SOURCE.search(f) and matches(f, own)
                 and not any(matches(f, o) for o in others)]
        cover = own + expanded(shared)
        missing = sorted(p for p in include_closure(start, files) if not matches(p, cover))
        for p in missing:
            print("%s: %s is included by the family's own files but not covered" % (" ".join(n), p))
        problems += len(missing)
    print("audit: %d problems" % problems)
    return 1 if problems else 0


def main(argv):
    if len(argv) >= 5 and argv[1] == "record":
        record(argv[2], argv[3], argv[4])
        return 0
    if len(argv) >= 4 and argv[1] == "begin":
        begin(int(argv[2]), argv[3])
        return 0
    if len(argv) >= 6 and argv[1] == "line":
        line_result(int(argv[2]), argv[3], argv[4], argv[5])
        return 0
    if len(argv) >= 2 and argv[1] == "audit":
        return audit()
    if len(argv) >= 2 and argv[1] in ("list", "recheck"):
        todo = plan(argv[2:])
        for name, why in todo:
            print("%-40s %s" % (name, why))
        print("%d checks to run" % len(todo))
        if argv[1] == "list":
            return 0
        failed = []
        for name, _ in todo:
            start, commit, tree = time.time(), head(), tree_state()
            fd, skipfile = tempfile.mkstemp(prefix="check-skip-")
            os.close(fd)
            env = dict(os.environ, FROND_NO_RECORD="1", FROND_SKIP_FILE=skipfile)
            rc = subprocess.run(["make", "-C", ROOT, name], env=env).returncode
            skipped = os.path.getsize(skipfile) > 0
            os.unlink(skipfile)
            record(name, "fail" if rc else "skip" if skipped else "pass", time.time() - start, commit, tree)
            if rc:
                failed.append(name)
        print("recheck: %d run, %d failed%s" % (len(todo), len(failed), ": " + " ".join(failed) if failed else ""))
        return 1 if failed else 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
