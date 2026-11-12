#!/usr/bin/env python3
"""Byte budget of the storage tiers and pack size estimates (M3-01).

Host only, standard library only; run with `python3 -I`.

  budget.py                      print every term of the 32 GB and 1 TB budgets
  budget.py --sources FILE       also estimate each pack of a sources.json and check
                                 each tier's packs, in priority order, against its budget

Every value below marked ASSUMED is a placeholder until the block named next to it
measures or fixes it; change it here and rerun. Sizes are decimal bytes (1 GB = 10^9 B),
the way disks are sold.

Pack estimate (M3-01 spec; M3-03, M3-04 and M3-04b measure the real numbers):
  text    = the nopic ZIM's size; a source with no nopic flavour counts its whole ZIM
            (an upper bound: its images are counted as text)
  chunks  = 32 B x (words / 280)
  fts     = 0.5 x text
  vectors = chunks x 384 x 1 B           (counted from the first build of a tier)
  words   = given in the entry, else nopic bytes x WORDS_PER_NOPIC_BYTE (ASSUMED)
Images are not in packs yet (M3-08); `maxi - nopic` is reported as the image share.
"""

import argparse
import json
import sys

GB = 10**9
MiB = 2**20
GiB = 2**30

# --- Assumptions (each one is ASSUMED until the named block fixes it) -------------
GPT_BYTES = 2 * MiB            # ASSUMED: 1 MiB alignment + backup GPT (M5 fixes layout)
ESP_BYTES = 256 * MiB          # ASSUMED: ESP size, fixed in M5 (ADR-0008)
SYSTEM_BYTES = 2 * GiB         # ASSUMED: system partition (/system, /apps), fixed in M5
FS_OVERHEAD = 0.02             # ASSUMED: ext2 metadata share of the data partition
RESERVE = 0.10                 # free-space reserve (M3-01 spec)
CHUNK_RECORD_BYTES = 32        # chunk record size (pack-v1.md)
CHUNK_WORDS = 280              # target words per chunk (content-lane.md)
FTS_RATIO = 0.5                # ASSUMED: FTS index / text, measured by M3-04
VECTOR_BYTES = 384             # ASSUMED: 384 dimensions x int8, measured by M3-04b
WORDS_PER_NOPIC_BYTE = None    # ASSUMED: set from a measured anchor (see --words)

# Models per tier (knowledge-os.md §4; ASSUMED until M2-06 measures them).
MODELS = {
    "32gb": [
        ("low-tier chat model (1-2B, 4-bit; §4 '~1-1.5 GB', upper end)", int(1.5 * GB)),
        ("embedder (§4 '~100 MB')", 100 * 10**6),
    ],
    "1tb": [
        ("low-tier chat model (1-2B, 4-bit; §4 '~1-1.5 GB', upper end)", int(1.5 * GB)),
        ("mid-tier chat model (7-8B, 4-bit; §4 '~5 GB')", 5 * GB),
        ("high-tier chat model (14B, 4-bit; not sized in §4, ~9 GB is a 4-bit 14B GGUF)", 9 * GB),
        ("embedder (§4 '~100 MB')", 100 * 10**6),
    ],
}

# Maps are a budget line only: M3-10 and M4-13 define map content (ASSUMED sizes).
MAPS = {
    "32gb": ("regional maps (§4 storage tiers)", 2 * GB),
    "1tb": ("world maps (§4 storage tiers)", 100 * GB),
}

TIERS = {"32gb": 32 * GB, "1tb": 1000 * GB}


def budget(tier):
    """Terms of a tier's pack budget: list of (label, bytes); the last is what's left."""
    disk = TIERS[tier]
    terms = [(f"disk ({tier}, decimal)", disk),
             ("- GPT and alignment (assumed)", -GPT_BYTES),
             ("- ESP (assumed)", -ESP_BYTES),
             ("- system partition (assumed)", -SYSTEM_BYTES)]
    data = disk - GPT_BYTES - ESP_BYTES - SYSTEM_BYTES
    overhead = int(data * FS_OVERHEAD)
    terms.append((f"- ext2 metadata ({FS_OVERHEAD:.0%} of data partition, assumed)", -overhead))
    usable = data - overhead
    reserve = int(usable * RESERVE)
    terms.append((f"- free-space reserve ({RESERVE:.0%} of usable)", -reserve))
    left = usable - reserve
    for label, size in MODELS[tier]:
        terms.append((f"- {label} (assumed)", -size))
        left -= size
    label, size = MAPS[tier]
    terms.append((f"- {label} (assumed)", -size))
    left -= size
    terms.append(("= left for packs", left))
    return terms


