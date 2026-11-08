---
name: add-syscall
description: Add or migrate one arc_os syscall to the Linux x86_64 ABI (number, semantics, structs, errno, libarc wrapper, tests, status table). Use for any syscall work.
---

# add-syscall

arc_os implements the Linux x86_64 syscall ABI as a copied convention (ADR-0001). Every syscall change follows
these steps, in this order, in one PR.

## 1. Look it up, don't guess
- Number: archived Linux `arch/x86/entry/syscalls/syscall_64.tbl` (pinned version from the M0 inventory).
- Semantics, flags, errno list: the man page (section 2) and, for struct layouts, the archived Linux UAPI headers
  (`include/uapi/...`, `arch/x86/include/uapi/...`). Record the exact source in the PR description.
- Check `.claude/skills/arc-os-architecture/reference/syscall-table.md` for current status and who needs it.

## 2. Kernel side
- Add `#define __NR_<name> <n>` to `kernel/include/uapi/unistd.h` (copied from the tbl) and a row for it in
  `.claude/skills/arc-os-architecture/reference/syscall-table.md`.
- Handler `static int64_t sys_<name>()` in the subsystem file that owns it: `kernel/fs/fs_syscalls.c`,
  `kernel/proc/proc_syscalls.c`, `kernel/mm/mm_syscalls.c` or `kernel/net/net_syscalls.c` (since M0-05;
  `arch/x86_64/syscall.c` holds only dispatch and registration). Register it in that file's
  `<subsystem>_syscalls_init()` with `syscall_register(__NR_<name>, sys_<name>)`; nothing else may use that slot.
- Constants and struct definitions go in `kernel/include/uapi/` (shared with libarc as `<uapi/...>`), with
  `_Static_assert`s, and get a line in `tests/abi_layout_list.h` (also add the `__NR_` there). `test_abi_layout`
  compares every line with the host's Linux headers.
- Validate every user pointer (`user_access.h`), copy in/out explicitly, never dereference user memory directly.
  New code uses the fault-free helpers `copy_from_user`, `copy_to_user` and `strncpy_from_user` (M0-06): they
  check the pages are mapped for user access, since the kernel has no page-fault fixups yet (M1-19).
- Return `-errno` using Linux errno values. Unsupported flags → `-EINVAL`; unsupported feature → `-ENOSYS` or
  `-EOPNOTSUPP` exactly as Linux would.
- Structs crossing the boundary must match Linux layout byte for byte: add a `_Static_assert` on `sizeof` and key
  `offsetof` values.

## 3. User side
- libarc wrapper in `libc/src/` with the POSIX/Linux prototype, using the private raw helpers in
  `libc/src/syscall_arch.h` (`syscallN(__NR_<name>, ...)`, then `syscall_ret()` to set `errno`). User programs
  never make raw syscalls; only `userland/testprogs/abi_check.c` does, to check the raw `-errno`.
- User-mode SSE is on since M0-06 (FXSAVE per thread; M1-01 moves to XSAVE/AVX). The kernel is built with
  `-mgeneral-regs-only` and must never touch x87/SSE/AVX registers: they hold the running thread's user state.
- If the call replaces an old arc_os syscall, migrate every caller in `userland/` in the same PR and delete the old
  number.

## 4. Tests
- Host test in `tests/` for argument validation, struct layout asserts and edge cases.
- QEMU integration test when behaviour depends on the running kernel (processes, memory, files): add checks to
  `userland/testprogs/abi_check.c` (ctest `qemu.abi`).
- Once musl is ported (M2): the same call made through musl must behave identically.

## 5. Bookkeeping
- Update the status row in `syscall-table.md` (done / stub) and the "Needed by" column.
- If behaviour deliberately differs from Linux, write it in the PR and in the table's notes. If the difference is
  permanent, it needs an ADR.

## Done when
Build passes, host tests pass, QEMU tests pass, table updated, no caller uses the old number.
