#!/usr/bin/env python3
"""Code hygiene counts (ADR-0020, M0-09).

Scans kernel/, libc/, userland/ and stack/ (C sources and headers, vendored files
skipped) and prints one `ARC_TEST budget <name> <value>` line per metric, the
format tools/budget-check.py reads, followed by where each count comes from:

    dup_windows                duplicated code regions: runs of 10 or more
                               normalized lines (comments, blank lines, lone
                               braces, preprocessor lines and whitespace
                               removed) that also appear elsewhere
                               on the same side (kernel, or libc + userland)
    dup_static_names           extra copies of static functions: a name defined
                               in N files on the same side adds N - 1 (minus
                               ALLOWED_STATIC_DUPS)
    arch_includes_outside_arch #include of arch/x86_64/... outside kernel/arch/
    long_functions             function bodies over 100 lines
    untracked_todos            TODO/FIXME without a block ID (M1-07) or DEFERRED

Kernel vs user space is a real boundary, so the same code in kernel/lib and
libc is not a duplicate. Limits live in tests/budgets.txt (source `hygiene`)
and only go down.

It also checks that the syscalls with a handler (syscall_register calls) match
the rows marked done or partial in the syscall table; a mismatch exits 1.

    tools/hygiene.py                       counts + locations
    tools/hygiene.py --out build/hygiene.log
    tools/hygiene.py --root DIR --expect dup_windows=2 ...   (self-test)

Exit status: 0 = ok, 1 = syscall table mismatch or --expect failed.
Python 3 standard library only.
"""

import argparse
import hashlib
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_DIRS = ("kernel", "libc", "userland", "stack")
VENDORED = {"kernel/include/limine.h"}
SYSCALL_TABLE = ".claude/skills/arc-os-architecture/reference/syscall-table.md"

WINDOW = 10            # lines per duplicate window
LONG_FUNCTION = 100    # lines

# static function names allowed in more than one file, with the reason
ALLOWED_STATIC_DUPS = {
    # name: reason
}

TODO_RE = re.compile(r"\b(TODO|FIXME)\b")
TRACKED_RE = re.compile(r"\bM\d-\d\d[a-z]?\b|DEFERRED")
ARCH_INC_RE = re.compile(r'^\s*#\s*include\s*["<][^">]*\barch/x86_64/')
STATIC_FN_RE = re.compile(r"^(?:__attribute__\(\(.*?\)\)\s+)?static\s+(?:inline\s+)?(?:[A-Za-z_][\w\s\*]*?[\s\*])"
                          r"([A-Za-z_]\w*)\s*\([^;]*$")
FN_START_RE = re.compile(r"^[A-Za-z_][\w\s\*,()\[\]]*?\b([A-Za-z_]\w*)\s*\(")
NOT_FUNCTIONS = {"if", "for", "while", "switch", "return", "sizeof", "do"}


def source_files(root):
    out = []
    for d in SCAN_DIRS:
        base = os.path.join(root, d)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for fn in sorted(filenames):
                if not fn.endswith((".c", ".h")):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, fn), root).replace(os.sep, "/")
                if rel not in VENDORED:
                    out.append(rel)
    return out


def side(rel):
    return "kernel" if rel.startswith("kernel/") else "user"


