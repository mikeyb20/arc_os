#!/usr/bin/env python3
"""Turn a Kiwix ZIM into an article stream (M3-03): docs/formats/article-stream.md.

Host only, standard library only; run with `python3 -I`.

  zim_extract.py --sha256 HEX [--zimcat CMD] ZIM OUT.jsonl

1. Checks the ZIM's SHA-256 against HEX (from sources.json, filled from Kiwix's .sha256 file) before
   anything else: libzim's zstd frames carry no checksum, so a damaged file could decode to wrong text.
2. Runs `zimcat stream ZIM` (our reader, ADR-0022; CMD is split like a shell command, default `zimcat`),
   which sends the HTML articles in cluster order, so each cluster is decompressed once.
3. Converts one article at a time from HTML to pack-v1 markup with html.parser (table-driven: the
   tables below say which tags are blocks, headings, list items, table cells, and what is dropped),
   writing each converted article to a temporary file and remembering (key, offset, length).
4. Writes OUT.jsonl in key order (UTF-8 byte order) from that index, then renames it into place.
5. Prints a report: articles, redirects, skipped entries per MIME type, the image types and bytes.

Links name their target by key (the ZIM path); a link to a redirect names the redirect's target, so
packbuild can resolve it. Images are not copied: an image line keeps the image's ZIM path and caption.
Cost: memory = the redirect map (two paths per redirect) + one article + the index (key and two ints
per article); time linear in the HTML. The temporary file is about the size of the stream.
"""

import argparse
import hashlib
import html.parser
import json
import os
import re
import shlex
import subprocess
import sys
import urllib.parse

STREAM_VERSION = 1
LINE_MAX = 64 << 20  # bytes of one stream line (article-stream.md)

# pack-v1 markup bytes (docs/formats/pack-v1.md, "Document text")
HEADING, LINK, LINK_END, IMAGE, SEP = "\x01", "\x02", "\x03", "\x04", "\x1f"
BULLET = "• "

# ---- Conversion tables -------------------------------------------------------------------------

# Elements dropped with everything inside them.
DROP_TAGS = {"head", "title", "script", "style", "noscript", "template", "nav", "footer", "form", "button",
             "input", "select", "textarea", "label", "iframe", "object", "embed", "audio", "video", "canvas",
             "svg", "math", "map", "search", "dialog"}
# Elements dropped by class: navigation boxes, reference markers and lists, edit links, hidden parts,
# the ZIM footer (licence line), notices.
DROP_CLASSES = {"navbox", "navbox-styles", "vertical-navbox", "navigation-not-searchable", "catlinks",
                "zim-footer", "mw-editsection", "reference", "mw-ref", "mw-references-wrap", "reflist",
                "refbegin", "references", "mw-cite-backlink", "noprint", "metadata", "ambox", "mbox-small",
                "sistersitebox", "side-box", "portalbox", "shortdescription", "printfooter", "toc",
                "mw-empty-elt", "mwe-math-mathml-inline", "mwe-math-mathml-display", "nhsuk-skip-link",
                "nhsuk-header", "nhsuk-footer", "nhsuk-breadcrumb", "nhsuk-back-link", "nhsuk-u-visually-hidden"}
DROP_ROLES = {"navigation", "banner", "contentinfo", "search", "note"}
DROP_IDS = {"toc", "catlinks", "siteSub", "contentSub", "jump-to-nav", "mw-navigation", "footer"}
# Elements whose start and end end the current line.
BLOCK_TAGS = {"p", "div", "section", "article", "main", "header", "aside", "blockquote", "center", "ul", "ol",
              "dl", "table", "caption", "thead", "tbody", "tfoot", "hr", "address", "details", "summary",
              "body", "html", "figure", "pre", "fieldset"}
HEADING_TAGS = {"h1": 1, "h2": 2, "h3": 3, "h4": 4, "h5": 5, "h6": 6}
ITEM_TAGS = {"li": BULLET, "dt": "", "dd": ""}  # a line each, with this prefix
CELL_TAGS = {"td", "th"}
VOID_TAGS = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source",
             "track", "wbr"}
