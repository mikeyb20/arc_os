# M1-22 — M1 done-test and second hardware boot

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | K + H | M1-04, M1-08, M1-15, M1-19, M1-20, M1-21 (so every M1 block) | M2 |

**Goal.** Prove M1's exit criteria: in QEMU, a 4-thread AVX2 program on 4 CPUs maps a 10 GB ext2 file with 4 GB of
RAM; on both real machines, arc_os boots to the shell.

**Read first.**
- `docs/plans/knowledge-os.md` §2 and §8 (M1), Notes of every M1 block (skim for open items)

**Build.**
1. `tests/target/m1_mmap_avx2.c` (musl, pthreads): map a 10 GiB file read-only; 4 threads pinned to 4 CPUs each sum
   their quarter with AVX2 and check it against the known pattern (data every 64 MiB, holes elsewhere); print the
   time and the page-fault and readahead counters.
2. Test disk builder: a sparse 12 GiB GPT image with an ext2 data partition holding the 10 GiB file
   (`truncate` + patterned writes + `mkfs.ext2 -d`).
3. ctest `qemu.m1_done` with label `slow` (`-smp 4 -m 4G`), run on demand, nightly in CI and at milestone gates.
4. Rebuild the USB image. **Michael:** boot both machines to the shell from the stick (RAM mode, ADR-0018; on the
   low-tier machine, typing with a USB keyboard waits for M4) and run the self-test; on the laptop, read idle
   watts from `/proc/arc/power`.
5. Tick the M1 boxes in knowledge-os.md §8 and record findings in its review history.
6. Hygiene pass (ADR-0020): read the `tools/hygiene.py` report, tighten every limit that has fallen, and list what
   is left (with the M2–M5 block expected to remove it) in Notes. Revisit GCC `-fanalyzer` and record the decision.

**Done when.**
- [ ] `arc.bench=1` results from both machines are in `docs/perf/` and compared with M1-04's (ADR-0019).
- [ ] `qemu.m1_done` passes in CI (or locally if the CI disk is too small; say so in Notes).
- [ ] Michael's photos or notes from both machines are in the PR.
- [ ] Hygiene limits equal the measured counts; the remaining debt is listed in Notes.

**Watch out.**
- Keep the image sparse; CI runners have about 14 GB of free disk.

**Notes.**
-
