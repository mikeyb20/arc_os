#!/usr/bin/env python3
"""Write `ARC_TEST budget <name> <bytes>` lines for lane binaries (M2-01, ADR-0019).

    sizes.py <out.budget.log> <name>=<elf-path> ...

The value is the binary's loaded size (SHF_ALLOC sections: .text + .rodata + .data
+ .bss), measured the same way as kernel_image_bytes, so symbol tables and debug
info don't count. tools/budget-check.py reads the log (--log-dir).
"""

import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_spec = importlib.util.spec_from_file_location("budget_check", os.path.join(ROOT, "tools", "budget-check.py"))
budget_check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(budget_check)


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    lines = []
    for arg in argv[2:]:
        name, _, path = arg.partition("=")
        size = budget_check.elf_alloc_bytes(path)
        lines.append(f"ARC_TEST budget {name} {size}")
        print(f"{name} = {size} bytes ({path})")
    with open(argv[1], "w") as f:
        f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