ICON_MAX = 32  # an <img> no larger than this (width and height, pixels) is an icon and dropped
MATH_CLASS = "mwe-math-fallback-image"  # mwoffliner's formula images: their alt text is the formula

CONTROL = re.compile("[\x00-\x1f\x7f-\x9f]")
SPACE = re.compile(r"\s+")
SPACE_BEFORE_PUNCT = re.compile(r" +(?=[,.;:!?)\]])")
MAIN = re.compile(rb"<main[\s>]", re.I)


def clean(text):
    """Collapse whitespace (newlines and tabs too) and drop control characters."""
    return CONTROL.sub("", SPACE.sub(" ", text))


def resolve(base, ref):
    """ZIM path that `ref` (an href or src) names from an article at path `base`, or None if it leaves the
    ZIM (another scheme or host, a data: URI, only a fragment, or above the root)."""
    parts = urllib.parse.urlsplit(ref)
    if parts.scheme or parts.netloc or not parts.path:
        return None
    try:
        path = urllib.parse.unquote(parts.path, errors="strict") if "%" in parts.path else parts.path
    except UnicodeDecodeError:
        return None
    out = [] if path.startswith("/") else base.split("/")[:-1]
    segs = path.lstrip("/").split("/")
    for i, seg in enumerate(segs):
        if seg == "..":
            if not out:
                return None
            out.pop()
        elif seg != "." and (seg or i == len(segs) - 1):
            out.append(seg)
    key = "/".join(out)
    if segs[-1] in (".", "..") and key:
        key += "/"
    return key if key and not CONTROL.search(key) else None


def attr_map(attrs):
    return {k: (v if v is not None else "") for k, v in attrs}


