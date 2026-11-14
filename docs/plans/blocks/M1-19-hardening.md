# M1-19 — Hardening: SMEP, SMAP, NX and W^X

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M1-07 | M1-22 |

**Goal.** The kernel can neither run user code nor touch user memory outside the copy helpers, and no kernel
memory is both writable and executable (knowledge-os.md §3.2).

**Read first.**
- `kernel/security/hardening.c`, `hardening.h`, `kernel/arch/x86_64/linker.ld`, the boot page-table setup in
  `kernel/mm/vmm.c`, the user-copy helpers from M1-07

**Build.**
1. EFER.NXE. Kernel mappings: `.text` read + execute, `.rodata` read-only, `.data`/`.bss` read-write + NX, the
   HHDM read-write + NX.
2. User mappings follow ELF segment flags and `PROT_EXEC`. The stack is NX unless `PT_GNU_STACK` asks for
   execute (follow Linux). The stack part is done (M1-01; since M1-07 the stack is a VMA, `mm_setup_stack(mm, exec, brk_start)`,
   and every user page gets its flags from its VMA's protection, `prot_to_vmm` in mm.c).
3. CR4.SMEP and CR4.SMAP when the CPU has them. `STAC`/`CLAC` only inside the user-copy helpers (M1-07: one routine, `arch_copy_user` in
   `arch/x86_64/uaccess.asm`, with the exception-table entry). Any other kernel
   access to user memory panics with a message naming the function.
4. W^X: kernel mappings are never writable and executable at once. User space follows Linux (W+X allowed), since
   nothing in the plan needs to forbid it; revisit if that changes.
5. Tests: a write to kernel `.text` faults (test-mode trigger); jumping to the user stack gives SIGSEGV; a
   deliberate direct user access in a test-only syscall is caught by SMAP; all suites pass with SMEP/SMAP on
   (`-cpu max` has both).

**Done when.**
- [ ] All suites pass with SMEP, SMAP and NX on.

**Watch out.**
- Interrupts that arrive between `STAC` and `CLAC` must not leak the AC flag; the entry stubs clear it.

**Notes.**
- From M1-03: do not enable CR4.FSGSBASE: the NMI/#DF/#GP/#MC entry (`isr_paranoid`) trusts that a user GS base
  is never a kernel address. The IST stacks (4 KiB each, `percpu.c`) and the AP's bootloader stack have no guard
  pages; SFMASK now clears TF, AC and NT on SYSCALL (AC matters once SMAP is on).
