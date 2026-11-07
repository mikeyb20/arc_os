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
- Handler `sys_<name>()` in the subsystem that owns it (fs, proc, mm, net...), not in `syscall.c`.
- Register it under the Linux number in the syscall table; nothing else may use that slot.
- Validate every user pointer (`user_access.h`), copy in/out explicitly, never dereference user memory directly.
- Return `-errno` using Linux errno values. Unsupported flags → `-EINVAL`; unsupported feature → `-ENOSYS` or
  `-EOPNOTSUPP` exactly as Linux would.
- Structs crossing the boundary must match Linux layout byte for byte: add a `_Static_assert` on `sizeof` and key
  `offsetof` values.

## 3. User side
- libarc wrapper in `libc/src/` with the POSIX/Linux prototype; sets `errno` from the negative return.
- If the call replaces an old arc_os syscall, migrate every caller in `userland/` in the same PR and delete the old
  number.

## 4. Tests
- Host test in `tests/` for argument validation, struct layout asserts and edge cases.
- QEMU integration test when behaviour depends on the running kernel (processes, memory, files).
- Once musl is ported (M2): the same call made through musl must behave identically.

## 5. Bookkeeping
- Update the status row in `syscall-table.md` (done / stub) and the "Needed by" column.
- If behaviour deliberately differs from Linux, write it in the PR and in the table's notes. If the difference is
  permanent, it needs an ADR.

## Done when
Build passes, host tests pass, QEMU tests pass, table updated, no caller uses the old number.