class Converter(html.parser.HTMLParser):
    """One article's HTML to pack-v1 text (links by key). Feed it, call close(), read .lines."""

    def __init__(self, path, redirects, stats, main_only):
        super().__init__(convert_charrefs=True)
        self.path = path
        self.redirects = redirects
        self.stats = stats
        self.stack = []        # open elements: (tag, flags)
        self.drop = 0          # depth inside dropped elements
        self.main = 0 if main_only else 1  # >0: inside the part we keep
        self.lines = []        # finished lines (strings)
        self.line = []         # tokens of the current line: str or ("link", key, text)
        self.prefix = ""       # list bullet for the current line
        self.link = None       # [key, text parts] while inside <a>
        self.heading = None    # [level, text parts] while inside <h1>..<h6>
        self.seen_h1 = False
        self.row = None        # cells of the current outermost table row: list of token lists
        self.table = 0         # table depth
        self.pre = 0
        self.figure = None     # {"src", "alt", "caption"} while inside <figure>
        self.pending = []      # image lines to add after the current line or row

    # -- output --
    def text_out(self, text):
        """Append text to wherever text goes now (link, heading, figure caption, cell, line)."""
        if self.heading is not None:
            self.heading[1].append(text)
        elif self.figure is not None and self.figure["in_caption"]:
            self.figure["caption"].append(text)
        elif self.link is not None:
            self.link[1].append(text)
        else:
            self.target().append(text)

    def target(self):
        if self.row is not None and self.row:
            return self.row[-1]
        return self.line

    def soft_break(self):
        """A block boundary: a new line, or inside a table cell a '; ' separator."""
        if self.row is not None:
            cell = self.target()
            last = last_text(cell)
            if last is not None and not (isinstance(last, str) and last.rstrip().endswith(";")):
                cell.append("; ")
            return
        self.end_line()

    def end_line(self):
        if self.link is not None:  # a block inside a link: the link becomes text
            self.close_link()
        text = tidy(render(self.line))
        if text:
            self.lines.append(self.prefix + text)
        self.line = []
        self.prefix = ""
        self.flush_images()

    def flush_images(self):
        if self.row is None:
            self.lines.extend(self.pending)
            self.pending = []

    def end_row(self):
        if self.row is None:
            return
        cells = [tidy(render(c)).rstrip(";").rstrip(" ") for c in self.row]
        cells = [c for c in cells if c]
        self.row = None
        if cells:
            self.end_line()
            self.lines.append("\t".join(cells))
        self.flush_images()

    def close_link(self):
        key, parts = self.link
        self.link = None
        raw = "".join(p if isinstance(p, str) else p[2] for p in parts)
        text = clean(raw).strip(" ")
        if not text:
            return
        lead = " " if raw[:1].isspace() else ""
        trail = " " if raw[-1:].isspace() else ""
        tgt = self.target()
        if key is None:
            tgt.append(lead + text + trail)
        else:
            self.stats["links"] += 1
            tgt.extend([lead, ("link", key, text), trail])

    def add_image(self, src, caption, caption_as_text):
        """An image line for `src` after the current line; if src is not in the ZIM, the caption as
        text (figures) or nothing (inline images)."""
        ref = resolve(self.path, src) if src else None
        caption = clean(caption).strip(" ")
        if ref is None:
            self.stats["images_outside"] += 1
            if caption and caption_as_text:
                self.text_out(caption)
            return
        self.stats["image_lines"] += 1
        self.pending.append(IMAGE + ref + SEP + caption)

    # -- parser callbacks --
    def handle_starttag(self, tag, attrs):
        a = attr_map(attrs)
        if tag == "main":
            self.main += 1
        if self.drop or dropped(tag, a):
            if tag not in VOID_TAGS:
                self.drop += 1
                self.stack.append((tag, "drop"))
            return
        if tag not in VOID_TAGS:
            self.stack.append((tag, ""))
        if not self.main:
            return
        if tag in HEADING_TAGS and self.row is None:
            self.end_line()
            self.heading = [HEADING_TAGS[tag], []]
        elif tag in ITEM_TAGS:
            self.soft_break()
            if self.row is None:
                self.prefix = ITEM_TAGS[tag]
        elif tag == "tr":
            if self.table == 1:
                self.end_row()
                self.row = []
            else:
                self.soft_break()
        elif tag in CELL_TAGS:
            if self.table == 1 and self.row is not None:
                self.row.append([])
            else:
                self.soft_break()
        elif tag == "table":
            self.table += 1
            if self.table == 1:
                self.end_line()
            else:
                self.soft_break()
        elif tag == "br":
            if self.pre or (self.row is None and self.heading is None and self.link is None and self.figure is None):
                self.end_line()
            else:
                self.text_out(" ")  # inside a cell, link, heading or caption a line break is a space
        elif tag == "a":
            if self.link is not None:
                self.close_link()
            href = a.get("href", "")
            key = None if self.heading is not None else resolve(self.path, href) if href else None
            if key is not None:
                key = self.redirects.get(key, key)
            self.link = [key, []]
        elif tag == "figure":
            self.end_line()
            self.figure = {"src": None, "alt": "", "caption": [], "in_caption": False}
        elif tag == "figcaption" and self.figure is not None:
            self.figure["in_caption"] = True
        elif tag == "img":
            self.image(a)
        elif tag == "pre":
            self.end_line()
            self.pre += 1
        elif tag in BLOCK_TAGS:
            self.soft_break()

    def image(self, a):
        src, alt = a.get("src", ""), a.get("alt", "")
        if self.figure is not None:
            if self.figure["src"] is None:
                self.figure["src"], self.figure["alt"] = src, alt
            return
        if MATH_CLASS in a.get("class", ""):
            formula = clean(alt).strip(" ")
            if formula.startswith("{\\displaystyle") and formula.endswith("}"):
                formula = formula[len("{\\displaystyle"):-1].strip(" ")
            self.text_out(formula)
            return
        try:
            small = int(a.get("width", "999")) <= ICON_MAX and int(a.get("height", "999")) <= ICON_MAX
        except ValueError:
            small = False
        if small:
            self.stats["icons_dropped"] += 1
            return
        self.add_image(src, alt, False)
        if self.heading is None and self.link is None and last_text(self.line) is None:
            self.flush_images()  # at the start of a line: the image line goes here

    def handle_endtag(self, tag):
        if tag in VOID_TAGS:
            return
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i][0] == tag:
                break
        else:
            return  # an end tag that closes nothing
        while len(self.stack) > i:
            t, flag = self.stack.pop()
            if flag == "drop":
                self.drop -= 1
                if t == "main":
                    self.main -= 1
                continue
            self.close_tag(t)

    def close_tag(self, tag):
        if tag == "main":
            self.end_line()
            self.main -= 1
            return
        if not self.main:
            return
        if tag in HEADING_TAGS and self.heading is not None:
            level, parts = self.heading
            self.heading = None
            text = clean("".join(parts)).strip(" ")
            if level == 1 and not self.seen_h1:
                self.seen_h1 = True  # the page title: the document's title already says it
            elif text:
                self.lines.append(HEADING + str(level) + text)
            self.flush_images()
        elif tag == "a" and self.link is not None:
            self.close_link()
        elif tag == "table":
            if self.table == 1:
                self.end_row()
                self.end_line()
            else:
                self.soft_break()
            self.table = max(0, self.table - 1)
        elif tag == "tr" and self.table == 1:
            self.end_row()
        elif tag == "figcaption" and self.figure is not None:
            self.figure["in_caption"] = False
        elif tag == "figure" and self.figure is not None:
            f, self.figure = self.figure, None
            caption = "".join(f["caption"]) or f["alt"]
            self.add_image(f["src"], caption, True)
            self.end_line()
        elif tag == "pre":
            self.end_line()
            self.pre = max(0, self.pre - 1)
        elif tag in ITEM_TAGS or tag in BLOCK_TAGS or tag in CELL_TAGS:
            if tag in CELL_TAGS and self.table == 1 and self.row is not None:
                return
            self.soft_break()

    def handle_data(self, data):
        if self.drop or not self.main:
            return
        if self.pre and self.heading is None and self.link is None and self.row is None:
            for i, part in enumerate(data.split("\n")):
                if i:
                    self.end_line()
                if part.strip():
                    self.line.append(CONTROL.sub("", part.replace("\t", " ")))
            return
        self.text_out(data)

    def finish(self):
        """Close everything still open; the document text (lines joined by newlines)."""
        self.close()  # html.parser's own: hands over text it still buffers
        while self.stack:
            self.handle_endtag(self.stack[-1][0])
        if self.link is not None:
            self.close_link()
        self.end_row()
        self.end_line()
        self.flush_images()
        return "\n".join(self.lines)


