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
- [x] Suites pass with `-smp 4` in both APIC modes.
- [x] No global syscall stack remains.
- [x] Files this block rewrites outside `kernel/arch/` include `arch/hal.h` or arch-neutral headers, not
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
- 2026-11-11, done. Per-CPU block (`percpu.{c,h}`): self, kernel RSP, user RSP scratch and current thread on one
  cache line, then index, 32-bit APIC ID, TSS pointer, core type, own GDT + TSS. APs get one 16 KiB block from the
  page allocator (three 4 KiB IST stacks + the block); the BSP's is static because `thread_current()` now reads GS
  from the GDT stage on. APIC ID -> index is a sorted map (binary search).
- SWAPGS protocol: SYSCALL and every user-mode interrupt swap on entry and exit; NMI, #DF, #GP and #MC are
  "paranoid" (`isr_paranoid` reads MSR_GS_BASE), and NMI/#DF/#MC run on IST stacks. #GP is paranoid because the
  IRET path below can fault in ring 0 with the user's GS. Every return to user mode runs `cli` before SWAPGS.
- SYSRET only to a canonical RIP, else IRET (`syscall_iret_user`). The 15 `syscall_saved_user_*` globals are gone:
  fork reads the SyscallFrame at the top of the thread's kernel stack. SFMASK also clears TF, NT and AC (a user TF
  used to trap on the first kernel instruction).
- APIC layer in `lapic.c`: x2APIC when CPUID has it or firmware enabled it, else xAPIC; APs follow the BSP. Limine
  is asked for x2APIC (`LIMINE_SMP_X2APIC`) so it can start APIC IDs above 255. MADT types 0 and 9 (deduplicated,
  `ACPI_MAX_CPUS` 256). AP start goes through `boot_start_cpu` in `boot/limine.c`: smp.c no longer includes limine.h.
  APs now load the IDT (they never did) and their own GDT/TSS, SYSCALL MSRs and XSAVE, then HLT.
- QEMU 8.2 has no x2APIC under TCG (added in 9.0), so `qemu.smp.x2apic` needs KVM and skips in cloud sessions; CI
  (KVM, `-cpu host`) runs it and every other suite in x2APIC. TCG sessions run everything in xAPIC.
  `tools/qemu-test.py` now boots `-smp 4` by default and takes `--cpu-flags` / `ARC_QEMU_CPU_FLAGS`.
- Moved behind the HAL: kmain (APIC/SMP block now `hal_smp_init`, GDT/IDT `hal_cpu_init`), sched, init, process
  (`hal_set_kernel_stack`, `hal_switch_address_space`); `arch_includes_outside_arch` 43 -> 26. The ISA IRQ routing
  loop moved from kmain to `ioapic_route_isa`. `kernel_image_bytes` limit 212000 -> 233000 (BSP NMI/#MC stacks and
  256-CPU tables).
- Next: M1-04 (hardware boot) or M1-05 (APs run threads); notes for both are in their specs.
