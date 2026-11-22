#!/usr/bin/env python3
"""lane.zimcat (M3-03a): the zimcat command on ZIMs written by zim_fixture (lane tests never download a
ZIM). Every zimcat run goes through $ARC_LANE_RUN.

Checks: `info` prints the header, MIME list, metadata (text values, binary ones as their size) and the
counts per namespace and kind; `ls` lists every entry in path order with redirect targets and titles,
control characters escaped, and `ls NS` only that namespace; `cat` prints content byte for byte and
follows redirects, and a missing path exits 1; `check` passes on the good file and exits 1 naming the
cluster on one with an xz cluster; a file that isn't a ZIM exits 1; bad usage exits 2. Budgets:
zimcat_bytes comes from sizes.py; zimcat_check_rss_kib is the peak RSS of `zimcat check` on a generated
ZIM of 20 000 articles in 40 zstd clusters spread out of path order, through lane_rss.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

failures = 0
runs = 0


def check(ok, what):
    global failures
    print(f"{'ok' if ok else 'FAIL'}: {what}")
    failures += not ok
    return ok


def run(args, *argv, raw=False):
    """Run zimcat through the lane runner: (exit code, stdout, stderr)."""
    global runs
    runs += 1
    p = subprocess.run([os.environ["ARC_LANE_RUN"], f"zimcat_run{runs}", args.zimcat, *argv],
                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = p.stdout if raw else p.stdout.decode("utf-8", "replace")
    sys.stdout.write(p.stderr.decode("utf-8", "replace"))
    return p.returncode, out, p.stderr.decode("utf-8", "replace")


def fixture(args, tmp, name, spec):
    path = os.path.join(tmp, name + ".json")
    with open(path, "w") as f:
        json.dump(spec, f)
    out = os.path.join(tmp, name + ".zim")
    subprocess.run([args.fixture, path, out], check=True)
    return out


GOOD = {
    "major": 6, "minor": 3, "main_page": "W/mainPage",
    "clusters": [{"compression": "zstd"}, {"compression": "none"}],
    "entries": [
        {"ns": "M", "path": "Title", "mime": "text/plain", "text": "Lane ZIM", "cluster": 0},
        {"ns": "M", "path": "License", "mime": "text/plain", "text": "CC-BY-SA-4.0", "cluster": 0},
        {"ns": "M", "path": "Illustration_48x48@1", "mime": "image/png", "text": "PNGDATA", "cluster": 1},
        {"ns": "C", "path": "Hypothermia", "title": "Hypothermia (cold)", "mime": "text/html",
         "text": "<h1>Hypothermia</h1><p>Body temperature below 35 °C.</p>", "cluster": 0},
        {"ns": "C", "path": "Cold", "redirect": "C/Hypothermia"},
        {"ns": "C", "path": "Tab\there", "mime": "text/plain", "text": "tab path", "cluster": 1},
        {"ns": "C", "path": "img/pic.png", "mime": "image/png", "text": "\u0000\u0001binary", "cluster": 1},
        {"ns": "W", "path": "mainPage", "redirect": "C/Hypothermia"},
    ],
}


def test_good(args, tmp):
    zim = fixture(args, tmp, "good", GOOD)
    code, out, _ = run(args, "info", zim)
    lines = out.splitlines()
    check(code == 0 and "version\t6.3" in lines and "entries\t8" in lines and "clusters\t2" in lines,
          "info: version and counts")
    check("main_page\tW/mainPage" in lines and "title_list\tnone" in lines, "info: main page, no title list")
    check("meta Title\tLane ZIM" in lines and "meta License\tCC-BY-SA-4.0" in lines, "info: text metadata")
    check("meta Illustration_48x48@1\t<image/png, 7 bytes>" in lines, "info: binary metadata as its size")
    check("namespace C\t4" in lines and "namespace M\t3" in lines and "namespace W\t1" in lines and
          "content\t6" in lines and "redirects\t2" in lines, "info: entries per namespace and kind")

    code, out, _ = run(args, "ls", zim)
    rows = [line.split("\t") for line in out.splitlines()]
    check(code == 0 and [r[1] + "/" + r[2] for r in rows] ==
          ["C/Cold", "C/Hypothermia", "C/Tab\\x09here", "C/img/pic.png", "M/Illustration_48x48@1",
           "M/License", "M/Title", "W/mainPage"], "ls: every entry in path order, control characters escaped")
    check(rows[0][3] == "-> C/Hypothermia" and rows[1][3:] == ["text/html", "Hypothermia (cold)"],
          "ls: redirect target, MIME type and title")
    code, out, _ = run(args, "ls", zim, "M")
    check(code == 0 and len(out.splitlines()) == 3 and all(r.split("\t")[1] == "M" for r in out.splitlines()),
          "ls M: only the metadata namespace")
    code, out, _ = run(args, "ls", zim, "Q")
    check(code == 0 and out == "", "ls of an empty namespace prints nothing")

    code, out, _ = run(args, "cat", zim, "C/Hypothermia", raw=True)
    check(code == 0 and out == GOOD["entries"][3]["text"].encode(), "cat: content byte for byte (UTF-8)")
    code, out, _ = run(args, "cat", zim, "C/Cold", raw=True)
    check(code == 0 and out == GOOD["entries"][3]["text"].encode(), "cat: redirect followed")
    code, out, _ = run(args, "cat", zim, "C/img/pic.png", raw=True)
    check(code == 0 and out == b"\x00\x01binary", "cat: binary content from an uncompressed cluster")
    code, _, err = run(args, "cat", zim, "C/Nope")
    check(code == 1 and "not found" in err, "cat: missing path exits 1")

    code, out, _ = run(args, "check", zim)
    lines = out.splitlines()
    check(code == 0 and lines[-1] == "check\tok" and "clusters zstd\t1" in lines and "clusters none\t1" in lines,
          "check: passes, clusters by compression")
    check("html entries\t1" in lines and "blobs unreferenced\t0" in lines, "check: html entries and blobs")


def test_bad(args, tmp):
    spec = dict(GOOD, clusters=[{"compression": "zstd"}, {"compression": "xz"}])
    zim = fixture(args, tmp, "xz", spec)
    code, _, err = run(args, "check", zim)
    check(code == 1 and "cluster 1" in err and "xz" in err, "check: xz cluster exits 1 naming the cluster")
    code, _, err = run(args, "cat", zim, "C/img/pic.png")
    check(code == 1 and "xz" in err, "cat: content in an xz cluster exits 1")
    code, out, _ = run(args, "cat", zim, "C/Hypothermia")
    check(code == 0 and "Hypothermia" in out, "cat: zstd cluster of the same file still reads")

    junk = os.path.join(tmp, "junk.zim")
    with open(junk, "wb") as f:
        f.write(b"not a zim" * 20)
    code, _, err = run(args, "info", junk)
    check(code == 1 and "bad magic" in err, "a file that isn't a ZIM exits 1")
    code, _, _ = run(args, "info", os.path.join(tmp, "missing.zim"))
    check(code == 1, "a missing file exits 1")
    code, _, _ = run(args, "frobnicate", junk)
    check(code == 2, "unknown command exits 2")
    code, _, _ = run(args, "cat", os.path.join(tmp, "good.zim"), "Hypothermia")
    check(code == 2, "cat without NS/ exits 2")


def test_rss(args, tmp):
    zim = fixture(args, tmp, "big", {"major": 6, "minor": 3, "clusters": [{"compression": "zstd"}],
                                     "entries": [{"ns": "M", "path": "Title", "mime": "text/plain",
                                                  "text": "big", "cluster": 0}],
                                     "generate": {"count": 20000, "clusters": 40, "bytes": 2000}})
    p = subprocess.run([os.environ["ARC_LANE_RUN"], "zimcat_rss", args.lane_rss, args.zimcat, "check", zim],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    m = re.search(r"lane_rss: peak RSS (\d+) KiB", p.stderr)
    check(p.returncode == 0 and "check\tok" in p.stdout and "html entries\t20000" in p.stdout,
          "check: 20 000 generated articles in 40 clusters")
    check(m is not None, "lane_rss reported the peak RSS")
    if m:
        print(f"zimcat check peak RSS {m.group(1)} KiB")
        budget_dir = os.environ.get("ARC_LANE_BUDGET_DIR")
        if budget_dir:
            with open(os.path.join(budget_dir, "zimcat.budget.log"), "w") as f:
                f.write(f"ARC_TEST budget zimcat_check_rss_kib {m.group(1)}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zimcat", required=True)
    ap.add_argument("--fixture", required=True)
    ap.add_argument("--lane-rss", required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        test_good(args, tmp)
        test_bad(args, tmp)
        test_rss(args, tmp)
    print(f"zimcat: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
