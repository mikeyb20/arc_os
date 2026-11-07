# ADR-0009: Directory layout

- **Status:** Accepted
- **Date:** 2026-11-07

## Decision
| Path | Contents | Partition |
|---|---|---|
| `/system` | Kernel modules, core programs, libarc, fonts, default config. Read-only at runtime | system |
| `/apps` | Installed `.arcpkg` programs | data |
| `/models` | Installed model packages, `roles.json` | data |
| `/packs` | Installed knowledge packs | data |
| `/home` | User files, bookmarks, notes | data |
| `/var` | Logs, caches, package database | data |
| `/dev`, `/proc`, `/tmp` | devfs, procfs, ramfs | virtual |

## Consequences
- The OS can be reinstalled without touching data; packages always know where to install.
- Rules out: programs writing into `/system` at runtime.
