#!/usr/bin/env python3
"""Read the Kiwix catalog (M3-01): the candidate ZIMs for the content list.

Host only, standard library only; run with `python3 -I`. Cloud sessions can't reach
library.kiwix.org, so .github/workflows/catalog.yml runs this on a GitHub runner and
prints the table into its log; it also runs on the build host.

  catalog.py [--lang eng] [--from FEED.xml] [--save FEED.xml] [--out TABLE.tsv]
             [--meta REGEX]

Reads the OPDS feed (catalog v2) and writes one tab-separated row per ZIM, sorted by
category, name, flavour: the values exactly as the catalog gives them, nothing
estimated. `--meta REGEX` also reads the License and Source metadata of every ZIM
whose name matches, from the ZIM itself through the server's /raw/<book>/meta/<key>
endpoint (one ZIM per name: metadata doesn't change between flavours); a ZIM without
that metadata leaves the cell empty, which the content list reports as a gap.
"""

import argparse
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

SERVER = "https://library.kiwix.org"
FEED = SERVER + "/catalog/v2/entries?lang={lang}&count=-1"
ZIM_LINK_REL = "http://opds-spec.org/acquisition/open-access"
AGENT = "arc_os-content-catalog"
META_KEYS = ("License", "Source")
COLUMNS = ("category", "name", "flavour", "date", "size_bytes", "article_count",
           "media_count", "title", "creator", "publisher", "tags", "url", "book")
DATE_IN_FILE = re.compile(r"_(\d{4}-\d{2})\.zim(?:\.meta4)?$")


def local(tag):
    """Element tag without its namespace."""
    return tag.rsplit("}", 1)[-1]


def child_text(elem, name):
    for c in elem:
        if local(c.tag) == name:
            return (c.text or "").strip()
    return ""


def nested_name(elem, name):
    """Text of <name> inside <author>/<publisher>."""
    for c in elem:
        if local(c.tag) == name:
            return child_text(c, "name")
    return ""


def clean(value):
    """One table cell: no tabs or line breaks."""
    return " ".join(value.split())


def parse_entry(entry):
    row = {k: "" for k in COLUMNS}
    row["name"] = child_text(entry, "name")
    row["flavour"] = child_text(entry, "flavour")
    row["category"] = child_text(entry, "category")
    row["title"] = child_text(entry, "title")
    row["tags"] = child_text(entry, "tags")
    row["article_count"] = child_text(entry, "articleCount")
    row["media_count"] = child_text(entry, "mediaCount")
    row["creator"] = nested_name(entry, "author")
    row["publisher"] = nested_name(entry, "publisher")
    for link in entry:
        if local(link.tag) != "link":
            continue
        href = link.get("href", "")
        if link.get("rel") == ZIM_LINK_REL:
            url = href[:-len(".meta4")] if href.endswith(".meta4") else href
            row["url"] = url
            row["size_bytes"] = link.get("length", "")
            m = DATE_IN_FILE.search(href)
            row["date"] = m.group(1) if m else ""
        elif link.get("type") == "text/html" and "/content/" in href:
            row["book"] = urllib.parse.unquote(href.split("/content/", 1)[1].strip("/"))
    if not row["book"] and row["url"].endswith(".zim"):
        # The server names a book after its file unless the feed says otherwise.
        row["book"] = row["url"].rsplit("/", 1)[-1][:-len(".zim")]
    return {k: clean(v) for k, v in row.items()}


def parse_feed(data):
    """Rows of an OPDS feed, sorted by category, name, flavour, date."""
    root = ET.fromstring(data)
    rows = [parse_entry(e) for e in root if local(e.tag) == "entry"]
    return sorted(rows, key=lambda r: (r["category"], r["name"], r["flavour"], r["date"]))


def fetch(url, timeout=120):
    req = urllib.request.Request(url, headers={"User-Agent": AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def read_meta(rows, pattern, getter=fetch):
    """Add License and Source columns for rows whose name matches `pattern`."""
    seen = {}
    for row in rows:
        for key in META_KEYS:
            row[key.lower()] = ""
        if not pattern.search(row["name"]) or not row["book"]:
            continue
        if row["name"] not in seen:
            meta = {}
            for key in META_KEYS:
                url = f"{SERVER}/raw/{urllib.parse.quote(row['book'])}/meta/{key}"
                try:
                    meta[key.lower()] = clean(getter(url).decode("utf-8", "replace"))
                except (urllib.error.URLError, OSError):
                    meta[key.lower()] = ""
                time.sleep(0.2)
            seen[row["name"]] = meta
        row.update(seen[row["name"]])


def write_table(rows, out, columns):
    out.write("\t".join(columns) + "\n")
    for r in rows:
        out.write("\t".join(r.get(c, "") for c in columns) + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--lang", default="eng")
    ap.add_argument("--from", dest="src", help="read a saved feed instead of the server")
    ap.add_argument("--save", help="also save the raw feed here")
    ap.add_argument("--out", help="write the table here (default: stdout)")
    ap.add_argument("--meta", help="read License and Source for names matching this regex")
    a = ap.parse_args(argv)

    if a.src:
        with open(a.src, "rb") as f:
            data = f.read()
    else:
        data = fetch(FEED.format(lang=urllib.parse.quote(a.lang)))
    if a.save:
        with open(a.save, "wb") as f:
            f.write(data)
    rows = parse_feed(data)
    columns = COLUMNS
    if a.meta:
        read_meta(rows, re.compile(a.meta))
        columns = COLUMNS + tuple(k.lower() for k in META_KEYS)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            write_table(rows, f, columns)
    else:
        write_table(rows, sys.stdout, columns)
    print(f"catalog: {len(rows)} entries", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
