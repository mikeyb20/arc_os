#!/usr/bin/env python3
"""Host tests for the lane L helpers (M2-01): tools/lane/run.sh, tools/lane/sizes.py and
lane budget logs in tools/budget-check.py; tools/lane/port-env.sh (M2-02)."""

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, rel))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


budget_check = load("budget_check", "tools/budget-check.py")
RUN = os.path.join(ROOT, "tools", "lane", "run.sh")
PORT_ENV = os.path.join(ROOT, "tools", "lane", "port-env.sh")


def port_env(script, **env):
    """Run `script` in bash after sourcing tools/lane/port-env.sh."""
    full = dict(os.environ, **env)
    return subprocess.run(["bash", "-c", f'set -eu; . "{PORT_ENV}"; {script}'], env=full,
                          capture_output=True, text=True)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()


class RunTests(Base):
    def test_plain_run_keeps_exit_status(self):
        env = {k: v for k, v in os.environ.items() if k != "ARC_LANE_LOGS"}
        self.assertEqual(subprocess.run([RUN, "x", "sh", "-c", "exit 3"], env=env).returncode, 3)
        self.assertEqual(os.listdir(self.dir), [])

    @unittest.skipUnless(shutil.which("strace"), "strace not installed")
    def test_traced_run_writes_log_and_keeps_exit_status(self):
        logs = os.path.join(self.dir, "logs")
        env = dict(os.environ, ARC_LANE_LOGS=logs)
        r = subprocess.run([RUN, "prog", "sh", "-c", "exit 5"], env=env)
        if r.returncode != 5 and not os.path.exists(os.path.join(logs, "prog.strace")):
            self.skipTest("ptrace not permitted here")
        self.assertEqual(r.returncode, 5)
        with open(os.path.join(logs, "prog.strace")) as f:
            self.assertIn("execve(", f.read())


class PortEnvTests(Base):
    def test_compiler_follows_musl_prefix(self):
        r = port_env('echo "$CC $AR"', ARC_MUSL_PREFIX="/opt/x/bin/x86_64-linux-musl-")
        self.assertEqual(r.stdout.split(), ["/opt/x/bin/x86_64-linux-musl-gcc", "/opt/x/bin/x86_64-linux-musl-ar"])

    @unittest.skipUnless(shutil.which("file"), "file not installed")
    def test_check_static_rejects_dynamic_program(self):
        sh = os.path.realpath(shutil.which("sh"))
        if "dynamic" not in subprocess.run(["file", sh], capture_output=True, text=True).stdout:
            self.skipTest("host sh is not dynamically linked")
        r = port_env(f'port_check_static "{sh}"')
        self.assertEqual(r.returncode, 1)
        self.assertIn("dynamically linked", r.stderr)

    def test_begin_refuses_missing_toolchain(self):
        prefix = os.path.join(self.dir, "prefix")
        r = port_env(f'port_begin zstd "{prefix}"', ARC_MUSL_PREFIX=os.path.join(self.dir, "none-"))
        self.assertEqual(r.returncode, 1)
        self.assertIn("no musl toolchain", r.stderr)
        self.assertFalse(os.path.exists(prefix))


class BudgetLogTests(Base):
    def test_budget_and_serial_logs_both_read(self):
        with open(os.path.join(self.dir, "sizes.budget.log"), "w") as f:
            f.write("ARC_TEST budget cxx_basic_bytes 1234\nnoise\n")
        with open(os.path.join(self.dir, "smoke.serial.log"), "w") as f:
            f.write("ARC_TEST budget boot_free_bytes 99\n")
        vals = budget_check.log_values(self.dir)
        self.assertEqual(vals, {"sizes": {"cxx_basic_bytes": 1234}, "smoke": {"boot_free_bytes": 99}})

    def test_sizes_writes_alloc_bytes(self):
        out = os.path.join(self.dir, "sizes.budget.log")
        elf = os.path.realpath(sys.executable)
        with open(elf, "rb") as f:
            if f.read(5) != b"\x7fELF\x02":
                self.skipTest("python3 is not an ELF64 binary here")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "lane", "sizes.py"), out,
                            f"py_bytes={elf}"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out) as f:
            line = f.read().strip()
        self.assertEqual(line, f"ARC_TEST budget py_bytes {budget_check.elf_alloc_bytes(elf)}")


if __name__ == "__main__":
    unittest.main()
