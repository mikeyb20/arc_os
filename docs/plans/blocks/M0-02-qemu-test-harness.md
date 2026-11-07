# M0-02 — QEMU test harness

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | I | M0-01 | M0-05, all kernel blocks |

**Goal.** One command boots arc_os in QEMU, runs test programs and reports pass or fail through the exit code,
in a cloud session and in CI.

**Read first.**
- `tools/run.sh`, `tools/make-iso.sh`, `limine.conf`
- `kernel/boot/kmain.c` (init order), `kernel/proc/init.c`, `userland/init/`
- `CMakeLists.txt`, `tests/CMakeLists.txt`

**Build.**
1. Kernel test mode, enabled only by the kernel command line `arc.test=<suite>`: a panic exits QEMU at once with
   a failure code through `isa-debug-exit` (I/O port `0xf4`; QEMU exits with `(value << 1) | 1`). When PID 1
   exits in test mode, QEMU exits with PID 1's status (0 = pass). This is kernel behaviour, not a new syscall.
2. Test init: in test mode, init runs the programs listed for the suite and prints one line each:
   `ARC_TEST PASS <name>` or `ARC_TEST FAIL <name> <reason>`, then `ARC_TEST DONE pass=N fail=M`, and exits
   with 0 only if all passed.
3. `make-iso.sh`: take an output name and an extra module list, so test ISOs carry test programs and suite lists
   as Limine modules (as user programs are loaded today).
4. `tools/qemu-test.py` (Python 3, standard library only): builds the QEMU command line (`-machine q35`, `-smp`,
   `-m`, `-cpu max`, or `-accel kvm -cpu host` when `/dev/kvm` is writable, `-display none`, serial to a file,
   `-device isa-debug-exit,iobase=0xf4,iosize=0x04`, `-no-reboot`, `-accel tcg,thread=multi` when not using KVM),
   enforces a timeout, parses the markers, prints a short summary and, on failure, the last 40 serial lines.
5. ctest: `qemu.smoke` (boot to the shell prompt marker, run `hello`) and `qemu.userland` (the existing userland
   programs as tests), label `qemu`. Reserve label `slow` for long tests.
6. CI job `qemu` after the build job: `ctest -L qemu`; upload serial logs on failure.

**Done when.**
- [ ] `ctest -L qemu` passes in a cloud session (TCG) in under 3 minutes, and in CI.
- [ ] A deliberately failing test program fails ctest with its name and reason.
- [ ] A kernel panic in test mode fails within seconds, not at the timeout.

**Watch out.**
- TCG is 10–50× slower than KVM. Use `-m 256M` for smoke tests; only tests that need it get `-m 4G`.
- Read serial logs by marker or tail, never whole (they are large and cost context).
- Check whether CI runners expose `/dev/kvm`; record it in Notes.

**Notes.**
-
