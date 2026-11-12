#!/usr/bin/env python3
"""Host tests for the content-lane scripts (M3-01): tools/content/catalog.py on a
generated OPDS feed (no network) and tools/content/budget.py's terms and estimates."""

import contextlib
import importlib.util
import io
import json
import os
import re
import tempfile
import unittest
import urllib.error

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, rel))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


catalog = load("catalog", "tools/content/catalog.py")
budget = load("budget", "tools/content/budget.py")

ZIM = "http://opds-spec.org/acquisition/open-access"
FEED = f"""<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xmlns:dc="http://purl.org/dc/terms/">
  <id>urn:uuid:feed</id>
  <entry>
    <name>wikipedia_en_medicine</name><flavour>nopic</flavour><category>wikipedia</category>
    <title>WikiMed\tMedical
      Encyclopedia</title>
    <tags>wikipedia;_pictures:no</tags><articleCount>371615</articleCount><mediaCount>5493</mediaCount>
    <author><name>Wikipedia</name></author><publisher><name>openZIM</name></publisher>
    <link type="text/html" href="/content/wikipedia_en_medicine_nopic_2026-10"/>
    <link rel="{ZIM}" type="application/x-zim"
          href="https://download.kiwix.org/zim/wikipedia/wikipedia_en_medicine_nopic_2026-10.zim.meta4"
          length="1047708672"/>
  </entry>
  <entry>
    <name>wikipedia_en_medicine</name><flavour>maxi</flavour><category>wikipedia</category>
    <link type="text/html" href="https://browse.example.org/content/wikipedia_en_medicine_maxi_2026-10"/>
    <link rel="{ZIM}" type="application/x-zim"
          href="https://download.kiwix.org/zim/wikipedia/wikipedia_en_medicine_maxi_2026-10.zim.meta4"
          length="1864210432"/>
  </entry>
  <entry>
    <name>cooking.stackexchange.com_en_all</name><category>stack_exchange</category>
    <link rel="{ZIM}" type="application/x-zim"
          href="https://download.kiwix.org/zim/stack_exchange/cooking.stackexchange.com_en_all_2026-10.zim"
          length="238098432"/>
  </entry>
  <entry><name>no_file</name><category>other</category></entry>
</feed>
"""


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.rows = catalog.parse_feed(FEED.encode())

    def test_rows_sorted_and_copied_verbatim(self):
        self.assertEqual([(r["name"], r["flavour"]) for r in self.rows],
                         [("no_file", ""), ("cooking.stackexchange.com_en_all", ""),
                          ("wikipedia_en_medicine", "maxi"), ("wikipedia_en_medicine", "nopic")])
        nopic = self.rows[3]
        self.assertEqual(nopic["size_bytes"], "1047708672")
        self.assertEqual(nopic["article_count"], "371615")
        self.assertEqual(nopic["date"], "2026-10")
        self.assertEqual(nopic["creator"], "Wikipedia")
        self.assertEqual(nopic["publisher"], "openZIM")
        self.assertEqual(nopic["url"],
                         "https://download.kiwix.org/zim/wikipedia/wikipedia_en_medicine_nopic_2026-10.zim")
        self.assertEqual(nopic["book"], "wikipedia_en_medicine_nopic_2026-10")
        self.assertEqual(nopic["server"], catalog.SERVER)
        self.assertEqual(nopic["title"], "WikiMed Medical Encyclopedia")   # no tab or newline

    def test_missing_values_stay_empty(self):
        empty = self.rows[0]
        for key in ("size_bytes", "date", "url", "book", "article_count"):
            self.assertEqual(empty[key], "")
        # no content link: book named after the file
        self.assertEqual(self.rows[1]["book"], "cooking.stackexchange.com_en_all_2026-10")

    def test_table_has_one_cell_per_column(self):
        out = io.StringIO()
        catalog.write_table(self.rows, out, catalog.COLUMNS)
        lines = out.getvalue().splitlines()
        self.assertEqual(len(lines), 5)
        self.assertTrue(all(len(l.split("\t")) == len(catalog.COLUMNS) for l in lines))

    def test_meta_once_per_name_and_failure_leaves_blank(self):
        calls = []

        def getter(url):
            calls.append(url)
            if url.endswith("/Source"):
                raise urllib.error.URLError("404")
            if "cooking" in url:
                return b"<!DOCTYPE html><html>Access denied</html>"
            return b"CC-BY-SA-4.0\n"

        with contextlib.redirect_stderr(io.StringIO()):
            catalog.read_meta(self.rows, re.compile(r"^(wikipedia_|cooking)"), getter, pause=0)
        # License + Source, once per name, from the server of its first row
        self.assertEqual(calls[2:], [
            "https://browse.example.org/raw/wikipedia_en_medicine_maxi_2026-10/meta/License",
            "https://browse.example.org/raw/wikipedia_en_medicine_maxi_2026-10/meta/Source"])
        for r in self.rows[2:]:
            self.assertEqual((r["license"], r["source"]), ("CC-BY-SA-4.0", ""))
        self.assertEqual(self.rows[1]["license"], "")      # an HTML page is not metadata
        self.assertEqual(self.rows[0]["license"], "")      # not matched

    def test_cli_reads_saved_feed(self):
        with tempfile.TemporaryDirectory() as d:
            src, out = os.path.join(d, "feed.xml"), os.path.join(d, "t.tsv")
            with open(src, "w") as f:
                f.write(FEED)
            self.assertEqual(catalog.main(["--from", src, "--out", out]), 0)
            with open(out) as f:
                self.assertEqual(len(f.read().splitlines()), 5)