def _scan(text, keep_comments):
    """Blank out string and char literals, and comments unless keep_comments,
    keeping newlines so line numbers and brace structure survive. With
    keep_comments the code itself is blanked instead and only comments remain."""
    out = []
    i, n = 0, len(text)
    blank = lambda t: re.sub(r"[^\n]", " ", t)
    while i < n:
        c = text[i]
        if text.startswith("/*", i) or text.startswith("//", i):
            if text[i + 1] == "*":
                j = text.find("*/", i + 2)
                j = n if j < 0 else j + 2
            else:
                j = text.find("\n", i)
                j = n if j < 0 else j
            out.append(text[i:j] if keep_comments else blank(text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            lit = text[i:j]
            out.append(blank(lit) if keep_comments else
                       c + " " * max(0, len(lit) - 2) + (c if len(lit) >= 2 else ""))
            i = j
        else:
            out.append(blank(c) if keep_comments else c)
            i += 1
    return "".join(out)


def strip_code(text):
    """Code with comments and literal contents blanked."""
    return _scan(text, keep_comments=False)


def comments_only(text):
    """Only the comments of a file (for TODO scanning), newlines kept."""
    return _scan(text, keep_comments=True)


# ---- metrics ----

def dup_windows(files):
    """Count duplicated regions and describe them."""
    norm = {}        # rel -> [(lineno, normalized)]
    for rel, (_, code) in files.items():
        lines = []
        for no, line in enumerate(code.split("\n"), 1):
            s = re.sub(r"\s+", "", line)
            if s and s not in ("{", "}", "};") and not s.startswith("#"):
                lines.append((no, s))
        norm[rel] = lines
    where = defaultdict(set)      # (side, hash) -> {rel}
    hashes = {}
    for rel, lines in norm.items():
        hs = []
        for k in range(len(lines) - WINDOW + 1):
            h = hashlib.sha1("\n".join(s for _, s in lines[k:k + WINDOW]).encode()).hexdigest()
            hs.append(h)
            where[(side(rel), h)].add((rel, k))
        hashes[rel] = hs
    regions = []
    for rel, hs in hashes.items():
        lines = norm[rel]
        k = 0
        while k < len(hs):
            if len(where[(side(rel), hs[k])]) > 1:
                start = k
                while k + 1 < len(hs) and len(where[(side(rel), hs[k + 1])]) > 1:
                    k += 1
                regions.append(f"{rel}:{lines[start][0]}-{lines[k + WINDOW - 1][0]}")
            k += 1
    return len(regions), regions


def dup_static_names(files):
    defs = defaultdict(set)       # (side, name) -> {rel}
    for rel, (_, code) in files.items():
        lines = code.split("\n")
        for i, line in enumerate(lines):
            m = STATIC_FN_RE.match(line)
            if not m:
                continue
            nxt = lines[i + 1].strip() if i + 1 < len(lines) else ""
            if "{" in line or nxt.startswith("{"):
                defs[(side(rel), m.group(1))].add(rel)
    found = sorted((name, sorted(rels)) for (sd, name), rels in defs.items()
                   if len(rels) > 1 and name not in ALLOWED_STATIC_DUPS)
    # Each copy beyond the first counts, so a third copy of a known duplicate still raises it
    return sum(len(rels) - 1 for _, rels in found), [f"{name}: {', '.join(rels)}" for name, rels in found]


def arch_includes(files):
    found = []
    for rel, (text, _) in files.items():
        if rel.startswith("kernel/arch/"):
            continue
        for no, line in enumerate(text.split("\n"), 1):
            if ARCH_INC_RE.match(line):
                found.append(f"{rel}:{no}: {line.strip()}")
    return len(found), found


def long_functions(files):
    found = []
    for rel, (_, code) in files.items():
        if not rel.endswith(".c"):
            continue
        lines = code.split("\n")
        i = 0
        while i < len(lines):
            line = lines[i]
            m = FN_START_RE.match(line)
            opens = None
            if m and m.group(1) not in NOT_FUNCTIONS and not line.rstrip().endswith(";"):
                # K&R: '{' ends this line (or a continuation line), or opens the next line
                for j in range(i, min(i + 6, len(lines))):
                    if lines[j].rstrip().endswith(";"):
                        break
                    if "{" in lines[j]:
                        opens = j
                        break
            if opens is None:
                i += 1
                continue
            depth, j = 0, opens
            end = None
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                if depth <= 0:
                    end = j
                    break
                j += 1
            if end is None:
                break
            length = end - i + 1
            if length > LONG_FUNCTION:
                found.append(f"{rel}:{i + 1}: {m.group(1)} ({length} lines)")
            i = end + 1
    return len(found), found


def untracked_todos(files):
    found = []
    for rel, (text, _) in files.items():
        com = comments_only(text).split("\n")
        for no, line in enumerate(com, 1):
            if TODO_RE.search(line) and not TRACKED_RE.search(line):
                found.append(f"{rel}:{no}: {line.strip()[:80]}")
    return len(found), found


def syscall_table_check(root, files):
    """[] when handlers and done/partial rows match, else problems."""
    table = os.path.join(root, SYSCALL_TABLE)
    if not os.path.isfile(table):
        return []
    handlers = set()
    for rel, (_, code) in files.items():
        handlers.update(re.findall(r"syscall_register\s*\(\s*__NR_(\w+)", code))
    rows = set()
    with open(table) as f:
        for line in f:
            cols = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cols) >= 3 and cols[0].isdigit() and re.match(r"(done|partial)\b", cols[2]):
                rows.add(cols[1])
    problems = [f"handler without a done/partial row: {n}" for n in sorted(handlers - rows)]
    problems += [f"done/partial row without a handler: {n}" for n in sorted(rows - handlers)]
    return problems


METRICS = (("dup_windows", dup_windows), ("dup_static_names", dup_static_names),
           ("arch_includes_outside_arch", arch_includes), ("long_functions", long_functions),
           ("untracked_todos", untracked_todos))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=ROOT, help="tree to scan (default: the repo)")
    ap.add_argument("--out", help="also write the budget lines to this file")
    ap.add_argument("--quiet", action="store_true", help="budget lines only, no locations")
    ap.add_argument("--expect", action="append", default=[], metavar="NAME=VALUE",
                    help="self-test: fail unless the metric has this value")
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    files = {}
    for rel in source_files(root):
        with open(os.path.join(root, rel), errors="replace") as f:
            text = f.read()
        files[rel] = (text, strip_code(text))

    results = {}
    budget_lines = []
    for name, fn in METRICS:
        count, where = fn(files)
        results[name] = count
        budget_lines.append(f"ARC_TEST budget {name} {count}")
        if not args.quiet and where:
            print(f"# {name}: {count}")
            for w in where:
                print(f"#   {w}")
    for l in budget_lines:
        print(l)
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w") as f:
            f.write("\n".join(budget_lines) + "\n")

    status = 0
    problems = syscall_table_check(root, files)
    for p in problems:
        print(f"hygiene: syscall table: {p} ({SYSCALL_TABLE})")
        status = 1
    for e in args.expect:
        name, _, want = e.partition("=")
        got = results.get(name)
        if got is None or got != int(want):
            print(f"hygiene: expected {name}={want}, got {got}")
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
