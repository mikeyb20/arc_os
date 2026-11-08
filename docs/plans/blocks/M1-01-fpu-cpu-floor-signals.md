# M1-01 — FPU state, CPU floor and Linux signal frames

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M0-06 | M1-04, M1-16 |

**Goal.** User programs can use SSE and AVX safely: the kernel never touches vector registers, every thread's
extended state is saved eagerly, signal frames use the Linux layout, and the CPU floor is checked at boot
(ADR-0003).

**Read first.**
- `docs/decisions/0003-fpu-xsave.md`, `0002-process-start-and-tls.md` (auxv)
- `toolchain-x86_64.cmake`, `kernel/arch/x86_64/context_switch.asm`, `kernel/proc/thread.c`, `thread.h`,
  `kernel/proc/signal.c`, `kernel/arch/x86_64/percpu.c`, `kernel/boot/kmain.c` (CPU init order)

**Already done by M0-06** (stock musl uses SSE before `main`; Michael's OK 2026-11-08): `-mgeneral-regs-only`
on the kernel, CR4.OSFXSR/OSXMMEXCPT, a 512-byte FXSAVE area per thread switched eagerly in
`arch_thread_switch()` (`kernel/arch/x86_64/fpu.c`), clean init image, fork copies the live state, execve
resets it; FS base per thread. This block replaces FXSAVE with XSAVE behind the same `fpu.h` calls. Also from
M0-06: the stack stays executable because the libarc trampoline lives on it; once `sa_restorer` is used, map
the stack NX unless `PT_GNU_STACK` asks for exec (`ElfLoadResult.stack_exec`). The signal frame must save the
argument registers (RDI–R10) too: today a handler return loses them.

**Build.**
1. Add `-mgeneral-regs-only` to the kernel flags (fixes a live bug: today GCC may use SSE registers in kernel
   code while nothing saves the user's). Fix whatever stops compiling.
2. `kernel/arch/x86_64/cpu.c`: CPUID leaves 1, 7, 0xD, 0x1A, 0x80000001, 0x80000007. Check x86-64-v3 (AVX, AVX2,
   FMA, BMI1, BMI2, F16C, LZCNT, MOVBE, XSAVE). If anything is missing, print which features on the framebuffer
   and serial, then halt.
3. Set CR4.OSFXSR, OSXMMEXCPT, OSXSAVE. XCR0 = x87 | SSE | AVX, plus opmask/ZMM state when AVX-512 exists;
   never AMX. Save-area size = CPUID(0xD, 0).EBX after XCR0 is set.
4. Per-thread XSAVE area, 64-byte aligned; new threads start from a clean init image (FCW 0x37F, MXCSR 0x1F80);
   eager XSAVE (XSAVEOPT if present) / XRSTOR in the context switch; fork copies the area.
5. Core type per CPU from CPUID 0x1A (0x40 = P-core, 0x20 = E-core) when CPUID 7 reports a hybrid part; stored in
   per-CPU data and shown in `/proc/cpuinfo`.
6. auxv: `AT_HWCAP` (CPUID 1 EDX), `AT_HWCAP2`, `AT_MINSIGSTKSZ` (frame size incl. XSAVE area).
7. Linux `rt_sigframe`: return address = `sa_restorer`, `ucontext` (`uc_flags`, `uc_link`, `uc_stack`,
   `uc_mcontext` with an `fpstate` pointer, `uc_sigmask`), `siginfo`, and the XSAVE image below it (64-byte
   aligned). `rt_sigreturn` restores registers, extended state and mask, and rejects a corrupt frame with SIGSEGV.
8. Tests (QEMU, `-cpu max`): two processes doing AVX2 math with distinct register contents under forced
   preemption check their registers; a signal handler that uses SSE/AVX leaves the interrupted code intact;
   musl `sigaction` + handler + return; `-cpu Nehalem` boot prints the floor message (marker) and halts.
9. ctest `kernel.no_simd`: `objdump -d kernel.elf` has no `xmm`/`ymm`/`zmm` registers outside the XSAVE helpers.

**Done when.**
- [ ] All tests above pass in TCG and in CI.
- [ ] `kernel.no_simd` passes.

**Watch out.**
- XSAVE faults on an area that isn't 64-byte aligned.
- At handler entry RSP + 8 must be 16-byte aligned (as if called).
- Struct copies in the kernel get slower without SSE; that is expected.

**Notes.**
-
