# Performance results

One file per run on real hardware: `docs/perf/<date>-<machine>.md`, from the `arc.bench=1` table on the
bring-up stick ([bringup.md](../hardware/bringup.md)). Numbers are per operation, best of 3 runs, timed with the
TSC. CI never checks times (ADR-0019); these files are the record that later blocks compare against.

Later blocks add their hot path to the benchmark (page fault, read, mmap fault) and a row here.

## Template

```markdown
# <machine>, <date>

- Machine: <maker model> (from the boot report)
- CPU: <brand>, <cores> (P/E), TSC <kHz> from <source>
- RAM: <MB>, arc.mem=<cap or none>
- Commit: <short hash>, from the `bringup-latest` release notes or `git rev-parse --short HEAD` if built with `cmake --build build --target iso-bringup`

| Benchmark | Cycles | ns | Runs |
|---|---|---|---|
| page alloc+free 4 KiB | | | 10000 |
| page alloc+free 2 MiB | | | 1000 |
| kmalloc+kfree 64 B | | | 10000 |
| context switch | | | 10000 |

Notes: <anything unusual: power plugged in or not, a self-test FAIL, firmware settings changed>
```
