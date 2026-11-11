#!/usr/bin/env python3
"""lane.search (M3-06a): keyword search over two small packs, written with libarcpack and indexed
with arcsearch_build_fts (search_test), queried with the `search` command. Every program runs
through $ARC_LANE_RUN.

Root A holds medical 0.9.0 and 1.0.0 and outdoors 2.0.0, all indexed. Root B holds medical 1.0.0,
outdoors rebuilt with one more document but its old index (stale), and a pack with no index.
Checks: ranking ("what is hypothermia", title over body), two packs merged in score order, the
highest version answers, stopword-only and empty queries, FTS5 syntax as words, Unicode words,
--pack, stale and missing indexes skipped with a warning, no socket or connect in any `search` run
(no arc-llm), and search_rss_kib (one query over both packs, through lane_rss).
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lane_pack import write_manifest  # noqa: E402  (stack/tests/lane_pack.py)

failures = 0
runs = 0


def check(ok, what):
    global failures
    print(f"{'ok' if ok else 'FAIL'}: {what}")
    failures += not ok
    return ok


def run(log, *argv):
    """Run a program through the lane runner: (exit code, stdout, stderr)."""
    p = subprocess.run([os.environ["ARC_LANE_RUN"], log, *argv], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True, errors="replace")
    sys.stdout.write(p.stdout)
    sys.stdout.write(p.stderr)
    return p.returncode, p.stdout, p.stderr


def search(args, root, *argv):
    """Run `search --root root argv...`: (exit code, [(score, id, title, section)], stderr)."""
    global runs
    runs += 1
    code, out, err = run(f"search_query{runs}", args.search, "--root", root, *argv)
    rows = [line.split("\t") for line in out.splitlines()]
    return code, [(float(r[0]), r[1], r[2], r[3]) for r in rows if len(r) == 4], err


def install(args, root, name, version, content, index=True):
    pack = os.path.join(root, "packs", name, version)
    os.makedirs(os.path.dirname(pack), exist_ok=True)
    ok = run(f"search_write_{name}_{version}", args.program, "write", pack, name, version, content)[0] == 0
    write_manifest(pack, name, version, ["general"])
    if ok and index:
        ok = run(f"search_index_{name}_{version}", args.program, "index", pack)[0] == 0
        write_manifest(pack, name, version, ["general"])  # checksums now include fts/index.sqlite
    check(ok, f"install {name} {version} ({content}{', indexed' if index else ''})")
    return pack


def ids(rows):
    return [r[1] for r in rows]


def test_root_a(args, root):
    install(args, root, "medical", "0.9.0", "old")
    install(args, root, "medical", "1.0.0", "medical")
    install(args, root, "outdoors", "2.0.0", "outdoors")
    check(run("search_api", args.program, "api", root)[0] == 0, "library checks (search_test api)")

    code, rows, err = search(args, root, "what is hypothermia")
    # The title weighs 5 and FTS5's bm25 saturates the summed term count, so the Hypothermia document's
    # three chunks score about the same; the body-only mention (Heat stroke) must come after all of them.
    check(code == 0 and ids(rows)[:3] and all(r[2] == "Hypothermia" for r in rows[:3]) and
          ids(rows)[3:] == ["medical:1:0"], "'what is hypothermia': the Hypothermia chunks first, a body mention after")
    code, rows, _ = search(args, root, "burns")
    check(code == 0 and ids(rows)[:1] == ["medical:2:0"] and "medical:1:0" in ids(rows),
          "a title match ('Burns') beats a body match in the same pack")
    code, rows, _ = search(args, root, "water")
    packs = {r[1].split(":")[0] for r in rows}
    check(code == 0 and packs == {"medical", "outdoors"}, "both packs answer 'water'")
    check(all(a[0] >= b[0] for a, b in zip(rows, rows[1:])), "merged results are in score order")
    code, rows, _ = search(args, root, "-k", "1", "water")
    check(code == 0 and len(rows) == 1, "-k 1 prints one result")
    code, rows, _ = search(args, root, "--pack", "outdoors", "water")
    check(code == 0 and rows and all(r[1].startswith("outdoors:") for r in rows), "--pack outdoors: only outdoors")
    code, rows, _ = search(args, root, "frostbite")
    check(code == 0 and rows == [], "the older medical version is not searched")
    for q in ("zurich", "ZÜRICH"):
        code, rows, _ = search(args, root, q)
        check(code == 0 and ids(rows) == ["outdoors:1:0"] and rows[0][3] == "Glacier > Ropes",
              f"'{q}' finds Zürich (diacritics and case folded); section path printed")
    for q in ("what is the", "", "   ", "?!"):
        code, rows, err = search(args, root, q)
        check(code == 0 and rows == [] and "search:" not in err, f"query {q!r}: no results, no error")
    for q in ('"', "water*", "-water", "NEAR(water fire)", "water AND fire", "title:water", "(water", "^water",
              '"water', "{title}: water", "water OR", "NOT water"):
        code, rows, err = search(args, root, "--", q)
        check(code == 0 and "search:" not in err and ("water" not in q.lower() or rows),
              f"FTS5 syntax {q!r} is only words")

    code, out, err = run("search_rss", args.lane_rss, args.search, "--root", root, "what is hypothermia")
    m = re.search(r"lane_rss: peak RSS (\d+) KiB", err)
    if check(code == 0 and m is not None, "peak RSS of one query over both packs"):
        budget_dir = os.environ.get("ARC_LANE_BUDGET_DIR")
        if budget_dir:
            with open(os.path.join(budget_dir, "search.budget.log"), "w") as f:
                f.write(f"ARC_TEST budget search_rss_kib {m.group(1)}\n")


def test_root_b(args, root, root_a):
    os.makedirs(os.path.join(root, "packs", "medical"))
    shutil.copytree(os.path.join(root_a, "packs", "medical", "1.0.0"), os.path.join(root, "packs", "medical", "1.0.0"))
    stale = install(args, root, "outdoors", "2.0.0", "outdoors-more", index=False)
    shutil.copytree(os.path.join(root_a, "packs", "outdoors", "2.0.0", "fts"), os.path.join(stale, "fts"))
    install(args, root, "bare", "1.0.0", "outdoors", index=False)
    code, rows, err = search(args, root, "water")
    check(code == 0 and rows and all(r[1].startswith("medical:") for r in rows), "only the medical pack answers")
    check("pack outdoors 2.0.0 skipped: stale index (chunk_count is 8, the pack has 9)" in err,
          "the stale outdoors index is skipped with a warning")
    check("pack bare 1.0.0 skipped: fts/index.sqlite" in err, "a pack with no index is skipped with a warning")


def check_no_socket(logs):
    """No `search` run opened a socket: search never reaches arc-llm (ADR-0015)."""
    if not logs:
        print("(ARC_LANE_LOGS not set: socket check skipped)")
        return
    paths = glob.glob(os.path.join(logs, "search_query*.strace")) + [os.path.join(logs, "search_rss.strace")]
    found = [p for p in paths if re.search(r"\b(socket|connect)\(", open(p).read())]
    check(len(paths) == runs + 1 and not found, f"no socket or connect in {len(paths)} search traces")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--program", required=True)
    ap.add_argument("--search", required=True)
    ap.add_argument("--lane-rss", required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="search") as tmp:
        root_a, root_b = os.path.join(tmp, "a"), os.path.join(tmp, "b")
        test_root_a(args, root_a)
        test_root_b(args, root_b, root_a)
    check_no_socket(os.environ.get("ARC_LANE_LOGS"))
    print(f"search: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
