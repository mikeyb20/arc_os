#!/usr/bin/env python3
"""Host tests for tools/hygiene.py and its budget limits (M0-09, ADR-0020).

Builds a tiny tree with one known instance of each pattern and checks every
count, then checks that tools/budget-check.py treats hygiene counts as exact
ratchets (one more than the limit fails, naming the metric)."""

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HYGIENE = os.path.join(ROOT, "tools", "hygiene.py")
BUDGET = os.path.join(ROOT, "tools", "budget-check.py")
spec = importlib.util.spec_from_file_location("hygiene", HYGIENE)
hy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hy)

# Twelve distinct statements: copied into two kernel files = one duplicate region each
BLOCK = "\n".join(f"    total += table[{i}] * {i + 3};" for i in range(12))


def sum_fn(name):
    return f"int {name}(const int *table) {{\n    int total = 0;\n{BLOCK}\n    return total;\n}}\n"


FILES = {
    "kernel/mm/a.c": '#include "arch/hal.h"\n'
                     "static int helper(int x) {\n    return x + 1;\n}\n" + sum_fn("sum_a"),
    "kernel/fs/b.c": '#include "arch/x86_64/paging.h"\n'
                     "static int helper(int x) {\n    return x + 2;\n}\n" + sum_fn("sum_b") +
                     "/* TODO: untracked */\n/* TODO(M1-07): tracked */\n/* FIXME see DEFERRED.md */\n",
    # Same code on the user side is not a duplicate of kernel code
    "libc/src/c.c": "static int helper(int x) {\n    return x;\n}\n" + sum_fn("sum_c"),
    # Arch includes inside kernel/arch are fine; TODO in a string is not a comment
    "kernel/arch/x86_64/d.c": '#include "arch/x86_64/paging.h"\n'
                              'const char *s = "TODO in a string";\n',
    "userland/long/e.c": "int long_one(void) {\n    int n = 0;\n" +
                         "".join(f"    n += {i};\n" for i in range(100)) + "    return n;\n}\n"
                         "int short_one(void) {\n    return 1;\n}\n",
    "kernel/include/limine.h": "/* TODO vendored, skipped */\n",
    # A third copy (with a leading attribute) still counts: N files add N - 1
    "kernel/net/f.c": "__attribute__((unused)) static int helper(int x) {\n    return x * 3;\n}\n",
}


def write_tree(root, files):
    for rel, text in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)


class HygieneCounts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        write_tree(self.tmp.name, FILES)

    def tearDown(self):
        self.tmp.cleanup()

    def run_hygiene(self, *extra):
        return subprocess.run([sys.executable, HYGIENE, "--root", self.tmp.name, *extra],
                              capture_output=True, text=True)

    def test_every_count(self):
        res = self.run_hygiene("--expect", "dup_windows=2", "--expect", "dup_static_names=2",
                               "--expect", "arch_includes_outside_arch=1",
                               "--expect", "long_functions=1", "--expect", "untracked_todos=1")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("ARC_TEST budget dup_windows 2", res.stdout)
        self.assertIn("kernel/fs/b.c:1:", res.stdout)          # the arch include, located
        self.assertIn("long_one (104 lines)", res.stdout)
        self.assertIn("helper: kernel/fs/b.c, kernel/mm/a.c, kernel/net/f.c", res.stdout)

    def test_expect_mismatch_fails(self):
        res = self.run_hygiene("--expect", "dup_windows=0")
        self.assertEqual(res.returncode, 1)
        self.assertIn("expected dup_windows=0, got 2", res.stdout)

    def test_strip_code_keeps_lines(self):
        src = 'a = "/* not */"; /* x\ny */ b; // z\n'
        out = hy.strip_code(src)
        self.assertEqual(out.count("\n"), src.count("\n"))
        self.assertNotIn("not", out)
        self.assertIn("b;", out)


class SyscallTable(unittest.TestCase):
    def check(self, handlers, rows):
        with tempfile.TemporaryDirectory() as root:
            code = "".join(f"    syscall_register(__NR_{h}, sys_{h});\n" for h in handlers)
            write_tree(root, {"kernel/x_syscalls.c": "void reg(void) {\n" + code + "}\n"})
            table = os.path.join(root, hy.SYSCALL_TABLE)
            os.makedirs(os.path.dirname(table))
            with open(table, "w") as f:
                f.write("| Linux # | Name | Status |\n|---|---|---|\n")
                for i, (name, status) in enumerate(rows):
                    f.write(f"| {i} | {name} | {status} |\n")
            files = {}
            for rel in hy.source_files(root):
                with open(os.path.join(root, rel)) as f:
                    text = f.read()
                files[rel] = (text, hy.strip_code(text))
            return hy.syscall_table_check(root, files)

    def test_match(self):
        self.assertEqual(self.check(["read", "write"],
                                    [("read", "done"), ("write", "partial (x)"), ("mmap", "missing")]), [])

    def test_mismatch(self):
        probs = self.check(["read", "brk"], [("read", "done"), ("open", "partial (y)")])
        self.assertIn("handler without a done/partial row: brk", probs)
        self.assertIn("done/partial row without a handler: open", probs)


class HygieneBudgets(unittest.TestCase):
    def run_check(self, value, limit):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "hygiene.log"), "w") as f:
                f.write(f"ARC_TEST budget arch_includes_outside_arch {value}\n")
            budgets = os.path.join(d, "b.txt")
            with open(budgets, "w") as f:
                f.write(f"arch_includes_outside_arch {limit} max includes M0-09 hygiene\n"
                        "kernel_image_bytes 1 max bytes M0-08 build\n")
            return subprocess.run([sys.executable, BUDGET, "--budgets", budgets, "--build-dir", d,
                                   "--only-source", "hygiene"], capture_output=True, text=True)

    def test_one_more_fails_by_name(self):
        res = self.run_check(45, 44)          # 2% over: a size budget would pass, a count must not
        self.assertEqual(res.returncode, 1, res.stdout)
        self.assertIn("FAIL arch_includes_outside_arch = 45", res.stdout)

    def test_at_limit_passes_and_other_sources_skipped(self):
        res = self.run_check(44, 44)
        self.assertEqual(res.returncode, 0, res.stdout)
        self.assertNotIn("kernel_image_bytes", res.stdout)


if __name__ == "__main__":
    unittest.main()
