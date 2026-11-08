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
3. `make-iso.sh`: take an output name (`-o`), a kernel command line (`-c`, needed for `arc.test=`) and an extra
   module list, so test ISOs carry test programs and suite lists as Limine modules (as user programs are loaded
   today).
4. `tools/qemu-test.py` (Python 3, standard library only): builds the QEMU command line (`-machine q35`, `-smp`,
   `-m`, `-cpu max`, or `-accel kvm -cpu host` when `/dev/kvm` is writable, `-display none`, serial to a file,
   `-device isa-debug-exit,iobase=0xf4,iosize=0x04`, `-no-reboot`, `-accel tcg,thread=multi` when not using KVM),
   enforces a timeout, parses the markers, prints a short summary and, on failure, the last 40 serial lines.
5. ctest: `qemu.smoke` (boot to the shell prompt marker, run `hello`) and `qemu.userland` (the existing userland
   programs as tests), label `qemu`. Reserve label `slow` for long tests. Login needs a TTY, so smoke runs the
   shell from test init with stdin from a file and checks the prompt and `hello` output in the serial log.
6. CI job `qemu` after the build job: `ctest -L qemu`; upload serial logs on failure.

**Done when.**
- [x] `ctest -L qemu` passes in a cloud session (TCG) in under 3 minutes, and in CI.
- [x] A deliberately failing test program fails ctest with its name and reason.
- [x] A kernel panic in test mode fails within seconds, not at the timeout.

**Watch out.**
- TCG is 10–50× slower than KVM. Use `-m 256M` for smoke tests; only tests that need it get `-m 4G`.
- Read serial logs by marker or tail, never whole (they are large and cost context).
- Check whether CI runners expose `/dev/kvm`; record it in Notes.

**Notes.**
- Done 2026-11-08. `ctest -L qemu` = 4 tests, ~26 s in a cloud session (TCG, no `/dev/kvm`): smoke 5.5 s,
  userland 10.5 s (18 programs), `qemu.selftest.fail` (expects `FAIL testfail exit=3`), `qemu.selftest.panic`
  (suite `kpanic`, the kernel panics on purpose; fails in ~3 s). One run: `tools/qemu-test.py --suite <name>`;
  logs in `build/qemu-test/<suite>.serial.log`.
- Interfaces: `arc.test=<suite>` on the kernel command line (BootInfo now carries `cmdline`; Limine kernel-file
  request); `/proc/cmdline`; init reads it and runs `/boot/<suite>.suite` (`name path args [< stdin-file]`).
  Exit codes: init status S → QEMU `(S<<1)|1`; panic → 255. Suites live in `tests/qemu/suites/`; test-only
  programs in `userland/testprogs/` (only on test ISOs). Normal boot is unchanged.
- Master never reached init in QEMU. The harness found six latent bugs, fixed here because every test needs
  them: (1) q35 memory map has >64 entries, BootInfo cut it and the framebuffer fell outside the HHDM → raised
  to 256; (2) the identity IOAPIC route for IRQ2 overwrote the PIT's override route (IRQ0→GSI2), so the timer
  hit vector 34 and nothing was scheduled; (3) `sys_wait` read (status, flags) but libc passes
  (pid, status, options) → status written to address `pid`; pid is still ignored (M0-05 makes it `wait4`);
  (4) `sched_yield` held `sched_lock` across `context_switch`, and a thread resumed via the timer never released
  it → deadlock on the next yield; now interrupts-off only, real locking is M1-05; (5) 16 KiB user stack too
  small for `ls` → 64 KiB; (6) STAR[63:48] lacked RPL 3: AMD's SYSRET loads SS = STAR+8 without OR-ing 3, so
  user code ran with SS = 0x18 and the first IRETQ to user mode raised #GP(0x18). Only seen under KVM on CI's
  AMD runners (TCG and Intel OR in the RPL).
- Follow-ups (not done): a user page fault halts the whole kernel instead of killing the process (M1-01 signals
  / M1-07 faults); `user_ptr_valid()` accepted address 0x2; procfs `/proc/<pid>` inodes (2000+pid) collide with
  meminfo/uptime (2001/2002); the PIT prints `[TIMER] N seconds` every second.
- CI: `qemu` job downloads kernel + programs from the build job, enables `/dev/kvm` with a udev rule when present
  (GitHub's ubuntu-24.04 runners normally have it); `qemu-test.py` prints `kvm` or `tcg` per suite. Check the
  first CI run's log for which one was used.