def dropped(tag, a):
    if tag in DROP_TAGS:
        return True
    if "hidden" in a or a.get("aria-hidden") == "true" or a.get("role") in DROP_ROLES or a.get("id") in DROP_IDS:
        return True
    if "display:none" in a.get("style", "").replace(" ", "").lower():
        return True
    return bool(DROP_CLASSES.intersection(a.get("class", "").split()))


def tidy(line):
    """A finished line: no spaces at its ends, none left before punctuation where a dropped element
    (a reference marker, say) stood between them."""
    return SPACE_BEFORE_PUNCT.sub("", line).strip(" ")


def last_text(tokens):
    """The last token holding more than spaces (a string or a link), or None."""
    for t in reversed(tokens):
        if not isinstance(t, str) or t.strip():
            return t
    return None


def render(tokens):
    """Tokens of a line to text: collapsed spaces, links as pack-v1 links by key."""
    out = []
    for t in tokens:
        if isinstance(t, str):
            s = clean(t)
            if out and out[-1].endswith(" ") and s.startswith(" "):
                s = s[1:]
            if s:
                out.append(s)
        else:
            if t[1] is None:
                out.append(t[2])
            else:
                out.append(LINK + t[1] + SEP + t[2] + LINK_END)
    return "".join(out)


def convert(path, title, body, mime, redirects, stats):
    """One article: (title, text) in pack-v1 markup with links by key."""
    m = re.search(r"charset=([\w.:-]+)", mime, re.I)
    try:
        doc = body.decode(m.group(1) if m else "utf-8", errors="replace")
    except LookupError:
        doc = body.decode("utf-8", errors="replace")
    conv = Converter(path, redirects, stats, MAIN.search(body) is not None)
    conv.feed(doc)
    text = conv.finish()
    title = clean(title).strip(" ") or clean(path).strip(" ") or path
    return title, text


