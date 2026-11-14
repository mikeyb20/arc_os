/* arc_os — CPU power hardware: idle states, HWP, RAPL (power.h; M1-21)
 *
 * Owns: the idle-state table and the MWAIT/HLT entry with per-CPU residency,
 * HWP enable and IA32_HWP_REQUEST on every CPU, RAPL energy counters.
 * Invariants: an idle sleep starts with interrupts off (no wake-up is lost
 * between the scheduler's check and the sleep: MWAIT is told to treat a
 * masked interrupt as a break event, HLT goes through STI's shadow); every
 * MSR CPUID doesn't promise goes through rdmsr_safe/wrmsr_safe.
 * Locks: rapl_lock (LOCK_LEVEL_POWER, IRQ-safe leaf), see power.h.
 * Cost: see power.h. */

#include "arch/x86_64/power.h"
#include "arch/x86_64/msr.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/cpuid.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/ipi.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/tsc.h"
#include "arch/hal_lock.h"
#include "proc/spinlock.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include <stddef.h>

_Static_assert(POWER_IDLE_MAX == PERCPU_IDLE_STATES, "per-CPU idle counters match the state table");

/* CPUID leaf 5 ECX: MWAIT extensions enumerated, interrupts break MWAIT even when masked */
#define MWAIT_ECX_EMX    (1u << 0)
#define MWAIT_ECX_IBE    (1u << 1)
/* CPUID 1 ECX bit 3: MONITOR/MWAIT; CPUID 6 EAX: ARAT, HWP, EPP */
#define CPUID1_ECX_MWAIT (1u << 3)
#define CPUID6_EAX_ARAT  (1u << 2)
#define CPUID6_EAX_HWP   (1u << 7)
#define CPUID6_EAX_EPP   (1u << 10)

#define HWP_CALL_CYCLES (1ULL << 32)   /* 1-4 s for every CPU to answer at boot */

static const PowerIdleState state_hlt = {"HLT", POWER_HINT_HLT, 1, 0};
static const PowerIdleState state_c1 = {"C1", 0x00, 2, 2};

/* The one deeper state per model: the two test machines' CPUs (family 6).
 * Conservative (C6, not the package's deepest) until M1-22 measures them.
 * 0x8E/0x9E: Kaby Lake and Coffee Lake (7th/8th gen), Linux intel_idle's
 * skl_cstates C6. 0xC5: Arrow Lake H (Core Ultra 200H); Linux has no table
 * for it, the values are its Meteor Lake (0xAA) C6. */
typedef struct {
    uint32_t model;
    PowerIdleState deep;
} IdleModel;

static const IdleModel idle_models[] = {
    {0x8E, {"C6", 0x20, 85, 200}},
    {0x9E, {"C6", 0x20, 85, 200}},
    {0xC5, {"C6", 0x20, 140, 420}},
};

static PowerHwInfo hw = {
    .idle_method = "hlt", .idle_why = "", .idle_count = 1, .idle = {{"HLT", POWER_HINT_HLT, 1, 0}}};

/* HWP: the EPP every CPU should request and its generation (power_cpu_sync) */
static uint8_t want_epp;
static uint32_t epp_gen;

/* RAPL */
static Spinlock rapl_lock = SPINLOCK_INIT(LOCK_LEVEL_POWER, "rapl");
static const uint32_t rapl_msr[POWER_RAPL_DOMAINS] = {MSR_PKG_ENERGY_STATUS, MSR_PP0_ENERGY_STATUS,
                                                      MSR_PLATFORM_ENERGY_STATUS};
static uint32_t rapl_last[POWER_RAPL_DOMAINS];
static uint64_t rapl_units[POWER_RAPL_DOMAINS];
static uint64_t rapl_next_tsc;     /* next idle-time sample at or after this TSC */
static uint64_t rapl_period;       /* cycles between idle-time samples (1 s) */

/* --- Pure helpers --- */

