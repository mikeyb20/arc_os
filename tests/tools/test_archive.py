#!/usr/bin/env python3
"""Host tests for tools/archive.py (M0-03). No network: every URL is file://."""

import hashlib
import io
import json
import os
import sys
import tarfile
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import archive  # noqa: E402


def rd(path):
    with open(path, "rb") as f:
        return f.read()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def comp(**kw):
    c = dict(name="x", version="1", kind="source", license="MIT", ships="build-host",
             purpose="", upstream_url="https://example.invalid/x-1.tar.gz",
             sha256="", archive_url="")
    c.update(kw)
    return c


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def tarball(self, name, files):
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode="w:gz") as t:
            for p, d in files.items():
                info = tarfile.TarInfo(p)
                info.size = len(d)
                t.addfile(info, io.BytesIO(d))
        return self.write(name, raw.getvalue())


class CheckTests(Base):
    def test_repo_lock_is_valid(self):
        self.assertEqual(archive.check(archive.load_lock(archive.DEFAULT_LOCK)), [])

    def test_missing_field_and_bad_values(self):
        bad = comp(kind="weird", ships="nowhere", sha256="ABC")
        del bad["license"]
        problems = archive.check({"schema_version": 1, "components": [bad, comp()]})
        text = "\n".join(problems)
        self.assertIn("missing field 'license'", text)
        self.assertIn("kind must be", text)
        self.assertIn("ships must be", text)
        self.assertIn("64 lowercase hex", text)
        self.assertIn("duplicate name", text)

    def test_schema_version(self):
        self.assertIn("schema_version must be 1", archive.check({"components": []}))


class AssetNameTests(unittest.TestCase):
    def test_names(self):
        self.assertEqual(archive.asset_name(comp(upstream_url="https://a/b/gcc-14.tar.xz")), "x-1.tar.xz")
        self.assertEqual(archive.asset_name(comp(upstream_url="https://a/m.gguf?download=1")), "x-1.gguf")
        self.assertEqual(archive.asset_name(comp(upstream_url="https://a/stb_image.h")), "x-1.h")
        self.assertEqual(archive.asset_name(comp(files=[{"path": "a", "url": "u"}])), "x-1.tar.gz")


class BundleTests(Base):
    def test_bundle_is_reproducible_and_checked(self):
        a = self.write("a.h", b"alpha\n")
        b = self.write("b.tbl", b"0 common read sys_read\n")
        files = [{"path": "include/a.h", "url": "file://" + a, "sha256": ""},
                 {"path": "arch/b.tbl", "url": "file://" + b, "sha256": sha(b"0 common read sys_read\n")}]
        c = comp(name="ref", files=files)
        w1, w2 = os.path.join(self.dir, "w1"), os.path.join(self.dir, "w2")
        os.makedirs(w1)
        os.makedirs(w2)
        p1, e1 = archive.build_bundle(c, w1)
        p2, e2 = archive.build_bundle(c, w2)
        self.assertEqual((e1, e2), ([], []))
        self.assertEqual(archive.sha256_file(p1), archive.sha256_file(p2))
        self.assertEqual(files[0]["sha256"], sha(b"alpha\n"))      # missing hash recorded
        with tarfile.open(p1) as t:
            self.assertEqual(sorted(t.getnames()), ["ref-1/arch/b.tbl", "ref-1/include/a.h"])

    def test_bundle_mismatch_fails(self):
        a = self.write("a.h", b"alpha\n")
        c = comp(files=[{"path": "a.h", "url": "file://" + a, "sha256": "0" * 64}])
        path, errs = archive.build_bundle(c, self.dir)
        self.assertIsNone(path)
        self.assertIn("sha256 mismatch", errs[0])


class FetchTests(Base):
    def run_fetch(self, comps, names, cache_only=False):
        lock = os.path.join(self.dir, "SOURCES.lock")
        archive.save_lock(lock, {"schema_version": 1, "components": comps})
        dest = os.path.join(self.dir, "src")
        args = type("A", (), {"lock": lock, "names": names, "dest": dest, "cache_only": cache_only})
        return archive.fetch(args), dest

    def test_cache_only_verifies_without_unpacking(self):
        t = self.tarball("x.tar.gz", {"x-1/README": b"hi"})
        rc, dest = self.run_fetch([comp(sha256=sha(rd(t)), archive_url="file://" + t)], ["x"], cache_only=True)
        self.assertEqual(rc, 0)
        self.assertFalse(os.path.exists(os.path.join(dest, "x")))
        rc, _ = self.run_fetch([comp(sha256="0" * 64, archive_url="file://" + t)], ["x"], cache_only=True)
        self.assertEqual(rc, 1)

    def test_tarball_unpacks_without_top_dir(self):
        t = self.tarball("x.tar.gz", {"x-1/README": b"hi", "x-1/src/a.c": b"int a;"})
        data = rd(t)
        rc, dest = self.run_fetch([comp(sha256=sha(data), archive_url="file://" + t)], ["x"])
        self.assertEqual(rc, 0)
        self.assertEqual(rd(os.path.join(dest, "x", "src", "a.c")), b"int a;")

    def test_single_file_and_split_parts(self):
        body = b"GGUF" + b"\0" * 1000
        p0 = self.write("m.gguf.part00", body[:600])
        p1 = self.write("m.gguf.part01", body[600:])
        c = comp(name="m", upstream_url="https://h/repo/resolve/main/model.gguf",
                 sha256=sha(body), archive_url="file:///unused/m-1.gguf",
                 archive_parts=["file://" + p0, "file://" + p1])
        rc, dest = self.run_fetch([c], ["m"])
        self.assertEqual(rc, 0)
        self.assertEqual(rd(os.path.join(dest, "m", "model.gguf")), body)

    def test_mismatch_unknown_and_unarchived_fail(self):
        t = self.tarball("x.tar.gz", {"x-1/a": b"a"})
        bad = comp(sha256="0" * 64, archive_url="file://" + t)
        rc, dest = self.run_fetch([bad], ["x"])
        self.assertEqual(rc, 1)
        self.assertFalse(os.path.exists(os.path.join(dest, "x")))
        rc, _ = self.run_fetch([comp()], ["x"])          # no archive_url yet
        self.assertEqual(rc, 1)
        rc, _ = self.run_fetch([comp()], ["nope"])
        self.assertEqual(rc, 1)

    def test_split_file_pieces(self):
        old = archive.SPLIT_BYTES
        archive.SPLIT_BYTES = 4
        try:
            path = self.write("big", b"0123456789")
            parts = archive.split_file(path)
            self.assertEqual([rd(p) for p in parts], [b"0123", b"4567", b"89"])
        finally:
            archive.SPLIT_BYTES = old


if __name__ == "__main__":
    unittest.main()
