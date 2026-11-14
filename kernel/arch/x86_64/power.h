#ifndef ARCHOS_ARCH_X86_64_POWER_H
#define ARCHOS_ARCH_X86_64_POWER_H

#include <stdint.h>

/* CPU power hardware (M1-21, ADR-0017): idle states (MWAIT or HLT), Intel
 * HWP with an energy preference (EPP), and RAPL energy counters.
 *
 * Idle: MWAIT when CPUID 1 ECX bit 3 says MONITOR/MWAIT, leaf 5 says
 * interrupts break MWAIT with interrupts masked and lists a C1 sub-state, and
 * the CPU is Intel; then C1 (hint 0x00) plus, on the models in a small table,
 * one deeper state whose hint leaf 5 lists. Without ARAT (CPUID 6 EAX bit 2)
 * the local APIC timer stops in C3 and deeper, so only C1 is used. Anything
 * else halts with HLT. arc.idle=hlt or arc.idle=c1 caps it (bring-up).
 *
 * HWP: when CPUID 6 EAX bit 7 is set, every CPU turns it on (IA32_PM_ENABLE,
 * can't be undone until reset) and writes its own IA32_HWP_REQUEST: min =
 * lowest, max = highest from its own IA32_HWP_CAPABILITIES (P- and E-cores
 * differ), desired 0 (the CPU chooses), and the EPP (bits 31:24) when EAX bit
 * 10 is set. A new EPP is applied by each CPU itself: power_set_epp bumps a
 * generation and sends every other CPU a reschedule IPI; a CPU compares
 * generations on that IPI and before every idle sleep (power_cpu_sync).
 *
 * RAPL: package, cores and platform (PSYS) 32-bit energy counters, extended
 * to 64 bits by sampling: at every read and at most once a second from an
 * idle CPU. A counter wraps after 2^32 units (about 40 minutes at 100 W with
 * the usual 61 uJ unit), so a wrap is lost only if every CPU stays busy and
 * nobody reads for that long.
 *
 * Locks: one spinlock (LOCK_LEVEL_POWER, IRQ-safe leaf) around the RAPL
 * samples; the idle path only tries it. Per-CPU state is written by its own
 * CPU with interrupts off.
 * Cost: an idle entry is two TSC reads, one generation compare and one
 * timestamp compare; MSRs are written only when the profile changes. */

/* Idle states: index 0 is the shallowest (C1 or HLT) */
#define POWER_IDLE_MAX 2
#define POWER_HINT_HLT 0xFFFFFFFFu   /* "hint" of the HLT state */

typedef struct {
    const char *name;        /* "HLT", "C1", "C6" */
    uint32_t hint;           /* MWAIT EAX hint, or POWER_HINT_HLT */
    uint32_t exit_us;        /* worst-case wake-up latency */
    uint32_t residency_us;   /* worth entering only for sleeps at least this long */
} PowerIdleState;

/* RAPL domains (bit i of PowerHwInfo.rapl_mask) */
#define POWER_RAPL_PKG     0
#define POWER_RAPL_CORES   1
#define POWER_RAPL_PSYS    2
#define POWER_RAPL_DOMAINS 3

/* What power_hw_init found (read-only after it) */
typedef struct {
    const char *idle_method;          /* "mwait" or "hlt" */
    const char *idle_why;             /* why not deeper, or "" */
    uint32_t idle_count;
    PowerIdleState idle[POWER_IDLE_MAX];
    uint8_t arat;                     /* LAPIC timer runs in deep C-states */
    uint8_t hwp;                      /* CPUID says HWP */
    uint8_t epp;                      /* CPUID says HWP EPP */
    uint32_t hwp_cpus;                /* CPUs with HWP on (power_cpu_sync) */
    uint32_t hwp_failed;              /* CPUs where an HWP MSR refused (#GP) */
    uint64_t hwp_caps0;               /* IA32_HWP_CAPABILITIES of CPU 0 */
    uint8_t rapl_mask;                /* POWER_RAPL_* present */
    uint8_t rapl_esu;                 /* energy unit: 1/2^esu J */
} PowerHwInfo;

/* Idle override from arc.idle (bring-up): none, HLT only, MWAIT C1 only */
#define POWER_IDLE_AUTO 0
#define POWER_IDLE_HLT  1
#define POWER_IDLE_C1   2

/* Probe idle states, HWP and RAPL on the BSP and set HWP up on every CPU
 * (IPI, bounded wait) with the given EPP. Boot, after every CPU's timer
 * queue exists. */
void power_hw_init(int idle_override, uint8_t epp);

/* What power_hw_init found. */
const PowerHwInfo *power_hw_info(void);

/* Sleep this CPU until an interrupt (or a write to wake's cache line) in
 * the deepest state worth it for sleep_ns (the time to the next timer;
 * UINT64_MAX: none). Called with interrupts off; returns with them on.
 * Also applies a pending EPP change and samples RAPL now and then. */
void power_idle_enter(uint64_t sleep_ns, const volatile void *wake);

/* Close this CPU's idle residency if it is still open (the interrupt entry
 * calls it: after HLT the handler runs, and may switch threads, before
 * power_idle_enter gets control back). */
void power_idle_close(void);

/* Per-CPU idle statistics: nanoseconds and entries per state index.
 * Returns 0, or -1 when the CPU is not online. */
int power_idle_stats(uint32_t cpu, uint64_t ns[POWER_IDLE_MAX], uint64_t entries[POWER_IDLE_MAX]);

/* Set the EPP every CPU requests (no-op without HWP); returns at once, the
 * other CPUs apply it on the reschedule IPI it sends them. */
void power_set_epp(uint8_t epp);

/* Apply a pending EPP change on this CPU (and turn HWP on here if it never
 * was). Interrupts off. */
void power_cpu_sync(void);

/* The EPP CPUs are asked for (the last power_set_epp). */
uint8_t power_get_epp(void);

/* Energy so far per RAPL domain in microjoules (0 for a missing one).
 * Returns rapl_mask. Samples the counters (wraparound). Any context. */
uint32_t power_energy_read(uint64_t uj[POWER_RAPL_DOMAINS]);

/* --- Pure helpers (host-tested) --- */

/* Index of the deepest state whose target residency fits in sleep_ns. */
uint32_t power_idle_select(const PowerIdleState *states, uint32_t count, uint64_t sleep_ns);

/* 1 if CPUID leaf 5 EDX (sub-states per MWAIT C-state) lists MWAIT hint. */
int power_mwait_hint_ok(uint32_t hint, uint32_t leaf5_edx);

/* IA32_HWP_REQUEST from the current value and the CPU's capabilities: min
 * = lowest, max = highest, desired = 0, EPP = epp (epp < 0: keep the old
 * field); activity window and package control kept. */
uint64_t power_hwp_request(uint64_t old, uint64_t caps, int epp);

/* Extend a 32-bit energy counter: add the units since *last (one wrap at
 * most) to total and remember now. */
uint64_t power_rapl_extend(uint64_t total, uint32_t *last, uint32_t now);

/* Energy units (1/2^esu J each) to microjoules, rounded down. */
uint64_t power_rapl_to_uj(uint64_t units, uint32_t esu);

#endif /* ARCHOS_ARCH_X86_64_POWER_H */
