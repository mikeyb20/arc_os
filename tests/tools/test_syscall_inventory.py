#!/usr/bin/env python3
"""Host tests for tools/syscall-inventory.py (M0-04), on hand-written strace lines."""

import collections
import importlib.util
import os
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location("inv", os.path.join(ROOT, "tools", "syscall-inventory.py"))
inv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inv)

LOG = """\
100   execve("/bin/prog", ["prog"], 0x7ffd /* 3 vars */) = 0
100   arch_prctl(ARCH_SET_FS, 0x7f00) = 0
100   set_tid_address(0x7f10) = 100
100   openat(AT_FDCWD, "/etc/passwd", O_RDONLY|O_CLOEXEC) = 3
100   open("/proc/101/stat", O_RDONLY|O_LARGEFILE) = 4
100   ioctl(1, TIOCGWINSZ, 0x7ffc) = -1 ENOTTY (Inappropriate ioctl for device)
100   mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7f20
100   clone(child_stack=0x7f30, flags=CLONE_VM|CLONE_FS|CLONE_THREAD|CLONE_SETTLS, parent_tid=0x7f40) = 101
101   futex(0x7f50, FUTEX_WAIT_PRIVATE, 2, NULL <unfinished ...>
100   futex(0x7f50, FUTEX_WAKE_PRIVATE, 1) = 1
101   <... futex resumed>) = 0
100   newfstatat(AT_FDCWD, "/sys/devices/system/cpu/cpu3/online", {st_mode=S_IFREG|0444, st_size=4096, ...}, 0) = 0
100   write(1, "a, b (c)", 8) = 8
101   +++ exited with 0 +++
100   exit_group(0)                     = ?
"""


class SplitArgsTests(unittest.TestCase):
    def test_nested_and_quoted(self):
        self.assertEqual(inv.split_args('1, "a, b (c)", 8) = 8'), ["1", '"a, b (c)"', "8"])
        self.assertEqual(inv.split_args('AT_FDCWD, "/x", {st_mode=S_IFREG, st_size=1}, 0) = 0'),
                         ["AT_FDCWD", '"/x"', "{st_mode=S_IFREG, st_size=1}", "0"])
        self.assertEqual(inv.split_args("0x7f50, FUTEX_WAIT_PRIVATE, 2, NULL <unfinished ...>"),
                         ["0x7f50", "FUTEX_WAIT_PRIVATE", "2", "NULL"])

    def test_symbols(self):
        self.assertEqual(inv.symbols("O_RDONLY|O_CLOEXEC"), ["O_RDONLY", "O_CLOEXEC"])
        self.assertEqual(inv.symbols("0x7f00"), [])
        self.assertEqual(inv.symbols("flags=CLONE_VM|CLONE_FS"), ["CLONE_VM", "CLONE_FS"])


class ParseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.logs = os.path.join(self.tmp.name, "logs")
        os.makedirs(self.logs)
        with open(os.path.join(self.logs, "prog.strace"), "w") as f:
            f.write(LOG)
        self.tbl = os.path.join(self.tmp.name, "syscall_64.tbl")
        with open(self.tbl, "w") as f:
            f.write("# comment\n1\tcommon\twrite\tsys_write\n2\tcommon\topen\tsys_open\n"
                    "9\tcommon\tmmap\tsys_mmap\n16\tcommon\tioctl\tsys_ioctl\n"
                    "56\tcommon\tclone\tsys_clone\n59\t64\texecve\tsys_execve\n"
                    "158\t64\tarch_prctl\tsys_arch_prctl\n202\tcommon\tfutex\tsys_futex\n"
                    "218\tcommon\tset_tid_address\tsys_set_tid_address\n"
                    "231\tcommon\texit_group\tsys_exit_group\n257\tcommon\topenat\tsys_openat\n"
                    "262\tcommon\tnewfstatat\tsys_newfstatat\n512\tx32\trt_sigaction\tcompat\n")

    def tearDown(self):
        self.tmp.cleanup()

    def parse(self):
        calls = collections.defaultdict(collections.Counter)
        details = collections.defaultdict(lambda: collections.defaultdict(set))
        files = collections.defaultdict(set)
        inv.parse_log(os.path.join(self.logs, "prog.strace"), "prog", calls, details, files)
        return calls, details, files

    def test_counts_call_starts_only(self):
        calls, _, _ = self.parse()
        self.assertEqual(calls["futex"]["prog"], 2)       # resumed line not counted again
        self.assertEqual(calls["exit_group"]["prog"], 1)
        self.assertNotIn("exited", calls)

    def test_details_and_files(self):
        _, details, files = self.parse()
        self.assertIn("flags: O_CLOEXEC", details["openat"])
        self.assertIn("request: TIOCGWINSZ", details["ioctl"])
        self.assertIn("prot: PROT_WRITE", details["mmap"])
        self.assertIn("flags: MAP_ANONYMOUS", details["mmap"])
        self.assertIn("flags: CLONE_THREAD", details["clone"])
        self.assertIn("op: FUTEX_WAIT_PRIVATE", details["futex"])
        self.assertIn("code: ARCH_SET_FS", details["arch_prctl"])
        self.assertEqual(set(files), {"/etc/passwd", "/proc/<pid>/stat",
                                      "/sys/devices/system/cpu/cpu<n>/online"})

    def test_tbl_skips_x32(self):
        tbl = inv.load_tbl(self.tbl)
        self.assertEqual(tbl["openat"], 257)
        self.assertEqual(tbl["execve"], 59)
        self.assertNotIn("rt_sigaction", tbl)

    def test_doc_keeps_hand_written_text(self):
        out = os.path.join(self.tmp.name, "inv.md")
        with open(out, "w") as f:
            f.write("# Title\n\nSummary by hand.\n\n" + inv.BEGIN + "\nold\n" + inv.END + "\n\nFooter.\n")
        calls, details, files = self.parse()
        doc = inv.render(calls, details, files, inv.load_tbl(self.tbl), {"write": "legacy"}, ["prog"], ["run-prog ok"])
        inv.write_doc(out, doc)
        with open(out) as f:
            text = f.read()
        self.assertIn("Summary by hand.", text)
        self.assertIn("Footer.", text)
        self.assertNotIn("\nold\n", text)
        self.assertIn("| 1 | write | legacy | 1 | prog |", text)
        self.assertIn("| 257 | openat | missing |", text)
        # Rows sorted by syscall number
        self.assertLess(text.index("| 1 | write"), text.index("| 257 | openat"))


if __name__ == "__main__":
    unittest.main()