uint32_t power_idle_select(const PowerIdleState *states, uint32_t count, uint64_t sleep_ns) {
    uint32_t pick = 0;
    for (uint32_t i = 1; i < count; i++) {
        if ((uint64_t)states[i].residency_us * 1000 <= sleep_ns) pick = i;
    }
    return pick;
}

int power_mwait_hint_ok(uint32_t hint, uint32_t leaf5_edx) {
    uint32_t cstate = ((hint >> 4) & 0xF) + 1;   /* EDX nibble n: sub-states of MWAIT C-state n */
    uint32_t sub = hint & 0xF;
    if (cstate > 7) return 0;
    return sub < ((leaf5_edx >> (cstate * 4)) & 0xF);
}

uint64_t power_hwp_request(uint64_t old, uint64_t caps, int epp) {
    uint64_t highest = caps & 0xFF, lowest = (caps >> 24) & 0xFF;
    uint64_t keep = old & ~0xFFFFFFFFULL;             /* activity window, package control, reserved */
    uint64_t field_epp = epp < 0 ? (old >> 24) & 0xFF : (uint64_t)(epp & 0xFF);
    return keep | (field_epp << 24) | (0ULL << 16) | (highest << 8) | lowest;
}

uint64_t power_rapl_extend(uint64_t total, uint32_t *last, uint32_t now) {
    total += (uint32_t)(now - *last);   /* unsigned: one wrap counts right */
    *last = now;
    return total;
}

uint64_t power_rapl_to_uj(uint64_t units, uint32_t esu) {
    return (uint64_t)(((unsigned __int128)units * 1000000u) >> esu);
}

/* --- Idle --- */

static int is_intel(void) {
    return strcmp(cpu_features()->vendor, "GenuineIntel") == 0;
}

static void probe_idle(int override) {
    const CpuFeatures *f = cpu_features();
    hw.arat = (f->l6_eax & CPUID6_EAX_ARAT) != 0;
    hw.idle[0] = state_hlt;
    hw.idle_count = 1;
    hw.idle_method = "hlt";
    if (override == POWER_IDLE_HLT) {
        hw.idle_why = "arc.idle=hlt";
        return;
    }
    if (!(f->l1_ecx & CPUID1_ECX_MWAIT) || f->max_leaf < 5 || !is_intel()) {
        hw.idle_why = "no MWAIT";
        return;
    }
    uint32_t a, b, c, d;
    cpuid(5, 0, &a, &b, &c, &d);
    if ((c & (MWAIT_ECX_EMX | MWAIT_ECX_IBE)) != (MWAIT_ECX_EMX | MWAIT_ECX_IBE) ||
        !power_mwait_hint_ok(state_c1.hint, d)) {
        hw.idle_why = "MWAIT lacks IRQ break or C1";
        return;
    }
    hw.idle[0] = state_c1;
    hw.idle_method = "mwait";
    hw.idle_why = "";
    if (override == POWER_IDLE_C1) {
        hw.idle_why = "arc.idle=c1";
        return;
    }
    uint32_t family, model, stepping;
    cpu_signature(&family, &model, &stepping);
    const IdleModel *m = NULL;
    for (uint32_t i = 0; family == 6 && i < sizeof(idle_models) / sizeof(idle_models[0]); i++) {
        if (idle_models[i].model == model) m = &idle_models[i];
    }
    if (m == NULL) {
        hw.idle_why = "model not in table";
    } else if (!hw.arat && m->deep.hint >= 0x10) {
        hw.idle_why = "no ARAT";
    } else if (!power_mwait_hint_ok(m->deep.hint, d)) {
        hw.idle_why = "leaf 5 lacks deep state";
    } else {
        hw.idle[1] = m->deep;
        hw.idle_count = 2;
    }
}

/* The idle fields of a CPU change only on that CPU with interrupts off,
 * inside idle_seq going odd and back even; power_idle_stats on another CPU
 * retries until it reads them between two changes (a seqcount), so an open
 * sleep is counted exactly once: in idle_tsc or in idle_cycles. */