def unescape(field):
    """A zimcat field: \\xNN back to the byte, then UTF-8."""
    raw = re.sub(rb"\\x([0-9a-f]{2})", lambda m: bytes([int(m.group(1), 16)]), field)
    return raw.decode("utf-8")


class StreamError(Exception):
    pass


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def extract(zim, out, zimcat, report):
    """Run zimcat stream on `zim` and write the article stream to `out`; returns the stats."""
    stats = {"articles": 0, "redirects": 0, "redirects_to_articles": 0, "obsolete": 0, "links": 0,
             "image_lines": 0, "images_outside": 0, "icons_dropped": 0, "empty_articles": 0,
             "bad_paths": 0, "bad_article_paths": 0, "failed": [], "other": {}, "meta": {}}
    redirects = {}
    tmp = out + ".part-articles"
    final = out + ".part"
    index = []  # (key bytes, offset, length)
    proc = subprocess.Popen(zimcat + ["stream", zim], stdout=subprocess.PIPE)
    try:
        with open(tmp, "wb") as t:
            read_stream(proc.stdout, t, index, redirects, stats)
        proc.stdout.close()
        if proc.wait() != 0:
            raise StreamError(f"zimcat stream exited with status {proc.returncode}")
        index.sort(key=lambda r: r[0])
        meta = stats["meta"]
        header = {"stream": STREAM_VERSION, "source": meta.get("Source", ""), "zim": os.path.basename(zim),
                  "date": meta.get("Date", ""), "name": meta.get("Name", ""), "title": meta.get("Title", ""),
                  "license": meta.get("License", ""), "sha256": report["sha256"]}
        with open(tmp, "rb") as t, open(final, "wb") as f:
            f.write(json.dumps(header, ensure_ascii=False).encode() + b"\n")
            for _, off, length in index:
                t.seek(off)
                f.write(t.read(length))
            f.flush()
            os.fsync(f.fileno())
        os.rename(final, out)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        for p in (tmp, final):
            if os.path.exists(p):
                os.unlink(p)
    return stats


