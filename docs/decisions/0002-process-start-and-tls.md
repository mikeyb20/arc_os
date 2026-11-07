# ADR-0002: System V process start and TLS

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
musl, libstdc++ and threaded programs expect the standard x86_64 System V process start and thread-local
storage. Ad-hoc startup would break C++ exceptions, threads and `getauxval`.

## Decision
- `execve` builds the initial stack exactly as the System V x86_64 ABI specifies: `argc`, `argv[]`, NULL,
  `envp[]`, NULL, auxv pairs, AT_NULL; 16-byte aligned `rsp` at entry.
- auxv includes at least `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM` (16 random bytes),
  `AT_HWCAP`, `AT_HWCAP2`, `AT_UID/EUID/GID/EGID`, `AT_SECURE`, `AT_EXECFN`.
- TLS: per-thread FS base set by `arch_prctl(ARCH_SET_FS)` and by `clone(CLONE_SETTLS)`; saved/restored on context
  switch (or via `wrfsbase` when FSGSBASE is enabled).
- Executables: static ELF64, `ET_EXEC` or static-PIE (`ET_DYN` with no `PT_INTERP`). Programs with `PT_INTERP`
  are refused with `-ENOEXEC`.

## Consequences
- musl's `crt1` and `__init_tls` work unmodified; libarc's `crt0` is updated to read the same layout.
- Rules out: custom process start layouts; dynamic linking (until a future ADR).

## Verification
- QEMU test: a static musl program prints argv, envp, `getauxval(AT_PAGESZ)` and a `__thread` variable from two threads.