static void idle_write_begin(PerCpu *c) {
    __atomic_store_n(&c->idle_seq, c->idle_seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static void idle_write_end(PerCpu *c) {
    __atomic_store_n(&c->idle_seq, c->idle_seq + 1, __ATOMIC_RELEASE);
}

/* Account the open residency of CPU c (interrupts off, on c) */
static void idle_close(PerCpu *c, uint64_t now) {
    uint64_t start = c->idle_tsc;
    if (start == 0) return;
    uint32_t s = c->idle_state;
    idle_write_begin(c);
    __atomic_store_n(&c->idle_cycles[s], c->idle_cycles[s] + (now - start), __ATOMIC_RELAXED);
    __atomic_store_n(&c->idle_tsc, 0, __ATOMIC_RELAXED);
    idle_write_end(c);
}

void power_idle_close(void) {
    PerCpu *c = this_cpu();
    if (c->idle_tsc != 0) idle_close(c, cpu_rdtsc());
}

static void rapl_sample_locked(void) {
    for (uint32_t d = 0; d < POWER_RAPL_DOMAINS; d++) {
        uint64_t v;
        if (!(hw.rapl_mask & (1u << d)) || rdmsr_safe(rapl_msr[d], &v) != 0) continue;
        rapl_units[d] = power_rapl_extend(rapl_units[d], &rapl_last[d], (uint32_t)v);
    }
}

/* From an idle CPU at most once a period: keeps the extension right while
 * nobody reads. Never waits for the lock. */
static void rapl_idle_sample(uint64_t now) {
    if (hw.rapl_mask == 0 || now < __atomic_load_n(&rapl_next_tsc, __ATOMIC_RELAXED)) return;
    if (!spin_trylock(&rapl_lock)) return;
    __atomic_store_n(&rapl_next_tsc, now + rapl_period, __ATOMIC_RELAXED);
    rapl_sample_locked();
    spin_unlock(&rapl_lock);
}

void power_idle_enter(uint64_t sleep_ns, const volatile void *wake) {
    power_cpu_sync();
    PerCpu *c = this_cpu();
    uint32_t s = power_idle_select(hw.idle, hw.idle_count, sleep_ns);
    uint32_t hint = hw.idle[s].hint;
    uint64_t now = cpu_rdtsc();
    rapl_idle_sample(now);
    idle_write_begin(c);
    __atomic_store_n(&c->idle_state, s, __ATOMIC_RELAXED);
    __atomic_store_n(&c->idle_entries[s], c->idle_entries[s] + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&c->idle_tsc, cpu_rdtsc(), __ATOMIC_RELAXED);
    idle_write_end(c);
    if (hint == POWER_HINT_HLT) {
        /* The handler runs before HLT "returns" and may switch threads: it
         * closes the residency itself (power_idle_close at interrupt entry) */
        cpu_sti_hlt();
        /* Woken by an NMI (no interrupt entry ran): close it here, with
         * interrupts off so an interrupt can't close it at the same time */
        uint64_t flags = irq_save();
        power_idle_close();
        irq_restore(flags);
        return;
    }
    /* Interrupts stay masked: one that arrives (or is already pending) ends
     * MWAIT (ECX bit 0), the residency is closed, then it is taken */
    if (wake == NULL) wake = &c->idle_state;   /* MONITOR needs a mapped line */
    cpu_monitor_mwait(wake, hint);
    idle_close(c, cpu_rdtsc());
    cpu_sti();
}

int power_idle_stats(uint32_t cpu, uint64_t ns[POWER_IDLE_MAX], uint64_t entries[POWER_IDLE_MAX]) {
    PerCpu *c = percpu_get(cpu);
    if (c == NULL || !c->online) return -1;
    uint64_t cycles[POWER_IDLE_MAX], open_tsc, now;
    uint32_t seq, open_state;
    do {
        while ((seq = __atomic_load_n(&c->idle_seq, __ATOMIC_ACQUIRE)) & 1) hal_cpu_relax();
        for (uint32_t s = 0; s < POWER_IDLE_MAX; s++) {
            cycles[s] = __atomic_load_n(&c->idle_cycles[s], __ATOMIC_RELAXED);
            entries[s] = __atomic_load_n(&c->idle_entries[s], __ATOMIC_RELAXED);
        }
        open_tsc = __atomic_load_n(&c->idle_tsc, __ATOMIC_RELAXED);
        open_state = __atomic_load_n(&c->idle_state, __ATOMIC_RELAXED);
        now = cpu_rdtsc();
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while (__atomic_load_n(&c->idle_seq, __ATOMIC_RELAXED) != seq);
    /* A CPU asleep now (tickless: maybe for seconds) counts up to now */
    if (open_tsc != 0 && now > open_tsc && open_state < POWER_IDLE_MAX) cycles[open_state] += now - open_tsc;
    for (uint32_t s = 0; s < POWER_IDLE_MAX; s++) ns[s] = tsc_cycles_to_ns(cycles[s]);
    return 0;
}

/* --- HWP --- */

/* Turn HWP on here and take this CPU's limits (interrupts off, on c) */
static void hwp_cpu_enable(PerCpu *c) {
    uint64_t en, caps, req;
    int ok = rdmsr_safe(MSR_PM_ENABLE, &en) == 0;
    if (ok && !(en & 1)) ok = wrmsr_safe(MSR_PM_ENABLE, en | 1) == 0;
    ok = ok && rdmsr_safe(MSR_HWP_CAPABILITIES, &caps) == 0 && rdmsr_safe(MSR_HWP_REQUEST, &req) == 0;
    if (!ok) {
        c->hwp_state = PERCPU_HWP_FAILED;
        __atomic_add_fetch(&hw.hwp_failed, 1, __ATOMIC_RELAXED);
        return;
    }
    if (c->cpu_index == 0) hw.hwp_caps0 = caps;
    c->hwp_req = power_hwp_request(req, caps, -1);
    c->hwp_state = PERCPU_HWP_ON;
    __atomic_add_fetch(&hw.hwp_cpus, 1, __ATOMIC_RELAXED);
}

void power_cpu_sync(void) {
    if (!hw.hwp) return;
    PerCpu *c = this_cpu();
    uint32_t gen = __atomic_load_n(&epp_gen, __ATOMIC_ACQUIRE);
    if (c->power_gen == gen) return;
    if (c->hwp_state == PERCPU_HWP_UNTRIED) hwp_cpu_enable(c);
    if (c->hwp_state == PERCPU_HWP_ON) {
        uint64_t req = c->hwp_req;   /* min, max, desired set at enable */
        if (hw.epp) req = (req & ~(0xFFULL << 24)) | ((uint64_t)__atomic_load_n(&want_epp, __ATOMIC_RELAXED) << 24);
        if (wrmsr_safe(MSR_HWP_REQUEST, req) == 0) {
            c->hwp_req = req;
        } else {
            c->hwp_state = PERCPU_HWP_FAILED;
            __atomic_sub_fetch(&hw.hwp_cpus, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&hw.hwp_failed, 1, __ATOMIC_RELAXED);
        }
    }
    c->power_gen = gen;
}

static void hwp_sync_fn(void *arg) {
    (void)arg;
    power_cpu_sync();
}

void power_set_epp(uint8_t epp) {
    __atomic_store_n(&want_epp, epp, __ATOMIC_RELAXED);
    __atomic_add_fetch(&epp_gen, 1, __ATOMIC_RELEASE);
    if (!hw.hwp) return;
    uint64_t flags = irq_save();
    power_cpu_sync();
    irq_restore(flags);
    /* Every other CPU applies it on this IPI (ipi.c) or before it next sleeps */
    if (cpu_count > 1) lapic_send_ipi_all_excluding_self(IPI_VEC_SCHEDULE);
}

uint8_t power_get_epp(void) {
    return __atomic_load_n(&want_epp, __ATOMIC_RELAXED);
}

/* --- RAPL --- */

static void probe_rapl(void) {
    uint64_t unit;
    hw.rapl_mask = 0;
    if (!is_intel() || rdmsr_safe(MSR_RAPL_POWER_UNIT, &unit) != 0 || unit == 0) return;
    hw.rapl_esu = (uint8_t)((unit >> 8) & 0x1F);
    for (uint32_t d = 0; d < POWER_RAPL_DOMAINS; d++) {
        uint64_t v;
        /* A domain the platform doesn't have reads as 0 (or faults) */
        if (rdmsr_safe(rapl_msr[d], &v) == 0 && (uint32_t)v != 0) {
            rapl_last[d] = (uint32_t)v;
            hw.rapl_mask |= (uint8_t)(1u << d);
        }
    }
    rapl_period = tsc_ns_to_cycles(1000000000ULL);
    rapl_next_tsc = cpu_rdtsc() + rapl_period;
}

uint32_t power_energy_read(uint64_t uj[POWER_RAPL_DOMAINS]) {
    uint64_t flags;
    spin_lock_irqsave(&rapl_lock, &flags);
    rapl_sample_locked();
    for (uint32_t d = 0; d < POWER_RAPL_DOMAINS; d++) uj[d] = power_rapl_to_uj(rapl_units[d], hw.rapl_esu);
    spin_unlock_irqrestore(&rapl_lock, flags);
    return hw.rapl_mask;
}

/* --- Bring-up --- */

void power_hw_init(int idle_override, uint8_t epp) {
    const CpuFeatures *f = cpu_features();
    probe_idle(idle_override);
    probe_rapl();
    hw.hwp = (f->l6_eax & CPUID6_EAX_HWP) != 0;
    hw.epp = hw.hwp && (f->l6_eax & CPUID6_EAX_EPP) != 0;
    want_epp = epp;
    __atomic_store_n(&epp_gen, 1, __ATOMIC_RELEASE);   /* publishes hw; every CPU's power_gen starts at 0 */

    if (hw.idle_count > 1) {
        kprintf("[POWER] Idle: MWAIT %s + %s (hint 0x%x, exit %u us, worth it from %u us)\n", hw.idle[0].name,
                hw.idle[1].name, hw.idle[1].hint, hw.idle[1].exit_us, hw.idle[1].residency_us);
    } else {
        kprintf("[POWER] Idle: %s (%s)\n", hw.idle[0].hint == POWER_HINT_HLT ? "HLT" : "MWAIT C1", hw.idle_why);
    }
    if (hw.hwp) {
        uint32_t others = ipi_call_all(hwp_sync_fn, NULL, HWP_CALL_CYCLES);
        uint64_t caps = hw.hwp_caps0;
        kprintf("[POWER] HWP on %u of %u CPUs%s, EPP %s%u; CPU 0 perf %lu-%lu (guaranteed %lu, efficient %lu)\n",
                __atomic_load_n(&hw.hwp_cpus, __ATOMIC_RELAXED), cpu_count,
                others + 1 < cpu_count ? " (others apply it when they next idle)" : "",
                hw.epp ? "" : "not supported, would be ", epp, (caps >> 24) & 0xFF, caps & 0xFF, (caps >> 8) & 0xFF,
                (caps >> 16) & 0xFF);
    } else {
        kprintf("[POWER] HWP: not present; the firmware's P-state settings stay\n");
    }
    if (hw.rapl_mask != 0) {
        kprintf("[POWER] RAPL:%s%s%s, energy unit 1/2^%u J\n", hw.rapl_mask & 1 ? " package" : "",
                hw.rapl_mask & 2 ? " cores" : "", hw.rapl_mask & 4 ? " platform" : "", hw.rapl_esu);
    } else {
        kprintf("[POWER] RAPL: not present\n");
    }
}

const PowerHwInfo *power_hw_info(void) {
    return &hw;
}
