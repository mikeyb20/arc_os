#!/usr/bin/env python3
"""Merge strace logs into the syscall inventory (M0-04).

    syscall-inventory.py --logs build/inventory/logs --tbl <syscall_64.tbl> \
        --table .claude/skills/arc-os-architecture/reference/syscall-table.md \
        --status build/inventory/status.txt --out docs/plans/syscall-inventory.md

Reads every <program>.strace in --logs (strace -f -qq output) and writes one
table: syscall, Linux number (from the archived syscall_64.tbl), the programs
that used it, call count, and the flags, ioctl requests, fcntl commands, socket
families and similar details seen; then the files opened under /proc, /sys,
/dev and /etc. The generated part of --out sits between BEGIN/END markers, so
hand-written text around it (the summary) survives a re-run.
"""

import argparse
import collections
import os
import re
import sys

BEGIN = "<!-- BEGIN GENERATED (tools/syscall-inventory.py) -->"
END = "<!-- END GENERATED -->"

# Lines from strace -f: "PID name(args) = ret", "PID name(args <unfinished ...>",
# "PID <... name resumed>...". Only call starts are counted.
CALL_RE = re.compile(r"^\s*(\d+)\s+(?:\d+\.\d+\s+)?([a-z_][a-z0-9_]*)\((.*)$")

# Which argument (0-based) carries a detail worth recording, per syscall.
DETAIL_ARGS = {
    "open": {1: "flags"}, "openat": {2: "flags"}, "openat2": {2: "how"},
    "ioctl": {1: "request"}, "fcntl": {1: "cmd"},
    "socket": {0: "family", 1: "type"}, "socketpair": {0: "family", 1: "type"},
    "mmap": {2: "prot", 3: "flags"}, "mprotect": {2: "prot"}, "madvise": {2: "advice"},
    "mremap": {3: "flags"}, "futex": {1: "op"}, "prctl": {0: "option"},
    "arch_prctl": {0: "code"}, "rt_sigprocmask": {0: "how"}, "rt_sigaction": {0: "signal"},
    "clock_gettime": {0: "clock"}, "clock_nanosleep": {0: "clock", 1: "flags"},
    "clock_getres": {0: "clock"}, "prlimit64": {1: "resource"}, "getrlimit": {0: "resource"},
    "newfstatat": {3: "flags"}, "statx": {2: "flags", 3: "mask"}, "faccessat": {2: "mode"},
    "faccessat2": {2: "mode", 3: "flags"}, "access": {1: "mode"}, "getrandom": {2: "flags"},
    "wait4": {2: "options"}, "pipe2": {1: "flags"}, "dup3": {2: "flags"},
    "membarrier": {0: "cmd"}, "sched_setaffinity": {}, "lseek": {2: "whence"},
    "kill": {1: "signal"}, "tkill": {1: "signal"}, "tgkill": {2: "signal"},
    "unlinkat": {2: "flags"}, "mkdirat": {}, "renameat2": {4: "flags"},
    "fadvise64": {3: "advice"}, "eventfd2": {1: "flags"}, "epoll_create1": {0: "flags"},
    "timerfd_create": {0: "clock", 1: "flags"}, "memfd_create": {1: "flags"},
    "set_robust_list": {}, "rseq": {},
}
# Named arguments strace prints as "name=VALUE" (clone, clone3 structs).
NAMED_DETAILS = {"clone": ("flags",), "clone3": ("flags", "exit_signal")}

# Path arguments (0-based) whose value is recorded as a file the program touched.
PATH_ARGS = {
    "open": 0, "openat": 1, "openat2": 1, "stat": 0, "lstat": 0, "newfstatat": 1, "statx": 1,
    "access": 0, "faccessat": 1, "faccessat2": 1, "readlink": 0, "readlinkat": 1,
    "execve": 0, "statfs": 0, "chdir": 0, "mkdir": 0, "mkdirat": 1,
}
FILE_PREFIXES = ("/proc/", "/sys/", "/dev/", "/etc/")
TOKEN_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")


def split_args(s):
    """Split a strace argument list at top-level commas. Stops at the closing ')'."""
    args, depth, cur, quote, i = [], 0, [], False, 0
    while i < len(s):
        ch = s[i]
        if quote:
            cur.append(ch)
            if ch == "\\" and i + 1 < len(s):
                cur.append(s[i + 1])
                i += 1
            elif ch == '"':
                quote = False
        elif ch == '"':
            quote = True
            cur.append(ch)
        elif ch in "([{":
            depth += 1
            cur.append(ch)
        elif ch in ")]}":
            if depth == 0:
                break
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    tail = "".join(cur).strip()
    if tail.endswith("<unfinished ...>"):
        tail = tail[: -len("<unfinished ...>")].strip()
    if tail:
        args.append(tail)
    return args


def symbols(value):
    """Symbolic names in a decoded strace value, e.g. 'O_RDONLY|O_CLOEXEC' -> both."""
    value = re.sub(r"^\w+=", "", value.strip())
    if value.startswith("_IOC("):
        return [value[:60]]
    out = []
    for part in re.split(r"[|\s]+", value.strip("{}[]")):
        part = part.strip()
        if TOKEN_RE.match(part):
            out.append(part)
    return out


