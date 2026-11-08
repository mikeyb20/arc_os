#!/usr/bin/env python3
"""Compare measured sizes and counts with their limits (ADR-0019, M0-08).

Limits live in tests/budgets.txt, one metric per line:

    <name> <limit> <max|min> <unit> <owner-block> <source> [off]

`source` says where the value comes from:
    build      measured here from the cross build (kernel.elf)
    hygiene    the lines tools/hygiene.py wrote to <build-dir>/hygiene.log
    <suite>    the `ARC_TEST budget <name> <value>` line in that QEMU suite's
               serial log (build/qemu-test/<suite>.serial.log), or in a lane
               test's <suite>.budget.log (--log-dir build-lane/stack/budgets, M2-01)
    *          every suite log that reports it; the worst value counts
`off` lists a metric that is not measured yet (shown, never checked).

A `max` metric fails when the value is more than 5% above its limit (hygiene
counts: any value above it) and warns
when it is more than 20% below (tighten the limit); `min` is the mirror image.
A metric that is not reported fails. Counts and sizes only: never time.

Exit status: 0 = within budget, 1 = a budget failed, 2 = harness error.
With --expect-fail (harness self-check), 0 means exactly the named metrics failed.
Python 3 standard library only.
"""

import argparse
import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FAIL_MARGIN = 0.05    # fail past limit by more than 5%
WARN_MARGIN = 0.20    # warn when better than the limit by more than 20%

BUDGET_RE = re.compile(r"^ARC_TEST budget (\S+) (-?\d+)\s*$")


def log(msg):
    print(f"budget-check: {msg}", flush=True)


def parse_budgets(path):
    """Return a list of dicts, one per metric line."""
    metrics = []
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) not in (6, 7) or parts[2] not in ("max", "min") \
                    or (len(parts) == 7 and parts[6] != "off"):
                raise ValueError(f"{path}:{lineno}: expected "
                                 "'<name> <limit> <max|min> <unit> <owner> <source> [off]'")
            metrics.append({"name": parts[0], "limit": int(parts[1]), "kind": parts[2],
                            "unit": parts[3], "owner": parts[4], "source": parts[5],
                            "off": len(parts) == 7})
    return metrics


