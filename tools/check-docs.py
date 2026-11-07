#!/usr/bin/env python3
"""Check that doc/developer-reference.md names every developer-facing item.

Collects command line options, console commands, control socket commands and events,
replay commands and keys, GDB monitor commands, environment variables, tools and make
targets from the sources and fails listing any that the reference does not mention in
backticks. Core sources come from build/source (after make prepare), or from the added
lines of patches/ when that isn't there. Needs no build and no dependencies.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / 'doc/developer-reference.md'

# Names that are real but not Frond's to document.
IGNORE = {
    'env': {'HOME', 'PATH', 'TMPDIR', 'CXX', 'SOURCE_DATE_EPOCH', 'DEB_HOST_ARCH', 'PULSE_PROP'},
    'monitor': {'tg50-original-pace-entry'},
    'target': {'FORCE'},
}


def read(path):
    try:
        return Path(path).read_text(errors='replace')
    except OSError:
        return ''


def core_text():
    """Text of the patched core sources, by relative path under src/."""
    source = ROOT / 'build/source/src'
    files = {}
    if source.is_dir():
        for path in source.rglob('*'):
            if path.suffix in ('.cpp', '.h') and path.is_file():
                files[str(path.relative_to(source))] = read(path)
        return files
    raise SystemExit('build/source is missing: run make prepare first')


def find(pattern, text, group=1):
    return set(m.group(group) for m in re.finditer(pattern, text, re.M))


def under(files, suffix):
    return '\n'.join(text for name, text in files.items() if name.endswith(suffix))


def collect():
    core = core_text()
    items = {}  # kind -> set of names
    main = under(core, 'uarm/native/main.cpp')
    commands = under(core, 'uarm/native/Commands.cpp') + under(core, 'common/Cli.cpp')
    control = under(core, 'native/DesktopControl.h')
    replay = under(core, 'native/InputReplay.cpp')
    gdb = under(core, 'uarm/gdbstub.cpp')

    items['option'] = find(r'"(--[a-z0-9-]+)"', ' '.join(re.findall(r'add_argument\([^)]*\)', main)))
    # Modes taken before the option parser, such as --card.
    items['option'] |= find(r'argv\[1\]\) == "(--[a-z0-9-]+)"', main)
    items['option'] |= find(r"add_argument\('(--[a-z0-9-]+)'", read(ROOT / 'tools/frond-run.py'))
    items['option'] |= {'--' + n for n in find(r'addOption\(\{"([a-z-]+)"', read(ROOT / 'desktop/main.cpp'))}
    items['console'] = find(r'\.name = "([a-z0-9-]+)"', commands)
    items['socket'] = find(r'command\s*==\s*"([a-z0-9-]+)"', control)
    items['socket'] |= {'cancel-files', 'panel-visible'}
    items['event'] = find(r'event\("([a-z0-9-]+)"', core_text_all(core))
    keys = re.search(r'keys = \{(.*?)\};', replay, re.S)
    items['replay key'] = find(r'\{"([a-z0-9-]+)",', keys.group(1)) if keys else set()
    items['replay command'] = find(r'command == "([a-z-]+)"', replay)
    items['monitor'] = find(r'cmd == "([a-z0-9-]+)(?: [a-z]+)?"', gdb) | find(r'cmd\.substr\(0, \d+\) == "([a-z-]+) "', gdb)
    items['monitor'] -= IGNORE['monitor']

    env = set()
    sources = list(core.values())
    for pattern in ('tools/*.cpp', 'tools/*.h', 'tools/*.py', 'tests/*.cpp', 'tests/*.py', 'tests/*.h',
                    'desktop/*.cpp', 'android/*.py', 'tests/palm/*.py'):
        sources += [read(p) for p in ROOT.glob(pattern)]
    sources = '\n'.join(sources)
    for pattern in (r'(?:getenv|qgetenv|qEnvironmentVariable|os\.getenv)\(\s*["\']([A-Z][A-Z0-9_]+)',
                    r'environ(?:\.get|\.setdefault)?[\[(]\s*["\']([A-Z][A-Z0-9_]+)',
                    r'\benv(?:\.get|\.setdefault)?[\[(]\s*["\']([A-Z][A-Z0-9_]+)',
                    r'dict\(os\.environ,\s*([A-Z][A-Z0-9_]+)='):
        env |= find(pattern, sources)
    makefile = read(ROOT / 'Makefile')
    env |= find(r'^([A-Z][A-Z0-9_]+)\s*\?=', makefile) | find(r'\$\(([A-Z][A-Z0-9_]*(?:_ARGS|_SESSION))\)', makefile)
    env -= {'CPU_TEST_LDLIBS', 'JIT_LIBS'}  # internal link flags
    items['env'] = env - IGNORE['env']

    items['tool'] = {p.name for p in (ROOT / 'tools').iterdir()
                     if p.suffix in ('.py', '.cpp') and p.is_file()}
    targets = find(r'^([a-zA-Z][a-zA-Z0-9_.-]*)\s*:(?!=)', makefile.replace('\\\n', ' '), 1)
    targets |= {m for m in re.findall(r'^([a-zA-Z][\w-]*)(?=:)', makefile, re.M)}
    items['target'] = (targets - IGNORE['target']) | {'out/frond'}
    return items


def core_text_all(core):
    return '\n'.join(core.values())


def doc_tokens():
    text = read(DOC)
    spans = re.findall(r'`([^`\n]+)`', text)
    tokens = set()
    for span in spans:
        tokens |= set(re.findall(r'-{0,2}[A-Za-z0-9_][A-Za-z0-9_./-]*', span))
        tokens.add(span.strip())
    return tokens


def main():
    if not DOC.is_file():
        print('missing', DOC.relative_to(ROOT))
        return 1
    tokens = doc_tokens()
    items = collect()
    missing = 0
    for kind in sorted(items):
        names = items[kind]
        if not names:
            print(f'check-docs: nothing found for {kind}; the collector is out of date')
            missing += 1
        for name in sorted(names):
            if name not in tokens and name.rstrip('.') not in tokens:
                print(f'missing {kind}: {name}')
                missing += 1
    total = sum(len(v) for v in items.values())
    if missing:
        print(f'check-docs: {missing} problems in {total} items')
        return 1
    print(f'check-docs: {total} items all in {DOC.relative_to(ROOT)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
