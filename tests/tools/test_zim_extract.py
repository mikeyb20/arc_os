#!/usr/bin/env python3
"""Host tests for tools/content/zim_extract.py (M3-03): the HTML conversion table on small fixtures, link
and image paths, and the whole run (SHA-256 check, `zimcat stream` parsing, key order, report, nothing
left behind on failure) with a stand-in for zimcat that prints a prepared stream. The run on a real
fixture ZIM through our zimcat is the lane test lane.zim_extract (stack/tests/zim/)."""

import hashlib
import importlib.util
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(ROOT, "tools/content/zim_extract.py")

spec = importlib.util.spec_from_file_location("zim_extract", SCRIPT)
zx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(zx)

H, L, E, I, S = "\x01", "\x02", "\x03", "\x04", "\x1f"


def stats():
    return {"links": 0, "image_lines": 0, "images_outside": 0, "icons_dropped": 0}


def conv(body, path="Hypothermia", title="Hypothermia", redirects=None, mime="text/html"):
    if isinstance(body, str):
        body = body.encode("utf-8")
    return zx.convert(path, title, body, mime, redirects or {}, stats())[1]


class Resolve(unittest.TestCase):
    def test_paths(self):
        r = zx.resolve
        self.assertEqual(r("Hypothermia", "Hyperthermia"), "Hyperthermia")
        self.assertEqual(r("Hypothermia", "./Frost%20bite#Signs"), "Frost bite")
        self.assertEqual(r("a/b/c", "../d"), "a/d")
        self.assertEqual(r("www.nhs.uk/medicines/a/", "../b/"), "www.nhs.uk/medicines/b/")
        self.assertEqual(r("www.nhs.uk/medicines/a/", "./"), "www.nhs.uk/medicines/a/")
        self.assertEqual(r("x", "_assets_/img.webp?v=2"), "_assets_/img.webp")
        for ref in ("#Signs", "https://en.wikipedia.org/wiki/X", "//host/x", "mailto:a@b", "data:image/png;base64,AA",
                    "../../up", "bad%ff"):
            self.assertIsNone(r("a/b", ref), ref)


class Convert(unittest.TestCase):
    def test_headings_and_title(self):
        text = conv("<h1>Hypothermia</h1><p>Lead.</p><h2>Signs</h2><p>Shivering.</p><h3>Early <a href='X'>signs</a></h3>"
                    "<h1>Second h1</h1>")
        self.assertEqual(text, f"Lead.\n{H}2Signs\nShivering.\n{H}3Early signs\n{H}1Second h1")

    def test_blocks_lists_definitions(self):
        text = conv("<p>One <b>two</b>\n  three</p><ul><li>a<ul><li>b</li></ul></li><li>c</li></ul>"
                    "<dl><dt>Term</dt><dd>Meaning</dd></dl><div>x<br>y</div><p>   </p>")
        self.assertEqual(text, "One two three\n• a\n• b\n• c\nTerm\nMeaning\nx\ny")

    def test_table_rows(self):
        text = conv("<table><tr><th>Specialty</th><td><a href='Critical_care'>Critical care</a></td></tr>"
                    "<tr><th>Types<br>of it</th><td><ul><li>Mild</li><li>Severe</li></ul></td></tr>"
                    "<tr><td><table><tr><td>in</td></tr><tr><td>ner</td></tr></table></td></tr></table><p>After.</p>")
        self.assertEqual(text, f"Specialty\t{L}Critical_care{S}Critical care{E}\nTypes of it\tMild; Severe\nin; ner\n"
                               "After.")

    def test_links(self):
        text = conv("<p>See <a href='Cold'>the cold</a>, <a href='#Signs'>signs</a>, "
                    "<a href='https://example.org/'>web</a> and <a href='Frost'> frost </a>.</p>",
                    redirects={"Cold": "Hypothermia_(cold)"})
        self.assertEqual(text, f"See {L}Hypothermia_(cold){S}the cold{E}, signs, web and {L}Frost{S}frost{E}.")

    def test_dropped(self):
        text = conv("<head><title>T</title><style>p{}</style></head><body><script>x()</script>"
                    "<p>Body<sup class='reference'><a href='#cite'>[1]</a></sup>, more.</p>"
                    "<div class='navbox'>Nav</div><nav>Menu</nav><p style='display: none'>Hidden</p>"
                    "<span class='mw-editsection'>edit</span><div class='zim-footer'>Licence</div></body>")
        self.assertEqual(text, "Body, more.")

    def test_main_only(self):
        text = conv("<body><header>Site header</header><main><h1>Title</h1><p>Inside.</p></main>"
                    "<footer>Foot</footer><p>Outside.</p></body>")
        self.assertEqual(text, "Inside.")

    def test_images(self):
        text = conv("<p>Before</p><figure><a href='File:X'><img src='./_assets_/x.webp' alt='alt'></a>"
                    "<figcaption>The <a href='Y'>caption</a></figcaption></figure>"
                    "<figure><img src='data:image/svg+xml,AA'><figcaption>Only text</figcaption></figure>"
                    "<p>Icon <img src='flag.png' width='20' height='14'> and "
                    "<img class='mwe-math-fallback-image-inline' alt='{\\displaystyle x^{2}}'> and "
                    "<img src='big.png' alt='Big picture'> end</p>")
        self.assertEqual(text, f"Before\n{I}_assets_/x.webp{S}The caption\nOnly text\nIcon and x^{{2}} and end\n"
                               f"{I}big.png{S}Big picture")

    def test_control_and_charset(self):
        text = conv("<p>A\x05B\tC\r\nD caf\xe9</p>".encode("latin-1"), mime="text/html; charset=iso-8859-1")
        self.assertEqual(text, "AB C D café")

    def test_space_before_punctuation(self):
        self.assertEqual(conv("<p>Culturable <sup class='reference'>[3]</sup>, but debated <span hidden>x</span>.</p>"),
                         "Culturable, but debated.")

    def test_markup_is_valid(self):
        """Every line is a heading, an image line, or text whose only control bytes are links and tabs."""
        text = conv("<h2>A <i>b</i></h2><table><tr><td>x\ty</td><td><a href='Z'>z</a></td></tr></table>"
                    "<pre>line 1\n  line 2</pre><p>p\x00q</p>")
        for line in text.split("\n"):
            body = line
            if line.startswith(H):
                self.assertTrue(line[1] in "123456" and zx.CONTROL.search(line[2:]) is None, line)
                continue
            for ch in (L, E, S):
                body = body.replace(ch, "")
            self.assertIsNone(zx.CONTROL.search(body.replace("\t", "")), repr(line))
        self.assertIn("line 1\nline 2", text)


