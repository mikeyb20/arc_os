#!/usr/bin/env python3
"""Check the QR codes arc_os drew against its serial log (M1-04c).

Called by tools/qemu-test.py --check after a run that took screendumps
(--screendump), as

    qr-check.py --kind report|log|panic SERIAL_LOG SHOT.ppm [SHOT.ppm ...]

Each screendump must hold exactly one ARC1 code, decoded with zbarimg
(zbar-tools, host only). For every page it checks the header (ARC1, the
build commit from the report's first line, the kind, n/N in order) and that
the page's CRC-32 matches its text; the last page's "all" CRC must match all
pages' text. Then the text itself:

  report  the pages' text is exactly the [REPORT] lines on serial, one '\\n'
          after each, as the kernel's sanitizer would write them
  log     the pages' lines appear on serial in the same order, each whole,
          and end with the last serial line before the first code (serial
          also carries lines that bypass the log, such as early boot stages)
  panic   the code holds the panic reason, the CPU, every register the
          serial report printed with the same value, and the backtrace's
          first frame

Exit status: 0 = all codes match, 1 = mismatch, 2 = usage or tool error.
Python 3 standard library plus the zbarimg program.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

HEADER = re.compile(r"ARC1 (\S+) (\S+) (\d+)/(\d+) ([0-9a-f]{8})\n")
ALL_LINE = re.compile(r"all ([0-9a-f]{8})\n\Z")


def fail(msg):
    print(f"qr-check: FAIL: {msg}", flush=True)
    sys.exit(1)


def ppm_to_pgm(src, dst):
    """Binary PPM (P6, 8-bit) to binary PGM (P5) by channel mean."""
    with open(src, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m or int(m.group(3)) != 255:
        raise ValueError(f"{src}: not an 8-bit binary PPM")
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():m.end() + w * h * 3]
    if len(px) != w * h * 3:
        raise ValueError(f"{src}: short pixel data")
    grey = bytes((px[i] + px[i + 1] + px[i + 2]) // 3 for i in range(0, len(px), 3))
    with open(dst, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h) + grey)
    return w, h


def decode(path):
    """Payload bytes of the one QR code in a screendump."""
    with tempfile.TemporaryDirectory() as tmp:
        pgm = os.path.join(tmp, "shot.pgm")
        w, h = ppm_to_pgm(path, pgm)
        res = subprocess.run(["zbarimg", "--raw", "-q", "-Sbinary", pgm], capture_output=True)
    if res.returncode != 0 or not res.stdout:
        fail(f"{os.path.basename(path)} ({w}x{h}): zbarimg found no code")
    if res.stdout.count(b"ARC1 ") != 1:
        fail(f"{os.path.basename(path)}: expected one ARC1 code, zbarimg read {res.stdout[:80]!r}")
    return res.stdout   # with -Sbinary, zbarimg writes the payload bytes as they are


def sanitize(text):
    """The kernel's qr_text_sanitize: printable ASCII and newline kept, any
    other byte as \\xNN, every "://" as ":\\/\\/"."""
    out = []
    for b in text.encode("latin-1", errors="replace"):
        out.append(chr(b) if b == 0x0A or 0x20 <= b <= 0x7E else f"\\x{b:02x}")
    return "".join(out).replace("://", ":\\/\\/")


def split_pages(payloads, kind):
    """Check headers and CRCs; returns (commit, [page text, ...])."""
    texts, commit, total = [], None, len(payloads)
    for i, raw in enumerate(payloads):
        try:
            p = raw.decode("ascii")
        except UnicodeDecodeError:
            fail(f"page {i + 1}: not plain ASCII")
        m = HEADER.match(p)
        if not m:
            fail(f"page {i + 1}: no ARC1 header: {p[:60]!r}")
        c, k, n, npages, crc = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), m.group(5)
        if k != kind:
            fail(f"page {i + 1}: kind {k}, expected {kind}")
        if n != i + 1 or npages != total:
            fail(f"page {i + 1}: header says {n}/{npages}, expected {i + 1}/{total}")
        if commit is None:
            commit = c
        elif c != commit:
            fail(f"page {i + 1}: commit {c}, page 1 says {commit}")
        body = p[m.end():]
        if n == npages:
            a = ALL_LINE.search(body)
            if not a:
                fail(f"last page: no 'all' line at the end")
            body = body[:a.start()]
            if format(zlib.crc32(body.encode()), "08x") != crc and body.endswith("\n"):
                body = body[:-1]   # the newline added before 'all' belongs to no page
            all_crc = a.group(1)
        if format(zlib.crc32(body.encode()), "08x") != crc:
            fail(f"page {n}: CRC {crc} does not match its text")
        texts.append(body)
    if format(zlib.crc32("".join(texts).encode()), "08x") != all_crc:
        fail("the 'all' CRC does not match the pages' text")
    return commit, texts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kind", required=True, choices=("report", "log", "panic"))
    ap.add_argument("serial_log")
    ap.add_argument("shots", nargs="+")
    args = ap.parse_args()
    if shutil.which("zbarimg") is None:
        print("qr-check: zbarimg not found: run tools/dev-setup.sh apt (zbar-tools)")
        return 2

    with open(args.serial_log, errors="replace") as f:
        serial = f.read().splitlines()
    payloads = [decode(s) for s in args.shots]
    commit, texts = split_pages(payloads, args.kind)
    text = "".join(texts)

    # The report's first line names the build; a panic before the report has
    # only the code's own header to go by
    first = next((l for l in serial if l.startswith("[REPORT] arc_os boot report, build ")), None)
    if first is not None and first.split()[-1] != commit:
        fail(f"codes say commit {commit}, the report says {first.split()[-1]}")
    if first is None and args.kind != "panic":
        fail("no '[REPORT] arc_os boot report, build ...' line on serial")
    if not re.fullmatch(r"[0-9a-f]{12}(-dirty)?|unknown", commit):
        fail(f"commit {commit!r} is not a 12-digit hash or 'unknown'")

    if args.kind == "report":
        want = sanitize("".join(l[len("[REPORT] "):] + "\n" for l in serial if l.startswith("[REPORT] ")))
        if text != want:
            for i, (a, b) in enumerate(zip(text, want)):
                if a != b:
                    break
            else:
                i = min(len(text), len(want))
            fail(f"report text differs from serial at byte {i}: codes {text[i:i + 60]!r}, serial {want[i:i + 60]!r}")
    elif args.kind == "log":
        # Serial also has lines written around the log (early boot stages go
        # straight to the port), so the log's lines must appear on serial in
        # order, each whole, ending with the last line before the first code
        if len(text) < 1000 or not text.endswith("\n"):
            fail(f"log text is {len(text)} bytes, or doesn't end with a whole line")
        shown = next(i for i, l in enumerate(serial) if l.startswith("ARC_QR shown "))
        want = [sanitize(l) for l in serial[:shown]]
        got = text.splitlines()
        pos = 0
        for line in got:
            while pos < len(want) and want[pos] != line:
                pos += 1
            if pos == len(want):
                fail(f"log line {line!r} is not on serial in this order")
            pos += 1
        if got[-1] != want[-1]:
            fail(f"log ends with {got[-1]!r}, serial's last line before the codes is {want[-1]!r}")
    else:
        regs = {}
        in_regs = False
        for l in serial:
            if l.startswith("PANIC: "):
                in_regs = True
                reason, cpu = re.match(r"PANIC: (.*) on CPU (\d+)$", l).groups()
                continue
            if in_regs and l.startswith("Backtrace:"):
                break
            if in_regs:
                regs.update(re.findall(r"(\w+) +([0-9a-f]{16})", l))
        if not regs:
            fail("no panic registers on serial")
        lines = text.splitlines()
        if lines[:2] != [f"PANIC {reason}", f"CPU {cpu}"]:
            fail(f"panic code starts {lines[:2]}, expected PANIC {reason} / CPU {cpu}")
        got = dict(re.findall(r"(\w+)=([0-9a-f]{16})", text))
        for name, value in regs.items():
            if got.get(name) != value:
                fail(f"register {name}: code has {got.get(name)}, serial has {value}")
        frame0 = next((l.split()[1:] for l in serial if re.match(r"\s+#0 ", l)), None)
        if frame0 is None or f"#0 {frame0[0]} {frame0[1]}" not in text:
            fail(f"backtrace frame #0 {frame0} not in the code")

    print(f"qr-check: {args.kind}: {len(texts)} code(s) decoded, {len(text)} bytes match serial (build {commit})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
