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
  permissions (`kimage_flags`), `vmm_map_page` always adds NX, and `vmm_kernel_wx_check` walks the image, the
  HHDM's first page and then the whole kernel half (PML4 256-511: nothing outside the code part may be
  executable, so a future mapper that skips NX is caught) at boot (a wrong mapping stops the boot) and in the
  self-test. NX joined the CPU floor list. `isr_stub_table` lives in `.rodata`.
- **STAC/CLAC without alternatives.** Both are #UD on CPUs without SMAP, and the floor (Haswell) has none, so
  `smap.inc` tests `protect_smap_on` first (one load and a predictable branch) instead of patching code at boot.
  Users: `arch_copy_user` (STAC before, CLAC on both exits) and both entry stubs (CLAC after saving registers).
  SFMASK clears AC on SYSCALL. Debug builds panic if AC is set in kernel C code (`protect_ac_leaked`, one home
  in protect.h): at `isr_dispatch` and at syscall entry and exit. mmtest "ac" proves each guard: a page fault
  with AC set (isr_common), a syscall with AC set (SFMASK) and a #GP with AC set (`cli` at CPL 3: isr_paranoid);
  a failed copy that left AC set is caught at syscall exit. Each of these four guards was deleted once and
  `qemu.mm` failed. `smap.inc` is an OBJECT_DEPENDS of the two stubs (CMake doesn't scan NASM includes).
- **Fault reasons (isr.c `kuser_fault_kind`).** A kernel-mode fault at a user address: the copy routine with AC
  set resolves as before; the copy routine without AC under SMAP panics ("SMAP: user copy without STAC",
  instead of faulting forever); a present page touched elsewhere panics "SMAP: kernel touched user memory" (without SMAP only a write to a
  read-only page faults: "WP: kernel wrote a read-only user page"), a present page executed "SMEP: kernel ran
  user code" ("NX: ..." without SMEP; the function is named from the return address on the stack); a missing page elsewhere stays an ordinary "Page Fault" panic (a NULL pointer). Each logs a
  `[SECURITY]` line naming the function.
- **Tests.** Spec item 5's "test-only syscall" became kernel test-mode triggers (`arc.panic=smap|smep`, suite
  kpanic) that map one user page into the kernel's own page tables: ADR-0001 forbids inventing a syscall
  number, and the CPU check is the same. Also `arc.panic=text|rodata|nx`. They run on `--cpu max` (TCG) so the
  result doesn't depend on the CI host. `qemu.mm.nosmap` runs mmtest on QEMU's Haswell model (the floor: SMEP
  on, SMAP off); `--cpu` forces TCG, because under KVM a CPUID-hidden SMAP leaves STAC/CLAC working in hardware
  and a missing `protect_smap_on` test wouldn't show. `qemu.mm` (default CPU) expects SMEP and SMAP on, so the
  ordinary suites are known to run with SMAP. A failed QEMU run keeps its serial log (`*.serial.log.fail-<pid>`).
- **D3 review (F1-F7) fixed** in the commit after `f47bce5`: the AC guards above, Haswell for nosmap, the
  kernel-half walk, honest fault reasons, the NXE comment (it can't come before the CR3 load: Limine sets it),
  one `RFLAGS_AC` define, the stub table read-only.
- **Open (not M1-19 scope).** The HHDM still maps the kernel image's physical pages read-write (NX): kernel
  code can be changed through that alias. Each mapping is W^X as the spec asks; closing the alias means splitting
  the HHDM's 2 MiB pages around the image (Linux does it). Kernel stack guard pages are M1-16's. Smoke boot on
  the laptop is owed after merge (work-blocks §3 rule 10): the boot log line `[SECURITY] NX on, SMEP on, SMAP on`
  and the report's `Protect` line prove it on hardware.