def elf_alloc_bytes(path):
    """Sum of the SHF_ALLOC sections of an ELF64 file: .text + .rodata + .data + .bss
    (what `size` prints as dec)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise ValueError(f"{path}: not an ELF64 file")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    total = 0
    for i in range(shnum):
        off = shoff + i * shentsize
        flags, = struct.unpack_from("<Q", data, off + 8)
        size, = struct.unpack_from("<Q", data, off + 32)
        if flags & 0x2:            # SHF_ALLOC
            total += size
    return total


def build_values(build_dir):
    kernel = os.path.join(build_dir, "kernel.elf")
    if not os.path.isfile(kernel):
        return {}
    return {"kernel_image_bytes": elf_alloc_bytes(kernel)}


LOG_SUFFIXES = (".serial.log", ".budget.log")


def log_values(log_dir):
    """{suite: {name: value}} from every <suite>.serial.log or <suite>.budget.log in log_dir."""
    out = {}
    paths = sorted(p for sfx in LOG_SUFFIXES for p in glob.glob(os.path.join(log_dir, "*" + sfx)))
    for path in paths:
        base = os.path.basename(path)
        suite = next(base[:-len(s)] for s in LOG_SUFFIXES if base.endswith(s))
        vals = {}
        with open(path, errors="replace") as f:
            for line in f:
                m = BUDGET_RE.match(line.rstrip("\r\n"))
                if m:
                    vals[m.group(1)] = int(m.group(2))
        out[suite] = vals
    return out


def hygiene_values(build_dir):
    """{name: value} from tools/hygiene.py's output, or None if it hasn't run."""
    path = os.path.join(build_dir, "hygiene.log")
    if not os.path.isfile(path):
        return None
    vals = {}
    with open(path) as f:
        for line in f:
            m = BUDGET_RE.match(line.rstrip("\r\n"))
            if m:
                vals[m.group(1)] = int(m.group(2))
    return vals


def lookup(metric, build, logs, hygiene):
    """(value, where) for a metric, or (None, why)."""
    src, name = metric["source"], metric["name"]
    if src == "build":
        return (build[name], "build") if name in build else (None, "not measured by the build")
    if src == "hygiene":
        if hygiene is None:
            return None, "no hygiene.log (run tools/hygiene.py --out <build-dir>/hygiene.log)"
        return (hygiene[name], "hygiene") if name in hygiene else (None, "hygiene.py did not report it")
    if src == "*":
        found = [(v[name], s) for s, v in logs.items() if name in v]
        if not found:
            return None, "no suite log reports it"
        pick = max if metric["kind"] == "max" else min
        return pick(found, key=lambda t: t[0])
    if src not in logs:
        return None, f"no log for suite '{src}' (run it first)"
    if name not in logs[src]:
        return None, f"suite '{src}' did not report it"
    return logs[src][name], src


def fail_margin(metric):
    """Hygiene counts are exact ratchets (ADR-0020): one more is a failure."""
    return 0.0 if metric["source"] == "hygiene" else FAIL_MARGIN


def judge(metric, value):
    """'ok', 'warn' or 'FAIL' for a measured value."""
    limit = metric["limit"]
    margin = fail_margin(metric)
    if metric["kind"] == "max":
        if value > limit * (1 + margin):
            return "FAIL"
        if limit > 0 and value < limit * (1 - WARN_MARGIN):
            return "warn"
    else:
        if value < limit * (1 - margin):
            return "FAIL"
        if value > limit * (1 + WARN_MARGIN):
            return "warn"
    return "ok"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--budgets", default=os.path.join(ROOT, "tests", "budgets.txt"))
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    ap.add_argument("--log-dir", default=None,
                    help="serial logs (default: <build-dir>/qemu-test)")
    ap.add_argument("--only-source", action="append", default=[], metavar="SOURCE",
                    help="check only metrics with this source (build, hygiene, a suite, *)")
    ap.add_argument("--skip-source", action="append", default=[], metavar="SOURCE",
                    help="leave out metrics with this source")
    ap.add_argument("--expect-fail", action="append", default=[], metavar="NAME",
                    help="self-check: pass only if exactly these metrics fail")
    args = ap.parse_args()
    log_dir = args.log_dir or os.path.join(args.build_dir, "qemu-test")

    try:
        metrics = parse_budgets(args.budgets)
        build = build_values(args.build_dir)
    except (OSError, ValueError) as e:
        log(str(e))
        return 2
    logs = log_values(log_dir)
    hygiene = hygiene_values(args.build_dir)
    metrics = [m for m in metrics
               if (not args.only_source or m["source"] in args.only_source)
               and m["source"] not in args.skip_source]

    rows, failed, warned, failed_names = [], [], [], []
    for m in metrics:
        if m["off"]:
            rows.append((m["name"], "-", f"{m['kind']} {m['limit']}", m["unit"], "off", f"not measured until {m['owner']}"))
            continue
        value, where = lookup(m, build, logs, hygiene)
        if value is None:
            rows.append((m["name"], "-", f"{m['kind']} {m['limit']}", m["unit"], "FAIL", where))
            failed.append(f"{m['name']}: not reported ({where})")
            failed_names.append(m["name"])
            continue
        verdict = judge(m, value)
        rows.append((m["name"], str(value), f"{m['kind']} {m['limit']}", m["unit"], verdict, where))
        if verdict == "FAIL":
            failed_names.append(m["name"])
            failed.append(f"{m['name']} = {value} {m['unit']}, limit {m['kind']} {m['limit']} "
                          f"({'+' if m['kind'] == 'max' else '-'}{int(fail_margin(m) * 100)}%), from {where}; "
                          f"owner {m['owner']}")
        elif verdict == "warn":
            warned.append(f"{m['name']} = {value}, limit {m['limit']}: more than "
                          f"{int(WARN_MARGIN * 100)}% better, tighten the limit")

    head = ("metric", "value", "limit", "unit", "result", "from")
    widths = [max(len(r[i]) for r in rows + [head]) for i in range(len(head))]
    for r in [head] + rows:
        print("  ".join(c.ljust(w) for c, w in zip(r, widths)).rstrip())
    for w in warned:
        log(f"warn {w}")
    for f in failed:
        log(f"FAIL {f}")
    log(f"{len(metrics)} metrics: {len(failed)} failed, {len(warned)} warnings")
    if args.expect_fail:
        if sorted(failed_names) == sorted(args.expect_fail):
            log("self-check: exactly the expected metrics failed")
            return 0
        log(f"self-check: expected failures {sorted(args.expect_fail)}, got {sorted(failed_names)}")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
