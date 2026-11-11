#!/usr/bin/env python3
"""Lane tests for libarcpack (M3-02), driving arcpack_test (every run goes through $ARC_LANE_RUN).

    three    write the three-document pack, write its manifest.json (with SHA-256 checksums, as the
             pipeline will, M3-04), check it against schemas/pack.schema.json, read everything back
             (arcpack_test read3: round trip, chunk IDs both ways, plain text, writer refusals)
    damaged  copies of that pack, each damaged one way (bad magic, wrong schema_version, truncated
             index, frame or title or chunk range out of bounds, corrupt frame, frame
             without its checksum or with a dictionary, manifest that
             disagrees, missing file): reading each gives its own error, never a crash
    long     a one-document pack with 9 MiB of text, past the 8 MiB window: it reads back, its frame
             states a window of at most 8 MiB, and with the window raised to 16 MiB it is refused
    big      a synthetic 200 000-chunk pack; 1000 random chunk lookups in a process forked by
             arcpack_test, which reports that process's peak RSS. Writes it as arcpack_lookup_rss_kib to
             arcpack_big.budget.log. (ru_maxrss of a process this driver starts includes Python's own
             RSS from before exec, about 13 MB, so the program measures its forked child instead.)
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import lane_pack  # noqa: E402  (stack/tests/lane_pack.py)
from lane_pack import HEADER, header_count  # noqa: E402

NAME = "test-pack"
VERSION = "1.0.0"
TAGS = ["medicine", "first-aid", "outdoors"]
BIG_CHUNKS = 200000
LONG_MIB = 9

failures = 0


def check(ok, what):
    global failures
    print(f"{'ok' if ok else 'FAIL'}: {what}")
    failures += not ok
    return ok


def write_manifest(pack, **override):
    """manifest.json for the test pack in `pack` (lane_pack.py)."""
    return lane_pack.write_manifest(pack, NAME, VERSION, TAGS, **override)


def run(args, log, *argv):
    """Run arcpack_test through the lane runner; returns (exit code, stdout)."""
    p = subprocess.run([os.environ["ARC_LANE_RUN"], log, args.program, *argv], stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    sys.stdout.write(p.stdout)
    return p.returncode, p.stdout


def make_three(args, tmp):
    pack = os.path.join(tmp, "three")
    code, _ = run(args, "arcpack_write3", "write3", pack)
    if not check(code == 0, "write the three-document pack"):
        raise SystemExit(1)
    write_manifest(pack)
    return pack


def test_three(args, tmp):
    pack = make_three(args, tmp)
    copy = os.path.join(tmp, f"pack.{NAME}.json")  # validate.py picks the schema by file name
    shutil.copy(os.path.join(pack, "manifest.json"), copy)
    v = subprocess.run([sys.executable, args.validate, copy], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)
    sys.stdout.write(v.stdout)
    check(v.returncode == 0, "manifest.json passes schemas/pack.schema.json")
    with open(os.path.join(pack, "content/docs.zst"), "rb") as f:
        head = f.read(8)
    check(head == bytes.fromhex("5a2a4d18f80f0000"), "content/docs.zst opens with a 4096-byte skippable frame")
    sizes_ok = all(os.path.getsize(os.path.join(pack, rel)) == HEADER + header_count(os.path.join(pack, rel)) * rs
                   for rel, rs in (("content/docs.idx", 32), ("chunks/chunks.idx", 32), ("content/strings", 1)))
    check(sizes_ok, "every file is its header plus count x record size")
    code, _ = run(args, "arcpack_read3", "read3", pack, os.path.join(tmp, "scratch"))
    check(code == 0, "read3: everything reads back")


def patch(path, offset, data):
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(data)


def record(pack, rel, index):
    """(path, file offset) of record `index` in a 32-byte-record file."""
    return os.path.join(pack, rel), HEADER + 32 * index


def damage_cases(pack):
    """name -> (function that damages a copy of the pack, text the error must contain)."""
    docs, chunks = "content/docs.idx", "chunks/chunks.idx"
    zst_size = os.path.getsize(os.path.join(pack, "content/docs.zst"))

    def frame0_byte(p, where, mask):
        """XOR one byte of document 0's frame with `mask`; `where` maps the frame size to the byte's index."""
        path, off = record(p, docs, 0)
        with open(path, "rb") as f:
            f.seek(off)
            frame_off, frame_size = struct.unpack("<QI", f.read(12))
        at = frame_off + where(frame_size)
        with open(os.path.join(p, "content/docs.zst"), "r+b") as f:
            f.seek(at)
            b = f.read(1)
            f.seek(at)
            f.write(bytes([b[0] ^ mask]))

    return {
        "bad magic": (lambda p: patch(os.path.join(p, chunks), 0, b"X"), "chunks/chunks.idx: not a pack file"),
        "schema_version": (lambda p: patch(os.path.join(p, docs), 8, struct.pack("<I", 2)), "schema_version 2"),
        "truncated index": (lambda p: os.truncate(os.path.join(p, chunks), os.path.getsize(os.path.join(p, chunks))
                                                  - 16), "but the header counts"),
        "short file": (lambda p: os.truncate(os.path.join(p, "content/docs.zst"), 100), "shorter than its 4096-byte"),
        "missing file": (lambda p: os.unlink(os.path.join(p, "content/strings")), "content/strings: No such file"),
        "frame past the end": (lambda p: patch(*record(p, docs, 2), struct.pack("<Q", zst_size)),
                               "document 2: frame outside content/docs.zst"),
        "frame offset huge": (lambda p: patch(*record(p, docs, 1), struct.pack("<Q", 2**64 - 8)),
                              "document 1: frame outside content/docs.zst"),
        "corrupt frame": (lambda p: frame0_byte(p, lambda n: n // 2, 0x5A), "document 0: damaged frame"),
        # Frame header descriptor (RFC 8878): bit 2 content checksum, bits 0-1 dictionary ID size.
        "frame without checksum": (lambda p: frame0_byte(p, lambda n: 4, 0x04), "document 0: not a plain pack-v1 frame"),
        "frame with a dictionary": (lambda p: frame0_byte(p, lambda n: 4, 0x01), "document 0: not a plain pack-v1 frame"),
        "title past the end": (lambda p: patch(record(p, docs, 0)[0], record(p, docs, 0)[1] + 16,
                                               struct.pack("<I", 2**32 - 4)), "document 0: title outside"),
        "chunks past the end": (lambda p: patch(record(p, docs, 2)[0], record(p, docs, 2)[1] + 28,
                                                struct.pack("<I", 3)), "document 2: chunks outside"),
        "chunk range outside its document": (lambda p: patch(record(p, chunks, 1)[0], record(p, chunks, 1)[1] + 8,
                                                             struct.pack("<I", 100000)),
                                             "chunk 1: range outside document 0's text"),
        "chunk names another document": (lambda p: patch(*record(p, chunks, 0), struct.pack("<I", 2)),
                                         "chunk 0: not among document 2's chunks"),
        "section past the end": (lambda p: patch(record(p, chunks, 3)[0], record(p, chunks, 3)[1] + 12,
                                                 struct.pack("<II", 1000, 10)), "chunk 3: section path outside"),
        "manifest chunk_count": (lambda p: write_manifest(p, chunk_count=5), "`chunk_count`"),
        "manifest document_count": (lambda p: write_manifest(p, document_count=2), "`document_count`"),
        "manifest of another version": (lambda p: write_manifest(p, version="2.0.0"),
                                        "belongs to another pack or version"),
        "manifest not JSON": (lambda p: open(os.path.join(p, "manifest.json"), "w").write("{\"name\": "),
                              "manifest.json:"),
        "too many domain tags": (lambda p: write_manifest(p, domain_tags=[f"t{i}" for i in range(65)]),
                                 "`domain_tags`"),
    }


def test_damaged(args, tmp):
    pack = make_three(args, tmp)
    code, out = run(args, "arcpack_scan", "scan", pack)
    check(code == 0 and "all readable" in out, "the undamaged pack scans clean")
    for i, (name, (damage, want)) in enumerate(damage_cases(pack).items()):
        copy = os.path.join(tmp, f"damaged{i}")
        shutil.copytree(pack, copy)
        damage(copy)
        code, out = run(args, f"arcpack_damaged{i}", "scan", copy)
        check(code == 1 and want in out, f"{name}: refused with '{want}'")


def test_big(args, tmp):
    pack = os.path.join(tmp, "big")
    code, _ = run(args, "arcpack_big_write", "big-write", pack, str(BIG_CHUNKS))
    if not check(code == 0, f"write a {BIG_CHUNKS}-chunk pack"):
        return
    write_manifest(pack)
    check(header_count(os.path.join(pack, "chunks/chunks.idx")) == BIG_CHUNKS, "chunk count in the header")
    code, out = run(args, "arcpack_big_lookup", "big-lookup", pack)
    check(code == 0 and "1000 of 1000" in out, "1000 random lookups right")
    m = re.search(r"lookup peak RSS: (\d+) KiB", out)
    if not check(m is not None and int(m.group(1)) > 0, "peak RSS of the lookups reported"):
        return
    rss = int(m.group(1))
    budget_dir = os.environ.get("ARC_LANE_BUDGET_DIR")
    if budget_dir:
        with open(os.path.join(budget_dir, "arcpack_big.budget.log"), "w") as f:
            f.write(f"ARC_TEST budget arcpack_lookup_rss_kib {rss}\n")


def first_frame_header(pack):
    """(offset, first 6 bytes) of document 0's zstd frame."""
    path, off = record(pack, "content/docs.idx", 0)
    with open(path, "rb") as f:
        f.seek(off)
        frame_off = struct.unpack("<Q", f.read(8))[0]
    with open(os.path.join(pack, "content/docs.zst"), "rb") as f:
        f.seek(frame_off)
        return frame_off, f.read(6)