def pack_budget(tier):
    return budget(tier)[-1][1]


def estimate(entry, words_per_byte=WORDS_PER_NOPIC_BYTE):
    """Estimated installed size of one sources.json entry: dict of terms, or None if
    an input is missing (the entry is then reported, not guessed)."""
    est = entry.get("estimate", {})
    nopic = est.get("nopic_bytes")
    basis = "nopic"
    if not isinstance(nopic, int) or nopic <= 0:
        nopic, basis = est.get("zim_bytes"), "whole ZIM"
        if not isinstance(nopic, int) or nopic <= 0:
            return None
    words = est.get("words")
    if not isinstance(words, int):
        if words_per_byte is None:
            return None
        words = int(nopic * words_per_byte)
    chunks = -(-words // CHUNK_WORDS)
    out = {"text": nopic, "chunks": chunks * CHUNK_RECORD_BYTES,
           "fts": int(nopic * FTS_RATIO), "vectors": chunks * VECTOR_BYTES, "words": words,
           "chunk_count": chunks, "basis": basis}
    out["total"] = out["text"] + out["chunks"] + out["fts"] + out["vectors"]
    maxi = est.get("maxi_bytes")
    out["images"] = maxi - nopic if isinstance(maxi, int) and maxi >= nopic else None
    return out


def check_tier(tier, sources, words_per_byte):
    """Packs of a tier in priority order with running totals; returns (rows, ok)."""
    limit = pack_budget(tier)
    picked = sorted((s for s in sources if tier in s.get("tiers", [])),
                    key=lambda s: (s.get("priority", 1 << 30), s.get("name", "")))
    rows, used, ok = [], 0, True
    for s in picked:
        e = estimate(s, words_per_byte)
        if e is None:
            rows.append((s.get("name", "?"), None, used, "no estimate: input missing"))
            ok = False
            continue
        used += e["total"]
        note = "" if used <= limit else "OVER BUDGET"
        ok = ok and used <= limit
        rows.append((s["name"], e["total"], used, note))
    return rows, ok, limit


def fmt(n):
    return f"{n:>18,} B  {n / GB:9.2f} GB  {n / GiB:9.2f} GiB"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sources", help="sources.json to estimate and check")
    ap.add_argument("--words", nargs=2, type=int, metavar=("WORDS", "NOPIC_BYTES"),
                    help="anchor for WORDS_PER_NOPIC_BYTE: a known word count and its nopic ZIM size")
    a = ap.parse_args(argv)
    wpb = WORDS_PER_NOPIC_BYTE
    if a.words:
        wpb = a.words[0] / a.words[1]

    for tier in TIERS:
        print(f"[{tier}]")
        for label, n in budget(tier):
            print(f"  {label:<84} {fmt(n)}")
    if not a.sources:
        return 0

    with open(a.sources, encoding="utf-8") as f:
        sources = json.load(f)["sources"]
    print(f"\nwords per nopic byte: {wpb if wpb is not None else 'not set'}")
    for s in sources:
        e = estimate(s, wpb)
        if e is None:
            print(f"  {s.get('name', '?'):<32} no estimate (nopic size or words missing)")
            continue
        img = "-" if e["images"] is None else f"{e['images'] / GB:.2f}"
        print(f"  {s['name']:<32} text {e['text'] / GB:7.2f}  fts {e['fts'] / GB:6.2f}  "
              f"chunks {e['chunks'] / GB:5.2f}  vectors {e['vectors'] / GB:5.2f}  "
              f"total {e['total'] / GB:7.2f} GB  images {img} GB  ({e['chunk_count']:,} chunks; "
              f"text from {e['basis']})")
    all_ok = True
    for tier in TIERS:
        rows, ok, limit = check_tier(tier, sources, wpb)
        all_ok = all_ok and ok
        print(f"\n[{tier}] budget {limit / GB:.2f} GB")
        for name, size, used, note in rows:
            size_s = "?" if size is None else f"{size / GB:7.2f}"
            print(f"  {name:<32} {size_s} GB  running {used / GB:8.2f} GB  {note}")
        print(f"  left {(limit - rows[-1][2]) / GB if rows else limit / GB:.2f} GB")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
