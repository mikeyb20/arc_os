# M0-03 — Archive outside sources and test assets

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | I | M0-01 | M0-04, M2-01, M3, M5 |

**Goal.** Every outside component is pinned and stored in Michael's own GitHub, and any session or CI job can
fetch it without reaching a third-party server (knowledge-os.md §3.4).

**Read first.**
- `docs/plans/knowledge-os.md` §3.4, `tools/setup-deps.sh`, `tools/toolchain.lock` and `tools/dev-setup.sh`
  (from M0-01)

**Build.**
1. `third_party/SOURCES.lock` (JSON): name, version, upstream URL, license, SHA-256, archive URL. First set:
   binutils, GCC (+ gmp, mpfr, mpc), musl, musl-cross-make (commit), Limine (binary release and source),
   llama.cpp (tag), SQLite amalgamation (with FTS5), zstd, uACPI, e2fsprogs, stb_image, a PSF2-capable font
   source with a permissive license, the Linux `syscall_64.tbl` and the UAPI headers needed for layout checks
   (one pinned tag, reference only), the ext2 layout spec, and for tests only: a tiny permissively licensed GGUF
   chat model (about 100–150 MB) and a tiny GGUF embedder.
2. `.github/workflows/archive.yml` (manual): downloads every entry, checks SHA-256 (a missing hash is recorded on
   the first run and committed; a mismatch fails), uploads to a dated release (`archive-YYYYMMDD`) at the archive
   location. Files over 2 GiB are split.
3. `tools/fetch-archived.sh <name>...`: downloads from the archive, verifies SHA-256, unpacks into
   `third_party/src/<name>/` (gitignored).
4. `third_party/LICENSES.md`: one row per component (license, where it ships: build host only, on the install key,
   or inside arc_os).

**Done when.**
- [ ] The archive workflow is green and every lock entry has a hash and an archive URL.
- [ ] In a cloud session, `tools/fetch-archived.sh llama.cpp sqlite tiny-chat-model` works with no third-party host.

**Watch out.**
- Default archive location: release assets on arc_os (public), for openly licensed material only. Content
  manifests and anything private go in the private repo (Michael's decision, work-blocks.md §6).
- Never commit sources or models into git history; release assets only.
- Linux UAPI files are reference material for layout checks. Never `#include` them in arc_os code (GPL).

**Notes.**
-
