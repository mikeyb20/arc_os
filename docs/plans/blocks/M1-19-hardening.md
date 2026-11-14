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
- [x] All suites pass with SMEP, SMAP and NX on.

**Watch out.**
- Interrupts that arrive between `STAC` and `CLAC` must not leak the AC flag; the entry stubs clear it.

**Notes.**
- From M1-03: do not enable CR4.FSGSBASE: the NMI/#DF/#GP/#MC entry (`isr_paranoid`) trusts that a user GS base
  is never a kernel address. The IST stacks (4 KiB each, `percpu.c`) and the AP's bootloader stack have no guard
  pages; SFMASK now clears TF, AC and NT on SYSCALL (AC matters once SMAP is on).
- **Built (M1-19).** `arch/x86_64/protect.c` turns on EFER.NXE, CR4.SMEP and CR4.SMAP per CPU (BSP:
  `hardening_protect_boot_cpu` right after `vmm_init`; APs: `ap_entry` after loading the kernel CR3). The linker
  script has four program headers (requests, text, rodata, data); `vmm_init` maps each part with its own
  permissions (`kimage_flags`), `vmm_map_page` always adds NX, and `vmm_kernel_wx_check` walks the image and the
  HHDM's first page at boot (a wrong mapping stops the boot) and in the self-test. NX joined the CPU floor list.
- **STAC/CLAC without alternatives.** Both are #UD on CPUs without SMAP, and the floor (Haswell) has none, so
  `smap.inc` tests `protect_smap_on` first (one load and a predictable branch) instead of patching code at boot.
  Users: `arch_copy_user` (STAC before, CLAC on both exits) and both entry stubs (CLAC after saving registers).
  A debug build panics if AC is still set at `isr_dispatch` (`check_ac_clear`); mmtest "ac" takes a page fault
  with AC set from user mode to prove it.
- **Fault reasons (isr.c `kuser_fault_kind`).** A kernel-mode fault at a user address: the copy routine with AC
  set resolves as before; the copy routine without AC under SMAP panics ("SMAP: user copy without STAC",
  instead of faulting forever); a present page touched elsewhere panics "SMAP: kernel touched user memory", a
  present page executed "SMEP: kernel ran user code" (the function is named from the return address on the
  stack); a missing page elsewhere stays an ordinary "Page Fault" panic (a NULL pointer). Each logs a
  `[SECURITY]` line naming the function.
- **Tests.** Spec item 5's "test-only syscall" became kernel test-mode triggers (`arc.panic=smap|smep`, suite
  kpanic) that map one user page into the kernel's own page tables: ADR-0001 forbids inventing a syscall
  number, and the CPU check is the same. Also `arc.panic=text|rodata|nx`. They run on `--cpu max` (TCG) so the
  result doesn't depend on the CI host. `qemu.mm.nosmap` runs mmtest with `-smap,-smep` (the Haswell path).
- **Open (not M1-19 scope).** The HHDM still maps the kernel image's physical pages read-write (NX): kernel
  code can be changed through that alias. Each mapping is W^X as the spec asks; closing the alias means splitting
  the HHDM's 2 MiB pages around the image (Linux does it). Kernel stack guard pages are M1-16's. Smoke boot on
  the laptop is owed after merge (work-blocks §3 rule 10): the boot log line `[SECURITY] NX on, SMEP on, SMAP on`
  and the report's `Protect` line prove it on hardware.
