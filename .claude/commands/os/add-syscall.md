# /os/add-syscall (retired)

This command used arc_os's own syscall numbers, which ADR-0001 retired: arc_os now follows the Linux x86_64
syscall ABI and never invents numbers.

Use the `add-syscall` skill instead: `.claude/skills/add-syscall/SKILL.md`. It covers the number lookup in the
archived `syscall_64.tbl`, the uapi struct, the handler in its subsystem, the libarc wrapper, tests and the
status table.