def test_long(args, tmp):
    pack = os.path.join(tmp, "long")
    code, _ = run(args, "arcpack_long_write", "long-write", pack, str(LONG_MIB))
    if not check(code == 0, f"write a {LONG_MIB} MiB document"):
        return
    write_manifest(pack)
    code, out = run(args, "arcpack_long_scan", "scan", pack)
    check(code == 0 and "all readable" in out, "it reads back")
    # Frame header descriptor (RFC 8878 3.1.1.1): bit 5 single segment; else byte 5 is the window descriptor.
    frame_off, head = first_frame_header(pack)
    window_log = 10 + (head[5] >> 3)
    window = (1 << window_log) + ((1 << window_log) >> 3) * (head[5] & 7)
    check(not head[4] & 0x20 and window <= 8 << 20, f"the frame has a window descriptor, window {window} <= 8 MiB")
    copy = os.path.join(tmp, "long-window")
    shutil.copytree(pack, copy)
    patch(os.path.join(copy, "content/docs.zst"), frame_off + 5, bytes([(24 - 10) << 3]))
    code, out = run(args, "arcpack_long_window", "scan", copy)
    want = "document 0: not a plain pack-v1 frame"
    check(code == 1 and want in out, f"a 16 MiB window refused with '{want}'")


TESTS = {"three": test_three, "damaged": test_damaged, "long": test_long, "big": test_big}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("test", choices=sorted(TESTS))
    ap.add_argument("--program", required=True)
    ap.add_argument("--validate", required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="arcpack") as tmp:
        TESTS[args.test](args, tmp)
    print(f"arcpack_{args.test}: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
