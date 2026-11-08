#!/usr/bin/env python3
"""Boot arc_os in QEMU in kernel test mode and report pass or fail (M0-02).

One run = one suite. The script builds a test ISO whose kernel command line is
`arc.test=<suite>` and which carries tests/qemu/suites/<suite>.suite plus any
extra modules, boots it headless, and waits for QEMU to exit. In test mode the
kernel ends the run itself through the isa-debug-exit device:

    init exits with status S  ->  QEMU exit code (S << 1) | 1   (S = 0 means pass)
    kernel panic              ->  QEMU exit code 255           (value 0x7f)

The serial log is parsed for ARC_TEST markers written by init:

    ARC_TEST PASS <name>
    ARC_TEST FAIL <name> <reason>
    ARC_TEST DONE pass=N fail=M
    ARC_TEST PANIC suite=<suite>          (written by the kernel)

Exit status of this script: 0 = suite passed, 1 = failed, 2 = harness error.
Python 3 standard library only.
"""

import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUITE_DIR = os.path.join(ROOT, "tests", "qemu", "suites")

PANIC_CODE = 0x7F
TAIL_LINES = 40


def log(msg):
    print(f"qemu-test: {msg}", flush=True)


def kvm_usable():
    return os.access("/dev/kvm", os.R_OK | os.W_OK)


def build_iso(args, iso):
    suite_file = os.path.join(SUITE_DIR, f"{args.suite}.suite")
    modules = []
    if os.path.isfile(suite_file):
        modules.append(suite_file)
    elif args.suite != "kpanic":
        log(f"no suite file {suite_file}")
        sys.exit(2)
    for m in args.module:
        path = m if os.path.isabs(m) else os.path.join(ROOT, m)
        if not os.path.isfile(path):
            # Test programs are build outputs: look in the build directory
            path = os.path.join(args.build_dir, m)
        modules.append(path)

    cmd = ["bash", os.path.join(ROOT, "tools", "make-iso.sh"),
           "-o", iso, "-c", f"arc.test={args.suite}"] + modules
    res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if res.returncode != 0:
        log("make-iso.sh failed:")
        print(res.stdout[-2000:] + res.stderr[-2000:])
        sys.exit(2)


def qemu_command(args, iso, serial_log):
    cmd = [args.qemu,
           "-machine", "q35",
           "-smp", str(args.smp),
           "-m", args.memory,
           "-display", "none",
           "-serial", f"file:{serial_log}",
           "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
           "-no-reboot",
           "-boot", "d",
           "-cdrom", iso]
    if kvm_usable() and not args.no_kvm:
        cmd += ["-accel", "kvm", "-cpu", "host"]
    else:
        cmd += ["-accel", "tcg,thread=multi", "-cpu", "max"]
    return cmd


def read_lines(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", required=True,
                    help="suite name: tests/qemu/suites/<suite>.suite ('kpanic' needs none)")
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build"),
                    help="cross build directory with kernel.elf and programs")
    ap.add_argument("--module", action="append", default=[],
                    help="extra file to put on the ISO (relative to the repo or build dir)")
    ap.add_argument("--expect", action="append", default=[],
                    help="regex that must appear in the serial log")
    ap.add_argument("--expect-panic", action="store_true",
                    help="the run passes only if the kernel panics (harness self-check)")
    ap.add_argument("--timeout", type=float, default=120, help="seconds")
    ap.add_argument("--memory", default="256M")
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--no-kvm", action="store_true", help="force TCG")
    args = ap.parse_args()
    args.build_dir = os.path.abspath(args.build_dir)

    if not os.path.isfile(os.path.join(args.build_dir, "kernel.elf")):
        log(f"{args.build_dir}/kernel.elf not found; run the cross build first")
        return 2

    out_dir = os.path.join(args.build_dir, "qemu-test")
    os.makedirs(out_dir, exist_ok=True)
    iso = os.path.join(out_dir, f"{args.suite}.iso")
    serial_log = os.path.join(out_dir, f"{args.suite}.serial.log")
    if os.path.exists(serial_log):
        os.remove(serial_log)

    build_iso(args, iso)
    cmd = qemu_command(args, iso, serial_log)
    accel = "kvm" if "kvm" in cmd else "tcg"

    start = time.monotonic()
    timed_out = False
    try:
        proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, timeout=args.timeout)
        rc = proc.returncode
        qemu_err = proc.stderr.strip()
    except subprocess.TimeoutExpired as e:
        timed_out = True
        rc = None
        qemu_err = (e.stderr or b"").decode(errors="replace").strip() if isinstance(e.stderr, bytes) else (e.stderr or "")
    elapsed = time.monotonic() - start

    lines = read_lines(serial_log)
    passes = [l.split(None, 2)[2] for l in lines if l.startswith("ARC_TEST PASS ")]
    fails = [l.split(None, 2)[2] for l in lines if l.startswith("ARC_TEST FAIL ")]
    done = next((l for l in lines if l.startswith("ARC_TEST DONE ")), None)
    panicked = any(l.startswith("ARC_TEST PANIC") for l in lines) or rc == ((PANIC_CODE << 1) | 1)
    text = "\n".join(lines)
    missing = [rx for rx in args.expect if not re.search(rx, text, re.MULTILINE)]

    # Decode the isa-debug-exit status: QEMU exits with (value << 1) | 1
    debug_value = (rc >> 1) if (rc is not None and rc & 1) else None

    head = f"suite {args.suite} ({accel}, {elapsed:.1f}s)"
    problems = []
    if args.expect_panic:
        if panicked:
            log(f"{head}: kernel panic after {elapsed:.1f}s, as expected")
            return 0
        problems.append("expected a kernel panic, none seen")
    else:
        if timed_out:
            problems.append(f"timeout after {args.timeout:.0f}s")
        elif panicked:
            problems.append(f"kernel panic after {elapsed:.1f}s")
        elif debug_value is None:
            problems.append(f"QEMU exited with {rc} without a test-mode exit "
                            "(triple fault, or init never finished)")
        elif debug_value != 0:
            problems.append(f"init exited with status {debug_value}")
        if done is None and not panicked:
            problems.append("no ARC_TEST DONE line")
        if not passes and not fails and not panicked:
            problems.append("no tests ran")
        problems.extend(f"FAIL {f}" for f in fails)
        problems.extend(f"expected output not found: /{rx}/" for rx in missing)

    if not problems:
        log(f"{head}: PASS ({len(passes)} passed: {', '.join(passes)})")
        return 0

    log(f"{head}: FAILED")
    for p in problems:
        log(f"  {p}")
    if qemu_err:
        log(f"  qemu stderr: {qemu_err[-500:]}")
    log(f"last {TAIL_LINES} serial lines ({serial_log}):")
    for l in lines[-TAIL_LINES:]:
        print(f"  | {l}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
