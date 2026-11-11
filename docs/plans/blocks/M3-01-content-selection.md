# M3-01 — Content selection, licences, size budget

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | C | M3-00a, M0-07 | M3-03 |

**Goal.** Choose the sources for each category and storage tier, record their licences, and set the byte budget
of the 32 GB and 1 TB tiers, as one list Michael approves. The pack pipeline (M3-03, M3-04) builds exactly that
list. Spec written by M3-00a (2026-11-11).

**Read first.**
- `docs/plans/knowledge-os.md` §3.7, §4 ("Storage tiers"), §6 and §10 (licence and public-repo risks)
- `docs/decisions/0011-data-contract.md`, `schemas/pack.schema.json`, `schemas/README.md`
- `docs/plans/content-lane.md` (conventions for M3-01, M3-03, M3-04)

**Build.**
1. **Budget.** A script (`tools/content/budget.py`, host only) computes the bytes left for packs on a 32 GB and a
   1 TB disk: the disk's decimal size, minus the ESP and system partitions (sizes are fixed in M5, so use the
   values in the script's header, marked *assumed*), minus the models of the tier (low-tier chat model and
   embedder from knowledge-os.md §4, also *assumed* until M2-06 measures them), minus a 10 % free-space reserve.
   It prints every term, so the numbers can be checked and rerun when an assumption changes.
2. **Candidates.** For each category in plan order (general, medical, food, repair/building; maps only as a
   budget line, since M3-10 and M4-13 define map content), list English Kiwix ZIMs from the Kiwix catalog: ZIM
   name, flavour (`maxi`, `nopic`, `mini`), date, size, article count, licence, and the source page. A category
   with no suitable ZIM is flagged, not filled from another format (the pipeline reads ZIMs only).
3. **Per candidate**, record: the licence and whether it allows sharing copies (flag NC and ND licences for
   Michael; don't decide for him); the image share (`maxi` size − `nopic` size); and the estimated installed pack
   size with the formula in the script: text ≈ the `nopic` size; chunks 32 bytes × (words / 280); FTS index
   estimated at 0.5 × text (M3-04 measures the real ratio); vectors chunks × 384 × 1 byte (M3-04b measures them).
4. **Pick** per tier, in priority order, until the budget is used; list what is left out and why.
5. **Write the list** as `sources.json`: one entry per pack with `name` (a schema id: it becomes the first part of
   every chunk ID, so no dates in it), `category`, `priority`, `zim` (file name), `url`, `flavour`, `date`,
   `license`, `source` (as `schemas/common.schema.json` defines them), `domain_tags`, `tiers` (`32gb`, `1tb`) and
   `sha256` (empty until M3-03 downloads the file). It goes in the private content repo (`arc_os` is public;
   knowledge-os.md §10), or in `/mnt/project-files/arc_os/content/` until that repo exists. A short summary
   (budget, picks per tier, flags) goes to Michael with a decision card to approve or change it.

**Done when.**
- [ ] Michael approved the list.
- [ ] `sources.json` is in the private repo (or project files) and `budget.py` reproduces the budget.

**Watch out.**
- `download.kiwix.org` is blocked from cloud sessions; catalog pages may be too. If they are, the laptop (Remote
  Control) or a GitHub Actions job reads the catalog. Never guess a size or licence: leave it blank and flag it.
- Pack version = the ZIM date as `YYYY.M.0` (e.g. `2026.11.0`), so a newer ZIM is a newer pack version.
- Domain tags are per pack in v1 (every chunk carries all of its pack's tags); per-chunk tagging is later.
- Whether a ZIM stores its images as WebP decides the image store (M3-08). The catalog rarely says; M3-03 counts
  image types when it opens the file.
- Content beyond the plan's categories needs Michael's OK.

**Notes.**
-
