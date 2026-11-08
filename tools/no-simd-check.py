#!/usr/bin/env python3
"""kernel.no_simd (M1-01, ADR-0003): kernel code never uses vector registers.

The kernel is built with -mgeneral-regs-only, because while a thread is in the
kernel its x87/SSE/AVX registers still hold its user values. This check
disassembles kernel.elf and fails on any instruction that names an XMM, YMM,
ZMM or AVX-512 mask register, outside the functions listed in ALLOWED. The
XSAVE helpers in fpu.c (XSAVE, XSAVEOPT, XRSTOR, FXSAVE) name no register, so
the list is empty today.

    python3 tools/no-simd-check.py [--objdump x86_64-elf-objdump] build/kernel.elf

Exit status: 0 = clean, 1 = vector registers found, 2 = could not disassemble.
"""

import argparse
import re
import shutil
import subprocess
import sys

ALLOWED = set()   # function names allowed to touch vector registers

VECTOR_REG = re.compile(r"%(?:[xyz]mm[0-9]+|k[0-7])\b")
FUNC = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kernel", help="kernel.elf")
    ap.add_argument("--objdump", default=None, help="objdump to use")
    args = ap.parse_args()

    objdump = args.objdump or shutil.which("x86_64-elf-objdump") or shutil.which("objdump")
    if objdump is None:
        print("no-simd-check: no objdump found")
        return 2
    res = subprocess.run([objdump, "-d", "--no-show-raw-insn", args.kernel],
                         capture_output=True, text=True)
    if res.returncode != 0:
        print(f"no-simd-check: {objdump} failed: {res.stderr.strip()[-500:]}")
        return 2

    func, hits, insns = "?", [], 0
    for line in res.stdout.splitlines():
        m = FUNC.match(line)
        if m:
            func = m.group(1)
            continue
        if ":\t" not in line:
            continue
        insns += 1
        if VECTOR_REG.search(line) and func not in ALLOWED:
            hits.append(f"{func}: {line.strip()}")

    if insns == 0:
        print("no-simd-check: no instructions disassembled")
        return 2
    for h in hits[:20]:
        print(f"FAIL vector register in kernel code: {h}")
    if len(hits) > 20:
        print(f"... and {len(hits) - 20} more")
    print(f"no-simd-check: {insns} instructions, {len(hits)} use vector registers")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