def stream_bytes(articles, redirects=(), others=(), meta=None):
    """What `zimcat stream` prints, for the stand-in."""
    out = [b"zim\t00\t6.3\t9\t2\n"]
    for k, v in (meta or {"Source": "en.wikipedia.org", "Date": "2026-10-10", "Name": "test"}).items():
        out.append(f"meta {k}\t{v}\n".encode())
    for a, b in redirects:
        out.append(f"redirect\t{a}\t{b}\n".encode())
    for path, title, html in articles:
        body = html.encode()
        out.append(f"article\t{path}\t{title}\ttext/html\t{len(body)}\n".encode() + body + b"\n")
    for mime, n, size in others:
        out.append(f"other\t{mime}\t{n}\t{size}\n".encode())
    out.append(f"count\tredirects\t{len(redirects)}\t{len(redirects)}\ncount\tobsolete\t0\nend\t{len(articles)}\n"
               .encode())
    return b"".join(out)


class Run(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.zim = os.path.join(self.dir, "test.zim")
        with open(self.zim, "wb") as f:
            f.write(b"stand-in ZIM bytes")
        self.sha = hashlib.sha256(b"stand-in ZIM bytes").hexdigest()
        self.out = os.path.join(self.dir, "out.jsonl")

    def tearDown(self):
        self.tmp.cleanup()

    def run_extract(self, stream, sha=None, cut=0):
        fake = os.path.join(self.dir, "stream.bin")
        with open(fake, "wb") as f:
            f.write(stream[:len(stream) - cut])
        zimcat = f"sh -c {shlex.quote('cat ' + shlex.quote(fake))} zimcat"
        return subprocess.run([sys.executable, "-I", SCRIPT, "--sha256", sha or self.sha, "--zimcat", zimcat,
                               self.zim, self.out], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def test_run(self):
        arts = [("Zeta", "Zeta", "<p>Last <a href='Cold'>cold</a>.</p>"),
                ("Alpha", "Alpha", "<p>First.</p>"),
                ("Été", "Été", "<p>Summer.</p>"),
                ("Tab\\x09path", "T", "<p>escaped path</p>")]
        p = self.run_extract(stream_bytes(arts, [("Cold", "Alpha")], [("image/webp", 3, 3000), ("text/css", 1, 10)]))
        self.assertEqual(p.returncode, 0, p.stderr)
        with open(self.out, encoding="utf-8") as f:
            lines = [json.loads(x) for x in f]
        self.assertEqual(lines[0], {"stream": 1, "source": "en.wikipedia.org", "zim": "test.zim", "date": "2026-10-10",
                                    "name": "test", "title": "", "license": "", "sha256": self.sha})
        keys = [d["key"] for d in lines[1:]]
        self.assertEqual(keys, ["Alpha", "Tab\tpath", "Zeta", "Été"])  # UTF-8 byte order
        self.assertEqual(sorted(keys, key=lambda k: k.encode()), keys)
        self.assertEqual(lines[3]["text"], f"Last {L}Alpha{S}cold{E}.")  # the redirect's target
        self.assertIn("articles\t4", p.stdout)
        self.assertIn("image image/webp\t3 entries\t3000 bytes", p.stdout)
        self.assertIn("skipped text/css\t1 entries\t10 bytes", p.stdout)
        self.assertEqual(sorted(os.listdir(self.dir)), ["out.jsonl", "stream.bin", "test.zim"])

    def test_sha_mismatch(self):
        p = self.run_extract(stream_bytes([("A", "A", "<p>x</p>")]), sha="0" * 64)
        self.assertEqual(p.returncode, 1)
        self.assertIn("SHA-256", p.stderr)
        self.assertFalse(os.path.exists(self.out))

    def test_cut_short(self):
        for cut in (1, 10, 40):
            p = self.run_extract(stream_bytes([("A", "A", "<p>" + "x" * 30 + "</p>")]), cut=cut)
            self.assertEqual(p.returncode, 1, cut)
            self.assertEqual(sorted(os.listdir(self.dir)), ["stream.bin", "test.zim"], cut)

    def test_bad_sha_argument(self):
        p = self.run_extract(b"", sha="xyz")
        self.assertEqual(p.returncode, 2)


if __name__ == "__main__":
    unittest.main()
