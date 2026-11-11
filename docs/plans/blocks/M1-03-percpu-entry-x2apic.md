# M1-03 — Per-CPU entry paths and x2APIC

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M0-06 | M1-04, M1-05 |

**Goal.** Every CPU has its own kernel stack for SYSCALL and interrupts, found through GS, and the APIC layer
works in xAPIC or x2APIC mode with 32-bit APIC IDs and up to 256 CPUs.

**Read first.**
- `kernel/arch/x86_64/syscall_entry.asm`, `percpu.c`, `percpu.h`, `gdt.c` (TSS), `isr_stubs.asm`, `lapic.c`,
  `lapic.h`, `ipi.c`, `smp.c`, `msr.h`, `kernel/drivers/acpi.c` (MADT)

**Build.**
1. Per-CPU block: self pointer, CPU index, 32-bit APIC ID, current thread, kernel stack top, user RSP scratch,
   TSS pointer, core type (M1-01). `MAX_CPUS` goes from 16 to 256 (the laptop already has 16 CPUs; desktops have
   more); blocks are allocated at boot for the CPUs found.
2. SYSCALL entry: `swapgs`, save user RSP in the per-CPU scratch slot, switch to the per-CPU kernel stack;
   `swapgs` before SYSRET. Remove the global `syscall_kernel_rsp`.
3. Interrupt and exception stubs: `swapgs` when entering from user mode (CS RPL check) and when returning to it.
   NMI, #DF and #MC use per-CPU IST stacks and decide on `swapgs` by reading MSR_GS_BASE.
4. One TSS per CPU; RSP0 = the current thread's kernel stack top, updated on every context switch.
5. APIC access layer: xAPIC (MMIO) or x2APIC (MSRs), chosen at boot (x2APIC when supported, and always when the
   firmware already enabled it). IPIs go through the layer. Parse MADT types 0 and 9. Map APIC ID → CPU index.
6. Before SYSRET, check that the return RIP is canonical; if not, return through IRET (Intel SYSRET issue).
7. Tests: `-smp 4` boots in both modes (`-cpu max,+x2apic` and `-cpu max,-x2apic`); each CPU reports its index
   through GS; host test for MADT parsing with type 9 entries; all user suites still pass.

**Done when.**
- [ ] Suites pass with `-smp 4` in both APIC modes.
- [ ] No global syscall stack remains.
- [ ] Files this block rewrites outside `kernel/arch/` include `arch/hal.h` or arch-neutral headers, not
  `arch/x86_64/`, and call the HAL through `hal_*` functions, not x86 names (work-blocks.md §7); the
  `arch_includes_outside_arch` limit in the budgets file is lowered to match (ADR-0020).

**Cost.** `this_cpu()` is one GS-relative load; no lock on per-CPU data; syscall entry touches no shared cache line.

**Watch out.**
- `swapgs` must come before any GS-relative access on every path, including NMI.
- Application CPUs still don't run threads; M1-05 does that. Keep them in HLT.
- MSI to CPUs with APIC ID above 255 needs interrupt remapping, which is out of scope; M1-09 handles the limit.

**Notes.**
- From M1-01: `syscall_entry.asm` does `pop rsp` (user RSP) before `o64 sysret`, so for two instructions the
  kernel runs on the user stack. An NMI or #MC there would push onto user memory; per-CPU entry should give NMI
  and #MC an IST stack.