class BudgetTests(unittest.TestCase):
    def test_terms_add_up(self):
        for tier, disk in budget.TIERS.items():
            terms = budget.budget(tier)
            self.assertEqual(terms[0][1], disk)
            self.assertEqual(sum(n for _, n in terms[:-1]), terms[-1][1])
            self.assertGreater(terms[-1][1], 0)
            self.assertTrue(all(n < 0 for _, n in terms[1:-1]))

    def test_estimate_formula(self):
        e = budget.estimate({"estimate": {"nopic_bytes": 1000, "maxi_bytes": 3000,
                                          "words": 2800}})
        self.assertEqual(e["chunk_count"], 10)
        self.assertEqual(e["chunks"], 320)
        self.assertEqual(e["fts"], 500)
        self.assertEqual(e["vectors"], 3840)
        self.assertEqual(e["total"], 1000 + 320 + 500 + 3840)
        self.assertEqual(e["images"], 2000)
        self.assertEqual(e["basis"], "nopic")

    def test_estimate_without_nopic_uses_whole_zim(self):
        e = budget.estimate({"estimate": {"zim_bytes": 1000}}, words_per_byte=0.28)
        self.assertEqual((e["text"], e["words"], e["chunk_count"], e["basis"]),
                         (1000, 280, 1, "whole ZIM"))
        self.assertIsNone(e["images"])

    def test_estimate_missing_input_is_not_guessed(self):
        self.assertIsNone(budget.estimate({"estimate": {}}))
        self.assertIsNone(budget.estimate({"estimate": {"nopic_bytes": 1000}}, words_per_byte=None))

    def test_tier_check_in_priority_order_flags_overrun(self):
        limit = budget.pack_budget("32gb")
        big = {"name": "big", "priority": 2, "tiers": ["32gb"],
               "estimate": {"nopic_bytes": limit, "words": 0}}
        small = {"name": "small", "priority": 1, "tiers": ["32gb", "1tb"],
                 "estimate": {"nopic_bytes": 1000, "words": 0}}
        rows, ok, _ = budget.check_tier("32gb", [big, small], None)
        self.assertEqual([r[0] for r in rows], ["small", "big"])
        self.assertFalse(ok)
        self.assertEqual(rows[1][3], "OVER BUDGET")
        rows, ok, _ = budget.check_tier("1tb", [big, small], None)
        self.assertTrue(ok)
        self.assertEqual([r[0] for r in rows], ["small"])

    def test_cli_exit_status(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "sources.json")
            with open(path, "w") as f:
                json.dump({"sources": [{"name": "a", "priority": 1, "tiers": ["32gb"],
                                        "estimate": {"nopic_bytes": 10**9, "words": 10**8}}]}, f)
            with open(os.devnull, "w") as null:
                with contextlib.redirect_stdout(null):
                    self.assertEqual(budget.main(["--sources", path]), 0)
                    self.assertEqual(budget.main([]), 0)


if __name__ == "__main__":
    unittest.main()