def read_stream(src, tmp, index, redirects, stats):
    """Read zimcat stream's output; convert each article into `tmp`, recording it in `index`."""
    ended = False
    for line in iter(src.readline, b""):
        if not line.endswith(b"\n"):
            raise StreamError("zimcat stream output cut short")
        fields = line[:-1].split(b"\t")
        kind = fields[0]
        if ended:
            raise StreamError("zimcat stream wrote past its end line")
        if kind.startswith(b"meta "):
            stats["meta"][unescape(kind[5:])] = unescape(b"\t".join(fields[1:]))
        elif kind == b"redirect" and len(fields) == 3:
            try:
                redirects[unescape(fields[1])] = unescape(fields[2])
            except UnicodeDecodeError:
                stats["bad_paths"] += 1
        elif kind == b"article" and len(fields) == 5:
            size = int(fields[4])
            body = src.read(size)
            if len(body) != size or src.read(1) != b"\n":
                raise StreamError("zimcat stream article cut short")
            try:
                path, title = unescape(fields[1]), unescape(fields[2])
            except UnicodeDecodeError:
                stats["bad_paths"] += 1
                stats["bad_article_paths"] += 1
                continue
            try:
                title, text = convert(path, title, body, fields[3].decode("ascii", "replace"), redirects, stats)
            except Exception as e:  # html.parser on HTML it cannot take: skip the article, name it
                stats["failed"].append(f"{path}: {type(e).__name__}: {e}")
                continue
            stats["articles"] += 1
            stats["empty_articles"] += not text
            rec = json.dumps({"key": path, "title": title, "text": text}, ensure_ascii=False).encode() + b"\n"
            if len(rec) - 1 > LINE_MAX:
                raise StreamError(f"article {path!r} is longer than {LINE_MAX} bytes as a stream line")
            index.append((path.encode(), tmp.tell(), len(rec)))
            tmp.write(rec)
        elif kind == b"other" and len(fields) == 4:
            stats["other"][unescape(fields[1])] = (int(fields[2]), int(fields[3]))
        elif kind == b"count" and fields[1] == b"redirects":
            stats["redirects"], stats["redirects_to_articles"] = int(fields[2]), int(fields[3])
        elif kind == b"count" and fields[1] == b"obsolete":
            stats["obsolete"] = int(fields[2])
        elif kind == b"end":
            if int(fields[1]) != stats["articles"] + stats["bad_article_paths"] + len(stats["failed"]):
                raise StreamError("zimcat stream's article count does not match what was read")
            ended = True
        elif kind != b"zim":
            raise StreamError(f"unexpected zimcat stream line: {line[:80]!r}")
    if not ended:
        raise StreamError("zimcat stream output has no end line")


def print_report(stats, report, out):
    images = {m: v for m, v in stats["other"].items() if m.startswith("image/")}
    rows = [("zim", report["zim"]), ("sha256", report["sha256"] + " (checked)"), ("stream", out),
            ("articles", stats["articles"]), ("articles without text", stats["empty_articles"]),
            ("redirects", f"{stats['redirects']} ({stats['redirects_to_articles']} to articles, used for links)"),
            ("obsolete entries", stats["obsolete"]), ("paths not UTF-8 (skipped)", stats["bad_paths"]),
            ("links written", stats["links"]), ("image lines", stats["image_lines"]),
            ("images not in the ZIM (dropped)", stats["images_outside"]), ("icons dropped", stats["icons_dropped"])]
    rows.insert(5, ("articles the HTML parser failed on (skipped)", len(stats["failed"])))
    for k, v in rows:
        print(f"{k}\t{v}")
    for f in stats["failed"][:20]:
        print(f"failed\t{f}")
    for m, (n, b) in sorted(stats["other"].items()):
        print(f"skipped {m}\t{n} entries\t{b} bytes")
    for m, (n, b) in sorted(images.items()):
        print(f"image {m}\t{n} entries\t{b} bytes")
    if not images:
        print("image types\tnone in the ZIM")


def main():
    ap = argparse.ArgumentParser(description="ZIM to article stream (M3-03)")
    ap.add_argument("--sha256", required=True, help="the ZIM's SHA-256 (sources.json); checked before anything else")
    ap.add_argument("--zimcat", default="zimcat", help="zimcat command (split like a shell command)")
    ap.add_argument("zim")
    ap.add_argument("out")
    args = ap.parse_args()
    want = args.sha256.strip().lower()
    if not re.fullmatch(r"[0-9a-f]{64}", want):
        print("zim_extract: --sha256 needs 64 hex digits", file=sys.stderr)
        return 2
    got = sha256_file(args.zim)
    if got != want:
        print(f"zim_extract: {args.zim}: SHA-256 is {got}, sources.json says {want}: not building", file=sys.stderr)
        return 1
    report = {"zim": args.zim, "sha256": got}
    try:
        stats = extract(args.zim, args.out, shlex.split(args.zimcat), report)
    except (StreamError, OSError, ValueError) as e:
        print(f"zim_extract: {e}", file=sys.stderr)
        return 1
    print_report(stats, report, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
