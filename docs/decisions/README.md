# Architecture Decision Records

Each file records one decision that is expensive to change later. The master plan is
[`docs/plans/knowledge-os.md`](../plans/knowledge-os.md).

Rules:
- An **Accepted** ADR is binding. Code that contradicts it is a bug.
- To change one, write a new ADR that supersedes it, get Michael's explicit OK, and update the plan.
- Format: Status, Date, Context, Decision, Consequences (including what it rules out), Verification.

| ADR | Title | Status |
|---|---|---|
| [0001](0001-linux-syscall-abi.md) | Linux x86_64 syscall ABI | Accepted |
| [0002](0002-process-start-and-tls.md) | System V process start and TLS | Accepted |
| [0003](0003-fpu-xsave.md) | Eager XSAVE with CPUID-sized save area | Accepted |
| [0004](0004-page-cache-and-mmap.md) | Unified page cache, demand paging, mmap, COW | Accepted |
| [0005](0005-async-block-layer.md) | Async block layer with DMA | Accepted |
| [0006](0006-driver-model.md) | Driver model and MSI-X | Accepted |
| [0007](0007-unified-input-events.md) | Unified input events | Accepted |
| [0008](0008-disk-layout-and-filesystem.md) | GPT disk layout and ext2 | Accepted |
| [0009](0009-directory-layout.md) | Directory layout | Accepted |
| [0010](0010-arcpkg-format.md) | `.arcpkg` package format | Accepted |
| [0011](0011-data-contract.md) | Knowledge pack data contract | Accepted |
| [0012](0012-model-service-api.md) | Models only through `arc-llm` | Accepted |
| [0013](0013-utf8-everywhere.md) | UTF-8 everywhere | Accepted |
| [0014](0014-userspace-graphics-and-input.md) | Graphics and input from user space | Accepted |
| [0015](0015-library-without-llm.md) | Library works without the LLM | Accepted |
| [0016](0016-kernel-memory-sizing.md) | Per-page metadata from the PMM | Accepted |
| [0017](0017-power-management.md) | Power management is first-class | Accepted |
| [0018](0018-portable-usb-mode.md) | Portable mode: runs entirely from the USB key | Accepted |
