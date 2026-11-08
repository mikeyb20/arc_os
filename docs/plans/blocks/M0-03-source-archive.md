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
- [x] The archive workflow is green and every lock entry has a hash and an archive URL.
- [x] In a cloud session, `tools/fetch-archived.sh llama.cpp sqlite tiny-chat-model` works with no third-party host.

**Watch out.**
- Default archive location: release assets on arc_os (public), for openly licensed material only. Content
  manifests and anything private go in the private repo (Michael's decision, work-blocks.md §6).
- Never commit sources or models into git history; release assets only.
- Linux UAPI files are reference material for layout checks. Never `#include` them in arc_os code (GPL).

**Notes.**
- 2026-11-08 (M0-03 PR): 21 components in `third_party/SOURCES.lock`, archived to the release
  `archive-20261108`. `tools/archive.py` does the work (`mirror` for the workflow, `fetch` for sessions,
  `check` offline); `tools/fetch-archived.sh` wraps `fetch`. Host test: `tests/tools/test_archive.py`
  (ctest `tools_archive`, file:// URLs only).
- The workflow runs on pushes to non-master branches that touch the lock file, and by hand. It commits the
  recorded hashes back to the branch (rebasing first); on master it only uploads the lock as an artifact.
  Pushes made with GITHUB_TOKEN don't start CI, so push one more commit after the bot's.
- Hashes pinned before the first run: binutils, GCC (from toolchain.lock), Limine source, zstd (matches
  upstream's .sha256), stb_image, Spleen and the 28 Linux UAPI files (downloaded in the session). The rest
  were recorded on first download (trust on first use over HTTPS); gmp and musl match known published values.
- Linux UAPI reference = 28 files from v6.12 packed into one reproducible tarball (fixed order, mtime 0).
- GitHub `archive/<ref>.tar.gz` tarballs (musl-cross-make, limine-binary, llama.cpp, uACPI) are not
  guaranteed byte-stable upstream; that only matters if an entry is re-archived. Fetches use our copy.
- Versions chosen: llama.cpp b12198, SQLite 3.54.1, uACPI 7.0.0, e2fsprogs 1.47.4, Spleen 2.2.0 (BSD-2,
  ships PSF2 `.psfu`), musl 1.2.5 (newest release the session could confirm), GCC prerequisites from
  gcc-14.1.0's `download_prerequisites`. Test models: SmolLM2-135M-Instruct Q8_0 (bartowski's GGUF repo)
  and nomic-embed-text-v1.5 Q8_0, both Apache-2.0.
- Not done here (follow-ups): `tools/build-cross-compiler.sh` and the toolchain workflow still download
  GNU sources from upstream; switching them to the archive belongs with M2-01's musl toolchain.
