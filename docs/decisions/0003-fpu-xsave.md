# ADR-0003: Eager XSAVE with CPUID-sized save area

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
The kernel currently never saves FPU/SSE/AVX state. llama.cpp depends on AVX2 (and benefits from AVX-VNNI on the
high-tier laptop). A fixed 512-byte FXSAVE area cannot hold AVX or AVX-512 state.

## Decision
- CPU floor is x86-64-v3 (AVX2, FMA, BMI2, XSAVE). Boot refuses to continue without them and prints why.
- Enable `CR4.OSFXSR`, `CR4.OSXMMEXCPT`, `CR4.OSXSAVE`; set XCR0 to every user-state component the CPU supports
  among x87, SSE, AVX, AVX-512 (opmask, ZMM_Hi256, Hi16_ZMM).
- Per-thread save area sized from CPUID leaf 0xD (EBX after XCR0 is set), 64-byte aligned, allocated with the thread.
- **Eager** switching: `XSAVEOPT`/`XSAVEC` (if supported) on switch-out, `XRSTOR` on switch-in. No lazy FPU (simpler,
  and lazy switching has known side-channel problems).
- Kernel code stays compiled without SSE/AVX (`-mgeneral-regs-only` style flags) so the kernel never clobbers user state.
- New threads start from a clean, initialized state (MXCSR = 0x1F80, FCW = 0x37F).
- Hybrid CPUs: core type (CPUID leaf 0x1A) is recorded per CPU for the scheduler.

## Consequences
- Same code path covers AVX2 machines and AVX-512 machines.
- Rules out: lazy FPU switching; SSE/AVX inside the kernel.

## Verification
- QEMU test: N threads each fill YMM registers with a unique pattern, yield in a loop, and check the pattern survives.
- Run with `-cpu max` and with a v3-level CPU model.