def normalize_path(p):
    p = re.sub(r"^/proc/\d+/", "/proc/<pid>/", p)
    p = re.sub(r"/task/\d+/", "/task/<tid>/", p)
    p = re.sub(r"(/sys/devices/system/cpu/cpu)\d+", r"\1<n>", p)
    p = re.sub(r"(/sys/devices/system/node/node)\d+", r"\1<n>", p)
    return p


def parse_log(path, program, calls, details, files):
    with open(path, errors="replace") as f:
        for line in f:
            m = CALL_RE.match(line)
            if not m:
                continue
            name, rest = m.group(2), m.group(3)
            calls[name][program] += 1
            if name not in DETAIL_ARGS and name not in NAMED_DETAILS and name not in PATH_ARGS:
                continue
            args = split_args(rest)
            for idx, label in DETAIL_ARGS.get(name, {}).items():
                if idx < len(args):
                    for sym in symbols(args[idx]):
                        details[name][f"{label}: {sym}"].add(program)
            for key in NAMED_DETAILS.get(name, ()):
                for a in args:
                    for sub in re.split(r",\s*", a.strip("{}")):
                        if sub.startswith(key + "="):
                            for sym in symbols(sub):
                                details[name][f"{key}: {sym}"].add(program)
            idx = PATH_ARGS.get(name)
            if idx is not None and idx < len(args) and args[idx].startswith('"'):
                p = args[idx].strip('"').rstrip(".")
                if p.startswith(FILE_PREFIXES):
                    files[normalize_path(p)].add(program)


def load_tbl(path):
    tbl = {}
    with open(path) as f:
        for line in f:
            parts = line.split("#")[0].split()
            if len(parts) >= 3 and parts[1] in ("common", "64"):
                tbl[parts[2]] = int(parts[0])
    return tbl


def load_status(table_path):
    """Map syscall name -> status from syscall-table.md ('| # | name | status |')."""
    status = {}
    if not table_path or not os.path.exists(table_path):
        return status
    with open(table_path) as f:
        for line in f:
            m = re.match(r"^\|\s*\d+\s*\|\s*(\w+)\s*\|\s*([^|]+?)\s*\|", line)
            if m:
                status[m.group(1)] = m.group(2)
    return status


def render(calls, details, files, tbl, status, programs, build_status):
    out = [BEGIN, ""]
    out.append(f"Programs: {', '.join(programs)}. Numbers from the archived Linux v6.12 `syscall_64.tbl`.")
    out.append("")
    if build_status:
        out.append("Build and run status:")
        out.append("")
        out.extend(f"- `{l}`" for l in build_status)
        out.append("")
    out.append(f"### Syscalls ({len(calls)})")
    out.append("")
    out.append("| # | Syscall | arc_os today | Calls | Programs | Flags, requests and arguments seen |")
    out.append("|---|---|---|---|---|---|")
    for name in sorted(calls, key=lambda n: (tbl.get(n, 10000), n)):
        num = tbl.get(name, "?")
        progs = calls[name]
        total = sum(progs.values())
        seen = ", ".join(sorted(f"`{d}`" for d in details.get(name, {}))) or ""
        out.append(f"| {num} | {name} | {status.get(name, 'missing')} | {total} | "
                   f"{', '.join(sorted(progs))} | {seen} |")
    out.append("")
    out.append(f"### Files under /proc, /sys, /dev, /etc ({len(files)})")
    out.append("")
    out.append("| Path | Programs |")
    out.append("|---|---|")
    for p in sorted(files):
        out.append(f"| `{p}` | {', '.join(sorted(files[p]))} |")
    out.append("")
    out.append(END)
    return "\n".join(out)


def write_doc(path, generated):
    if os.path.exists(path):
        with open(path) as f:
            text = f.read()
        if BEGIN in text and END in text:
            head, rest = text.split(BEGIN, 1)
            tail = rest.split(END, 1)[1]
            text = head + generated + tail
        else:
            text = text.rstrip("\n") + "\n\n" + generated + "\n"
    else:
        text = "# Syscall inventory (M0-04)\n\n" + generated + "\n"
    with open(path, "w") as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--logs", required=True)
    ap.add_argument("--tbl", required=True)
    ap.add_argument("--table", help="syscall-table.md, for the 'arc_os today' column")
    ap.add_argument("--status", help="status.txt from run-in-alpine.sh")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    calls = collections.defaultdict(collections.Counter)
    details = collections.defaultdict(lambda: collections.defaultdict(set))
    files = collections.defaultdict(set)
    programs = []
    for fn in sorted(os.listdir(args.logs)):
        if fn.endswith(".strace"):
            prog = fn[: -len(".strace")]
            programs.append(prog)
            parse_log(os.path.join(args.logs, fn), prog, calls, details, files)
    if not programs:
        print("syscall-inventory: no .strace logs found", file=sys.stderr)
        return 1

    tbl = load_tbl(args.tbl)
    unknown = sorted(n for n in calls if n not in tbl)
    build_status = []
    if args.status and os.path.exists(args.status):
        with open(args.status) as f:
            build_status = [l.strip() for l in f if l.strip()]
    write_doc(args.out, render(calls, details, files, tbl, load_status(args.table), programs, build_status))
    print(f"syscall-inventory: {len(calls)} syscalls, {len(files)} files, {len(programs)} runs -> {args.out}")
    if unknown:
        print(f"syscall-inventory: not in syscall_64.tbl: {', '.join(unknown)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
