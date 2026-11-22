#!/usr/bin/env python3
"""lane.packbuild_cli (M3-03): the packbuild program, every run through $ARC_LANE_RUN.

Checks: a good stream builds and the summary names documents, chunks and words per chunk; a bad stream
exits 1 naming the line and leaves no output directory; bad usage and bad tags exit 2. Budget:
packbuild_rss_kib is the peak RSS of building a generated 10 000-document stream (about 400 words
each in two sections, with links), through lane_rss so the driver's own RSS isn't counted.
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


def run(args, *argv, rss=False):
    global runs
    runs += 1
    cmd = [os.environ["ARC_LANE_RUN"], f"packbuild_run{runs}"] + ([args.lane_rss] if rss else []) + \
          [args.packbuild, *argv]
    p = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    sys.stdout.write(p.stderr)
    return p.returncode, p.stdout, p.stderr


def write_stream(path, docs):
    with open(path, "w", encoding="utf-8") as f:
        f.write(json.dumps({"stream": 1, "source": "lane", "zim": "none", "date": "2026-11-22"}) + "\n")
        for key, title, text in docs:
            f.write(json.dumps({"key": key, "title": title, "text": text}, ensure_ascii=False) + "\n")


def build(args, stream, out, *extra, rss=False):
    return run(args, "build", "--name", "lane-pack", "--version", "1.0.0", *extra, stream, out, rss=rss)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--packbuild", required=True)
    ap.add_argument("--lane-rss", required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        good = os.path.join(tmp, "good.jsonl")
        write_stream(good, [("a", "A", "Alpha links to \u0002b\u001fbeta\u0003."), ("b", "B", "Beta text.")])
        code, out, _ = build(args, good, os.path.join(tmp, "p1"), "--tags", "medical,first-aid")
        check(code == 0 and "documents\t2\nchunks\t2\n" in out and "words per chunk\tmin 2\tmedian 2\tmax 4" in out
              and "links\t1\n" in out, "a good stream builds; the summary counts documents, chunks, words, links")
        check(sorted(os.listdir(os.path.join(tmp, "p1"))) == ["chunks", "content"], "content/ and chunks/ written")

        bad = os.path.join(tmp, "bad.jsonl")
        write_stream(bad, [("b", "B", "x"), ("a", "A", "y")])
        code, _, err = build(args, bad, os.path.join(tmp, "p2"))
        check(code == 1 and "line 3" in err and not os.path.exists(os.path.join(tmp, "p2")),
              "a stream out of order exits 1, names the line, leaves no output directory")
        code, _, _ = build(args, good, os.path.join(tmp, "p3"), "--tags", "medical,,x")
        check(code == 2, "an empty tag exits 2")
        code, _, _ = run(args, "build", "--name", "x", good, os.path.join(tmp, "p4"))
        check(code == 2, "a missing --version exits 2")
        code, _, _ = run(args, "frob")
        check(code == 2, "an unknown command exits 2")

        big = os.path.join(tmp, "big.jsonl")
        words = " ".join(f"word{i % 97}" + ("." if i % 23 == 22 else "") for i in range(200))
        with open(big, "w", encoding="utf-8") as f:
            f.write(json.dumps({"stream": 1}) + "\n")
            for i in range(10000):
                text = f"\u00012Part {i}\n{words}\nSee \u0002doc{(i * 7) % 10000:05d}\u001fanother\u0003.\n{words}"
                f.write(json.dumps({"key": f"doc{i:05d}", "title": f"Document {i}", "text": text}) + "\n")
        code, out, err = build(args, big, os.path.join(tmp, "big"), "--tags", "medical", rss=True)
        m = re.search(r"lane_rss: peak RSS (\d+) KiB", err)
        check(code == 0 and "documents\t10000\n" in out, "a 10 000-document stream builds")
        check(m is not None, "lane_rss reported the peak RSS")
        if m:
            print(f"packbuild peak RSS {m.group(1)} KiB")
            budget_dir = os.environ.get("ARC_LANE_BUDGET_DIR")
            if budget_dir:
                with open(os.path.join(budget_dir, "packbuild_cli.budget.log"), "w") as f:
                    f.write(f"ARC_TEST budget packbuild_rss_kib {m.group(1)}\n")
    print(f"packbuild_cli: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
