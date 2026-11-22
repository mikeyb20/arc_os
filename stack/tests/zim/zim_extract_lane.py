#!/usr/bin/env python3
"""lane.zim_extract (M3-03): a fixture ZIM through our zimcat and tools/content/zim_extract.py into an article
stream, checked line by line, then through packbuild into a pack. zimcat and packbuild run through
$ARC_LANE_RUN; the extractor is a host-only driver (python3 -I).

The fixture holds headings, links (one through a redirect, one to a page not in the ZIM), a table, a figure
whose image is in the ZIM, an inline script, a redirect and a stylesheet, in three clusters out of path order.
"""

import argparse
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile

failures = 0


def check(ok, what):
    global failures
    print(f"{'ok' if ok else 'FAIL'}: {what}")
    failures += not ok
    return ok


ZIM = {
    "major": 6, "minor": 3,
    "clusters": [{"compression": "zstd"}, {"compression": "zstd"}, {"compression": "none"}],
    "entries": [
        {"ns": "M", "path": "Source", "mime": "text/plain", "text": "lane.example", "cluster": 2},
        {"ns": "M", "path": "Date", "mime": "text/plain", "text": "2026-11-22", "cluster": 2},
        {"ns": "M", "path": "Name", "mime": "text/plain", "text": "lane_medical", "cluster": 2},
        {"ns": "C", "path": "Hypothermia", "mime": "text/html", "cluster": 1, "text":
            "<html><head><title>Hypothermia</title><script>var x = 1;</script></head><body><main>"
            "<h1>Hypothermia</h1><p>Body temperature below 35 °C. See <a href='Frostbite'>frostbite</a> and "
            "<a href='Cold'>cold</a><sup class='reference'><a href='#n1'>[1]</a></sup>.</p>"
            "<h2>Signs</h2><table><tr><th>Mild</th><td>Shivering</td></tr><tr><th>Severe</th><td>No shivering</td>"
            "</tr></table><figure><img src='_assets_/stages.webp' alt='x'><figcaption>The stages</figcaption>"
            "</figure><h2>Treatment</h2><ul><li>Warm drinks</li><li>Dry clothes, <a href='Nowhere'>blankets</a>"
            "</li></ul><script>track()</script></main></body></html>"},
        {"ns": "C", "path": "Frostbite", "mime": "text/html", "cluster": 0, "text":
            "<p>Frozen skin. Compare <a href='Hypothermia'>hypothermia</a>.</p>"},
        {"ns": "C", "path": "Burns", "mime": "text/html", "cluster": 1, "text": "<p>Cool a burn.</p>"},
        {"ns": "C", "path": "Cold", "redirect": "C/Hypothermia"},
        {"ns": "C", "path": "site.css", "mime": "text/css", "text": "p{}", "cluster": 2},
        {"ns": "C", "path": "_assets_/stages.webp", "mime": "image/webp", "text": "WEBP", "cluster": 2},
    ],
}

WANT = [
    {"key": "Burns", "title": "Burns", "text": "Cool a burn."},
    {"key": "Frostbite", "title": "Frostbite", "text": "Frozen skin. Compare \u0002Hypothermia\u001fhypothermia\u0003."},
    {"key": "Hypothermia", "title": "Hypothermia", "text":
        "Body temperature below 35 °C. See \u0002Frostbite\u001ffrostbite\u0003 and "
        "\u0002Hypothermia\u001fcold\u0003.\n\u00012Signs\nMild\tShivering\nSevere\tNo shivering\n"
        "\u0004_assets_/stages.webp\u001fThe stages\n\u00012Treatment\n• Warm drinks\n"
        "• Dry clothes, \u0002Nowhere\u001fblankets\u0003"},
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zimcat", required=True)
    ap.add_argument("--fixture", required=True)
    ap.add_argument("--packbuild", required=True)
    ap.add_argument("--extract", required=True)
    args = ap.parse_args()
    run = os.environ["ARC_LANE_RUN"]
    with tempfile.TemporaryDirectory() as tmp:
        spec = os.path.join(tmp, "spec.json")
        with open(spec, "w") as f:
            json.dump(ZIM, f)
        zim = os.path.join(tmp, "lane.zim")
        subprocess.run([args.fixture, spec, zim], check=True)
        with open(zim, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        out = os.path.join(tmp, "lane.jsonl")
        zimcat = " ".join(shlex.quote(x) for x in (run, "zim_extract_zimcat", args.zimcat))
        p = subprocess.run([sys.executable, "-I", args.extract, "--sha256", sha, "--zimcat", zimcat, zim, out],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        sys.stdout.write(p.stdout)
        check(p.returncode == 0, "zim_extract.py exits 0")
        check("articles\t3" in p.stdout and "redirects\t1 (1 to articles" in p.stdout and
              "image image/webp\t1 entries\t4 bytes" in p.stdout and "skipped text/css\t1 entries\t3 bytes" in p.stdout,
              "report: articles, redirects, skipped entries, image types")
        with open(out, encoding="utf-8") as f:
            raw = f.read().split("\n")
        check(raw[-1] == "" and len(raw) == 5, "stream: header, three documents, newline at the end")
        header = json.loads(raw[0])
        check(header == {"stream": 1, "source": "lane.example", "zim": "lane.zim", "date": "2026-11-22",
                         "name": "lane_medical", "title": "", "license": "", "sha256": sha}, "stream: header line")
        for want, line in zip(WANT, raw[1:4]):
            got = json.loads(line)
            if not check(got == want, f"stream: document {want['key']}"):
                print(f"  got  {got!r}\n  want {want!r}")

        pack = os.path.join(tmp, "pack")
        p = subprocess.run([run, "zim_extract_packbuild", args.packbuild, "build", "--name", "lane-medical",
                            "--version", "1.0.0", "--tags", "medical", out, pack],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        sys.stdout.write(p.stdout)
        check(p.returncode == 0 and "documents\t3" in p.stdout and "links\t3" in p.stdout and
              "links to keys not in the stream (kept as text)\t1" in p.stdout,
              "packbuild builds the pack: 3 documents, 3 links resolved, 1 kept as text")
    print(f"zim_extract: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
