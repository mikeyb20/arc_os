/* arc_os — Host-side tests for kernel/arch/x86_64/power.c and
 * kernel/power/power.c (M1-21)
 *
 * The real idle-state probe, idle entry and residency accounting, HWP set-up
 * and EPP changes, RAPL extension and the profile layer, over fake CPUID,
 * fake per-CPU model-specific registers (a missing one "faults" like
 * rdmsr_safe's #GP), a fake TSC and two simulated CPUs. The idle
 * instructions are recorders: HLT "takes" an interrupt (which closes the
 * residency, as the interrupt entry does), MWAIT reports its hint. */

#include "test_framework.h"
#include <stdint.h>
#include <string.h>

/* Guard kernel headers that need stubbing */
#define ARCHOS_ARCH_X86_64_CPUID_H
#define ARCHOS_ARCH_X86_64_PERCPU_H
#define ARCHOS_ARCH_X86_64_IPI_H
#define ARCHOS_ARCH_X86_64_LAPIC_H
#define ARCHOS_ARCH_X86_64_TSC_H
#define ARCHOS_ARCH_HAL_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_STRING_H
#define ARCHOS_LIB_CMDLINE_H
#define ARCHOS_TIME_CLOCK_H
#define ARCHOS_TIME_TIMER_H

/* Unique names: other suites may link the real ones */
#define cpu_features  power_test_cpu_features
#define cpu_signature power_test_cpu_signature
#define rdmsr_safe    power_test_rdmsr_safe
#define wrmsr_safe    power_test_wrmsr_safe
#include "../kernel/arch/x86_64/cpu.h"
#include "../kernel/arch/x86_64/msr.h"

static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}

/* ---- Fake CPU identification ---- */

static CpuFeatures fake_features;
const CpuFeatures *power_test_cpu_features(void) {
    return &fake_features;
}
static uint32_t fake_family = 6, fake_model;
void power_test_cpu_signature(uint32_t *family, uint32_t *model, uint32_t *stepping) {
    *family = fake_family;
    *model = fake_model;
    *stepping = 0;
}
static uint32_t leaf5_ecx, leaf5_edx;
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    (void)sub;
    *a = *b = *c = *d = 0;
    if (leaf == 5) {
        *c = leaf5_ecx;
        *d = leaf5_edx;
    }
}

/* ---- Fake TSC (1 cycle = 1 ns) and idle instructions ---- */

static uint64_t fake_tsc = 1000;
static uint64_t cpu_rdtsc(void) {
    return fake_tsc;
}
static inline uint64_t tsc_cycles_to_ns(uint64_t c) {
    return c;
}
static inline uint64_t tsc_ns_to_cycles(uint64_t n) {
    return n;
}
static uint64_t sleep_cycles = 5000;   /* how long each fake sleep lasts */
static int hlts, mwaits, stis;
static uint32_t last_hint;
static const volatile void *last_wake;
void power_idle_close(void);
static void cpu_sti_hlt(void) {
    hlts++;
    fake_tsc += sleep_cycles;
    power_idle_close();   /* the interrupt's entry closes the residency */
}
static void cpu_sti(void) {
    stis++;
}
static void (*during_mwait)(void);
static void cpu_monitor_mwait(const volatile void *addr, uint32_t hint) {
    mwaits++;
    last_hint = hint;
    last_wake = addr;
    fake_tsc += sleep_cycles / 2;
    if (during_mwait) during_mwait();
    fake_tsc += sleep_cycles - sleep_cycles / 2;
}

/* ---- Two simulated CPUs ---- */

#define PERCPU_IDLE_STATES 2
#define PERCPU_HWP_UNTRIED 0
#define PERCPU_HWP_ON      1
#define PERCPU_HWP_FAILED  2
typedef struct PerCpu {
    uint32_t cpu_index;
    volatile uint8_t online;
    uint64_t idle_tsc;
    uint32_t idle_state;
    uint32_t idle_seq;
    uint32_t power_gen;
    uint64_t idle_cycles[PERCPU_IDLE_STATES];
    uint64_t idle_entries[PERCPU_IDLE_STATES];
    uint64_t hwp_req;
    uint8_t hwp_state;
} PerCpu;
#define TEST_CPUS 2
static PerCpu test_cpus[TEST_CPUS];
static uint32_t cur_cpu;
static uint32_t cpu_count = TEST_CPUS;
static PerCpu *this_cpu(void) {
    return &test_cpus[cur_cpu];
}
static PerCpu *percpu_get(uint32_t i) {
    return i < TEST_CPUS ? &test_cpus[i] : NULL;
}

#define IPI_VEC_SCHEDULE 0xF1
static int ipis_broadcast;
static void lapic_send_ipi_all_excluding_self(uint32_t vector) {
    if (vector == IPI_VEC_SCHEDULE) ipis_broadcast++;
}
static uint32_t ipi_call_all(void (*fn)(void *arg), void *arg, uint64_t max_cycles) {
    (void)max_cycles;
    uint32_t self = cur_cpu;
    for (uint32_t i = 0; i < cpu_count; i++) {
        cur_cpu = i;
        fn(arg);
    }
    cur_cpu = self;
    return cpu_count - 1;
}

/* ---- Fake model-specific registers (per CPU for HWP) ---- */

typedef struct {
    uint64_t val;
    int present;
    int wr_fault;   /* writes raise #GP */
    int writes;
} FakeMsr;
static FakeMsr pm_enable[TEST_CPUS], hwp_caps[TEST_CPUS], hwp_req[TEST_CPUS];
static FakeMsr rapl_unit, rapl_pkg, rapl_pp0, rapl_psys;

static FakeMsr *fake_msr(uint32_t msr) {
    switch (msr) {
    case MSR_PM_ENABLE:
        return &pm_enable[cur_cpu];
    case MSR_HWP_CAPABILITIES:
        return &hwp_caps[cur_cpu];
    case MSR_HWP_REQUEST:
        return &hwp_req[cur_cpu];
    case MSR_RAPL_POWER_UNIT:
        return &rapl_unit;
    case MSR_PKG_ENERGY_STATUS:
        return &rapl_pkg;
    case MSR_PP0_ENERGY_STATUS:
        return &rapl_pp0;
    case MSR_PLATFORM_ENERGY_STATUS:
        return &rapl_psys;
    default:
        return NULL;
    }
}
int power_test_rdmsr_safe(uint32_t msr, uint64_t *out) {
    FakeMsr *m = fake_msr(msr);
    if (m == NULL || !m->present) {
        *out = 0;
        return -5;
    }
    *out = m->val;
    return 0;
}
int power_test_wrmsr_safe(uint32_t msr, uint64_t value) {
    FakeMsr *m = fake_msr(msr);
    if (m == NULL || !m->present || m->wr_fault) return -5;
    m->val = value;
    m->writes++;
    return 0;
}

#include "../kernel/arch/x86_64/power.c"

/* ---- The HAL, clock, timer and command line under kernel/power/power.c ---- */

static inline void hal_power_init(int idle_override, uint8_t epp) {
    power_hw_init(idle_override, epp);
}
static inline const PowerHwInfo *hal_power_info(void) {
    return power_hw_info();
}
static inline void hal_idle_enter(uint64_t sleep_ns, const volatile void *wake) {
    power_idle_enter(sleep_ns, wake);
}
static inline void hal_power_set_epp(uint8_t epp) {
    power_set_epp(epp);
}
static inline uint32_t hal_power_energy(uint64_t uj[POWER_RAPL_DOMAINS]) {
    return power_energy_read(uj);
}
static uint64_t fake_now_ns = 1000000;
static uint64_t clock_mono_ns(void) {
    return fake_now_ns;
}
#define TIMER_NEVER UINT64_MAX
static uint64_t fake_next_event = TIMER_NEVER;
static uint64_t timer_next_event(void) {
    return fake_next_event;
}
static const char *fake_cmdline = "";
static const char *cmdline_raw(void) {
    return fake_cmdline;
}
static int cmdline_get(const char *line, const char *key, char *out, size_t outsz) {
    const char *p = strstr(line, key);
    if (p == NULL || p[strlen(key)] != '=') return 0;
    p += strlen(key) + 1;
    size_t n = strcspn(p, " ");
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

#include "../kernel/power/power.c"

/* ---- Set-ups ---- */

#define L6_ARAT (1u << 2)
#define L6_HWP  (1u << 7)
#define L6_EPP  (1u << 10)

/* An Intel CPU with MWAIT (leaf 5: C1 x2, C6 x2 sub-states), ARAT, HWP+EPP,
 * RAPL package + cores; model m. Two CPUs with different HWP limits
 * (hybrid: CPU 0 a P-core 5..0x40, CPU 1 an E-core 4..0x20). */
static void reset_machine(uint32_t model) {
    memset(&fake_features, 0, sizeof(fake_features));
    strcpy(fake_features.vendor, "GenuineIntel");
    fake_features.max_leaf = 0x20;
    fake_features.l1_ecx = 1u << 3;
    fake_features.l6_eax = L6_ARAT | L6_HWP | L6_EPP;
    fake_family = 6;
    fake_model = model;
    leaf5_ecx = 3;
    leaf5_edx = (2u << 4) | (2u << 12);
    memset(test_cpus, 0, sizeof(test_cpus));
    for (uint32_t i = 0; i < TEST_CPUS; i++) {
        test_cpus[i].cpu_index = i;
        test_cpus[i].online = 1;
        pm_enable[i] = (FakeMsr){0, 1, 0, 0};
        hwp_req[i] = (FakeMsr){0x80002A04ULL | (0x3FFULL << 32), 1, 0, 0}; /* firmware: EPP 0x80, 4..0x2A, window set */
    }
    hwp_caps[0] = (FakeMsr){0x05102040ULL, 1, 0, 0}; /* lowest 5, efficient 0x10, guaranteed 0x20, highest 0x40 */
    hwp_caps[1] = (FakeMsr){0x04081820ULL, 1, 0, 0}; /* lowest 4, highest 0x20 */
    rapl_unit = (FakeMsr){0xA0E03, 1, 0, 0}; /* energy unit 2^-14 J */
    rapl_pkg = (FakeMsr){1000, 1, 0, 0};
    rapl_pp0 = (FakeMsr){500, 1, 0, 0};
    rapl_psys = (FakeMsr){0, 1, 0, 0}; /* reads 0: no PSYS on this platform */
    cur_cpu = 0;
    cpu_count = TEST_CPUS;
    fake_tsc = 1000;
    sleep_cycles = 5000;
    hlts = mwaits = stis = 0;
    last_hint = 0xDEAD;
    during_mwait = NULL;
    ipis_broadcast = 0;
    memset(&hw, 0, sizeof(hw));
    want_epp = 0;
    epp_gen = 0;
    memset(rapl_units, 0, sizeof(rapl_units));
    memset(rapl_last, 0, sizeof(rapl_last));
    rapl_next_tsc = 0;
    profile = POWER_PROFILE_BALANCED;
    memset(last_uj, 0, sizeof(last_uj));
    last_ns = 0;
    fake_now_ns = 1000000;
    fake_next_event = TIMER_NEVER;
    fake_cmdline = "";
}

/* ---- Pure helpers ---- */

TEST(power_mwait_hint_check) {
    uint32_t edx = (2u << 4) | (1u << 12); /* C1: 2 sub-states, MWAIT C3 slot (hint 0x2x): 1 */
    ASSERT_TRUE(power_mwait_hint_ok(0x00, edx));
    ASSERT_TRUE(power_mwait_hint_ok(0x01, edx)); /* C1E: sub-state 1 of 2 */
    ASSERT_FALSE(power_mwait_hint_ok(0x02, edx)); /* sub-state 2 doesn't exist */
    ASSERT_TRUE(power_mwait_hint_ok(0x20, edx));
    ASSERT_FALSE(power_mwait_hint_ok(0x21, edx));
    ASSERT_FALSE(power_mwait_hint_ok(0x10, edx)); /* nibble 2 empty */
    ASSERT_FALSE(power_mwait_hint_ok(0x00, 0)); /* TCG: EDX 0 */
    ASSERT_FALSE(power_mwait_hint_ok(0x70, 0xFFFFFFFFu)); /* past the last nibble */
    return 0;
}

TEST(power_idle_select_by_residency) {
    PowerIdleState st[2] = {{"C1", 0, 2, 2}, {"C6", 0x20, 85, 200}};
    ASSERT_EQ(power_idle_select(st, 2, 0), 0);
    ASSERT_EQ(power_idle_select(st, 2, 199999), 0);
    ASSERT_EQ(power_idle_select(st, 2, 200000), 1);
    ASSERT_EQ(power_idle_select(st, 2, UINT64_MAX), 1);
    ASSERT_EQ(power_idle_select(st, 1, UINT64_MAX), 0);
    return 0;
}

TEST(power_hwp_request_fields) {
    uint64_t caps = 0x05102040ULL; /* lowest 5, highest 0x40 */
    uint64_t old = (0x3FFULL << 32) | (1ULL << 42) | (0x33ULL << 24) | (0x77ULL << 16) | 0x1111;
    uint64_t r = power_hwp_request(old, caps, 0x80);
    ASSERT_EQ(r & 0xFF, 0x05); /* min = lowest */
    ASSERT_EQ((r >> 8) & 0xFF, 0x40); /* max = highest */
    ASSERT_EQ((r >> 16) & 0xFF, 0); /* desired 0: the CPU picks */
    ASSERT_EQ((r >> 24) & 0xFF, 0x80); /* EPP */
    ASSERT_EQ(r >> 32, (old >> 32)); /* activity window and package control kept */
    ASSERT_EQ((power_hwp_request(old, caps, -1) >> 24) & 0xFF, 0x33); /* no EPP: the old field stays */
    return 0;
}

TEST(power_rapl_maths) {
    uint32_t last = 0xFFFFFFF0u;
    ASSERT_EQ(power_rapl_extend(100, &last, 0x10), 100 + 0x20); /* across the wrap */
    ASSERT_EQ(last, 0x10);
    ASSERT_EQ(power_rapl_extend(0, &last, 0x10), 0); /* no change */
    ASSERT_EQ(power_rapl_to_uj(16384, 14), 1000000); /* 2^14 units of 2^-14 J = 1 J */
    ASSERT_EQ(power_rapl_to_uj(1, 14), 61); /* 61.035 uJ, rounded down */
    ASSERT_EQ(power_rapl_to_uj(1ULL << 50, 14), 68719476736000000ULL); /* no 64-bit overflow */
    ASSERT_EQ(power_avg_mw(1000000, 1000000000ULL), 1000); /* 1 J over 1 s = 1 W */
    ASSERT_EQ(power_avg_mw(500, 0), 0);
    ASSERT_EQ(power_avg_mw(UINT64_MAX / 100, 1000000000ULL), (UINT64_MAX / 100) / 1000000 * 1000);
    return 0;
}

/* ---- Idle-state probe ---- */

TEST(power_probe_test_cpus) {
    static const uint32_t models[] = {0x8E, 0x9E, 0xC5};
    for (uint32_t i = 0; i < 3; i++) {
        reset_machine(models[i]);
        power_hw_init(POWER_IDLE_AUTO, 0x80);
        ASSERT_STR_EQ(hw.idle_method, "mwait");
        ASSERT_EQ(hw.idle_count, 2);
        ASSERT_STR_EQ(hw.idle[0].name, "C1");
        ASSERT_STR_EQ(hw.idle[1].name, "C6");
        ASSERT_EQ(hw.idle[1].hint, 0x20);
    }
    ASSERT_EQ(hw.idle[1].residency_us, 420); /* Arrow Lake H: the Meteor Lake figures */
    return 0;
}

TEST(power_probe_fallbacks) {
    reset_machine(0x55); /* a model not in the table */
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_EQ(hw.idle_count, 1);
    ASSERT_STR_EQ(hw.idle[0].name, "C1");
    ASSERT_STR_EQ(hw.idle_why, "model not in table");

    reset_machine(0x8E);
    fake_features.l6_eax &= ~L6_ARAT; /* the timer would stop in C6 */
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_EQ(hw.idle_count, 1);
    ASSERT_STR_EQ(hw.idle_why, "no ARAT");

    reset_machine(0xC5);
    leaf5_edx = 2u << 4; /* C6 not listed */
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_EQ(hw.idle_count, 1);
    ASSERT_STR_EQ(hw.idle_why, "leaf 5 lacks deep state");

    reset_machine(0x8E);
    leaf5_ecx = 1; /* no interrupt break with interrupts masked */
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_STR_EQ(hw.idle_method, "hlt");
    ASSERT_EQ(hw.idle[0].hint, POWER_HINT_HLT);

    reset_machine(0x8E);
    fake_features.l1_ecx = 0; /* QEMU: no MONITOR/MWAIT */
    power_hw_init(POWER_IDLE_C1, 0x80);
    ASSERT_STR_EQ(hw.idle_method, "hlt");
    ASSERT_STR_EQ(hw.idle_why, "no MWAIT");

    reset_machine(0x8E);
    strcpy(fake_features.vendor, "AuthenticAMD");
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_STR_EQ(hw.idle_method, "hlt");
    ASSERT_EQ(hw.rapl_mask, 0); /* Intel RAPL registers only */

    reset_machine(0x8E);
    power_hw_init(POWER_IDLE_HLT, 0x80); /* arc.idle=hlt */
    ASSERT_STR_EQ(hw.idle_method, "hlt");
    reset_machine(0x8E);
    power_hw_init(POWER_IDLE_C1, 0x80); /* arc.idle=c1 */
    ASSERT_STR_EQ(hw.idle_method, "mwait");
    ASSERT_EQ(hw.idle_count, 1);
    return 0;
}

/* ---- Idle entry and residency ---- */

TEST(power_idle_hlt_counts_residency) {
    reset_machine(0x8E);
    fake_features.l1_ecx = 0;
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    power_idle_enter(UINT64_MAX, NULL);
    power_idle_enter(UINT64_MAX, NULL);
    ASSERT_EQ(hlts, 2);
    ASSERT_EQ(mwaits, 0);
    uint64_t ns[2], n[2];
    ASSERT_EQ(power_idle_stats(0, ns, n), 0);
    ASSERT_EQ(n[0], 2);
    ASSERT_EQ(ns[0], 10000);
    ASSERT_EQ(test_cpus[0].idle_tsc, 0); /* closed by the "interrupt" */
    ASSERT_EQ(test_cpus[0].idle_seq & 1, 0);
    test_cpus[1].online = 0;
    ASSERT_EQ(power_idle_stats(1, ns, n), -1);
    return 0;
}

static uint64_t mid_ns[2], mid_n[2];
static void read_stats_mid_sleep(void) {
    power_idle_stats(0, mid_ns, mid_n);
}

TEST(power_idle_mwait_picks_state) {
    reset_machine(0x9E);
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    int wake_word = 0;
    power_idle_enter(150000, &wake_word); /* 150 us: C6 isn't worth it (200 us) */
    ASSERT_EQ(last_hint, 0x00);
    ASSERT_TRUE(last_wake == &wake_word);
    power_idle_enter(250000, &wake_word);
    ASSERT_EQ(last_hint, 0x20);
    power_idle_enter(UINT64_MAX, NULL); /* no timer: deepest */
    ASSERT_EQ(last_hint, 0x20);
    ASSERT_TRUE(last_wake == &test_cpus[0].idle_state); /* a mapped line of this CPU */
    ASSERT_EQ(mwaits, 3);
    ASSERT_EQ(stis, 3); /* interrupts back on after each */
    uint64_t ns[2], n[2];
    power_idle_stats(0, ns, n);
    ASSERT_EQ(n[0], 1);
    ASSERT_EQ(n[1], 2);
    ASSERT_EQ(ns[0], 5000);
    ASSERT_EQ(ns[1], 10000);

    /* A reader during a sleep sees the open part counted once */
    during_mwait = read_stats_mid_sleep;
    power_idle_enter(UINT64_MAX, NULL);
    ASSERT_EQ(mid_n[1], 3);
    ASSERT_EQ(mid_ns[1], 10000 + 2500); /* half the fake sleep so far */
    power_idle_stats(0, ns, n);
    ASSERT_EQ(ns[1], 15000);
    return 0;
}

/* ---- HWP and EPP ---- */

TEST(power_hwp_every_cpu_its_own_limits) {
    reset_machine(0xC5);
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_EQ(hw.hwp_cpus, 2);
    ASSERT_EQ(hw.hwp_failed, 0);
    for (uint32_t i = 0; i < TEST_CPUS; i++) {
        ASSERT_EQ(pm_enable[i].val & 1, 1);
        ASSERT_EQ((hwp_req[i].val >> 24) & 0xFF, 0x80);
        ASSERT_EQ(hwp_req[i].val >> 32, 0x3FF); /* firmware's activity window kept */
    }
    ASSERT_EQ(hwp_req[0].val & 0xFFFF, 0x4005); /* P-core: 5..0x40 */
    ASSERT_EQ(hwp_req[1].val & 0xFFFF, 0x2004); /* E-core: 4..0x20 */
    ASSERT_EQ(hw.hwp_caps0, 0x05102040ULL);

    /* A new EPP: this CPU at once, the other on its IPI or next idle */
    cur_cpu = 1;
    power_set_epp(0xC0);
    ASSERT_EQ(ipis_broadcast, 1);
    ASSERT_EQ((hwp_req[1].val >> 24) & 0xFF, 0xC0);
    ASSERT_EQ((hwp_req[0].val >> 24) & 0xFF, 0x80);
    cur_cpu = 0;
    int writes = hwp_req[0].writes;
    power_cpu_sync(); /* the reschedule IPI */
    ASSERT_EQ((hwp_req[0].val >> 24) & 0xFF, 0xC0);
    ASSERT_EQ(hwp_req[0].val & 0xFFFF, 0x4005); /* limits unchanged */
    power_cpu_sync(); /* nothing new: no MSR write */
    ASSERT_EQ(hwp_req[0].writes, writes + 1);
    ASSERT_EQ(power_get_epp(), 0xC0);
    return 0;
}

TEST(power_hwp_absent_or_refused) {
    reset_machine(0x8E);
    fake_features.l6_eax = L6_ARAT; /* no HWP: firmware's settings stay */
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_EQ(pm_enable[0].writes + hwp_req[0].writes, 0);
    power_set_epp(0x00);
    ASSERT_EQ(ipis_broadcast, 0);
    ASSERT_EQ(hwp_req[0].writes, 0);

    reset_machine(0x8E);
    fake_features.l6_eax = L6_ARAT | L6_HWP; /* HWP without EPP */
    power_hw_init(POWER_IDLE_AUTO, 0xC0);
    ASSERT_EQ((hwp_req[0].val >> 24) & 0xFF, 0x80); /* the old field, not 0xC0 */

    reset_machine(0x8E);
    pm_enable[1].wr_fault = 1; /* CPU 1 refuses (#GP): counted, not fatal */
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_EQ(hw.hwp_cpus, 1);
    ASSERT_EQ(hw.hwp_failed, 1);
    ASSERT_EQ(test_cpus[1].hwp_state, PERCPU_HWP_FAILED);
    ASSERT_EQ(hwp_req[1].writes, 0);
    cur_cpu = 1;
    power_set_epp(0xFF); /* a failed CPU stays out */
    ASSERT_EQ(hwp_req[1].writes, 0);
    ASSERT_EQ(hw.hwp_failed, 1);
    return 0;
}

/* ---- RAPL ---- */

TEST(power_rapl_domains_and_wrap) {
    reset_machine(0x8E);
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_EQ(hw.rapl_mask, 3); /* package, cores; PSYS reads 0 */
    ASSERT_EQ(hw.rapl_esu, 14);
    uint64_t uj[3];
    rapl_pkg.val = 1000 + 16384; /* +1 J */
    ASSERT_EQ(power_energy_read(uj), 3);
    ASSERT_EQ(uj[0], 1000000);
    ASSERT_EQ(uj[1], 0);
    ASSERT_EQ(uj[2], 0);
    rapl_pkg.val = 0xFFFFFF00u;
    power_energy_read(uj);
    rapl_pkg.val = 0x100; /* wrapped */
    power_energy_read(uj);
    ASSERT_EQ(uj[0], power_rapl_to_uj(16384 + (0xFFFFFF00u - 1000 - 16384) + 0x200, 14));

    /* The idle path samples at most once a period and never waits */
    uint64_t before = rapl_units[0];
    rapl_pkg.val = 0x100 + 16384;
    fake_tsc = rapl_next_tsc - 1;
    fake_features.l1_ecx = 0;
    power_idle_enter(UINT64_MAX, NULL);
    ASSERT_EQ(rapl_units[0], before); /* not due yet */
    fake_tsc = rapl_next_tsc;
    power_idle_enter(UINT64_MAX, NULL);
    ASSERT_EQ(rapl_units[0], before + 16384);
    spin_lock(&rapl_lock);
    rapl_pkg.val += 16384;
    fake_tsc = rapl_next_tsc;
    power_idle_enter(UINT64_MAX, NULL); /* lock busy: skipped, no deadlock */
    spin_unlock(&rapl_lock);
    ASSERT_EQ(rapl_units[0], before + 16384);

    reset_machine(0x8E);
    rapl_unit.val = 0; /* QEMU TCG: unknown MSRs read as 0 */
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_EQ(hw.rapl_mask, 0);
    reset_machine(0x8E);
    rapl_unit.present = 0; /* KVM: #GP */
    power_hw_init(POWER_IDLE_AUTO, 0x80);
    ASSERT_EQ(power_energy_read(uj), 0);
    ASSERT_EQ(uj[0], 0);
    return 0;
}

/* ---- Profiles and status (kernel/power/power.c) ---- */

TEST(power_profile_names_and_epp) {
    PowerProfile p;
    ASSERT_EQ(power_profile_parse("saver\n", 6, &p), 0);
    ASSERT_EQ(p, POWER_PROFILE_SAVER);
    ASSERT_EQ(power_profile_parse("performance", 11, &p), 0);
    ASSERT_EQ(p, POWER_PROFILE_PERFORMANCE);
    ASSERT_EQ(power_profile_parse("balanced  \n", 11, &p), 0);
    ASSERT_EQ(p, POWER_PROFILE_BALANCED);
    ASSERT_EQ(power_profile_parse("balance", 7, &p), -1);
    ASSERT_EQ(power_profile_parse("balancedx", 9, &p), -1);
    ASSERT_EQ(power_profile_parse("", 0, &p), -1);
    ASSERT_EQ(power_profile_epp(POWER_PROFILE_PERFORMANCE), 0x00);
    ASSERT_EQ(power_profile_epp(POWER_PROFILE_BALANCED), 0x80);
    ASSERT_EQ(power_profile_epp(POWER_PROFILE_SAVER), 0xC0);
    ASSERT_STR_EQ(power_profile_name(POWER_PROFILE_SAVER), "saver");
    ASSERT_STR_EQ(power_profile_name((PowerProfile)7), "?");
    return 0;
}

TEST(power_set_profile_sets_epp) {
    reset_machine(0x8E);
    power_init();
    ASSERT_EQ(power_get_profile(), POWER_PROFILE_BALANCED);
    ASSERT_EQ((hwp_req[0].val >> 24) & 0xFF, 0x80);
    ASSERT_EQ(power_set_profile(POWER_PROFILE_SAVER), 0);
    ASSERT_EQ(power_get_profile(), POWER_PROFILE_SAVER);
    ASSERT_EQ((hwp_req[0].val >> 24) & 0xFF, 0xC0);
    ASSERT_EQ(power_set_profile((PowerProfile)3), -EINVAL);
    ASSERT_EQ(power_get_profile(), POWER_PROFILE_SAVER);

    reset_machine(0x8E);
    fake_cmdline = "arc.test=x arc.idle=hlt";
    power_init();
    ASSERT_STR_EQ(hw.idle_method, "hlt");
    reset_machine(0x8E);
    fake_cmdline = "arc.idle=c1";
    power_init();
    ASSERT_EQ(hw.idle_count, 1);
    ASSERT_STR_EQ(hw.idle_method, "mwait");
    return 0;
}

TEST(power_idle_uses_next_timer) {
    reset_machine(0x8E);
    power_init();
    fake_next_event = fake_now_ns + 100000; /* 100 us away: C1 */
    power_idle(NULL);
    ASSERT_EQ(last_hint, 0x00);
    fake_next_event = fake_now_ns + 300000; /* 300 us: C6 */
    power_idle(NULL);
    ASSERT_EQ(last_hint, 0x20);
    fake_next_event = fake_now_ns - 5; /* already due: shallowest */
    power_idle(NULL);
    ASSERT_EQ(last_hint, 0x00);
    fake_next_event = TIMER_NEVER;
    power_idle(NULL);
    ASSERT_EQ(last_hint, 0x20);
    return 0;
}

TEST(power_status_average_watts) {
    reset_machine(0x8E);
    power_init();
    PowerStatus st;
    power_status(&st); /* the first read starts the interval */
    ASSERT_EQ(st.interval_ns, 0);
    ASSERT_EQ(st.energy_mask, 3);
    ASSERT_EQ(st.avg_mw[0], 0);
    ASSERT_EQ(st.hwp, 1);
    ASSERT_EQ(st.hwp_cpus, 2);
    ASSERT_EQ(st.idle_count, 2);
    ASSERT_STR_EQ(st.idle_names[1], "C6");
    rapl_pkg.val += 2 * 16384; /* 2 J */
    rapl_pp0.val += 16384; /* 1 J */
    fake_now_ns += 500000000; /* over 0.5 s */
    power_status(&st);
    ASSERT_EQ(st.interval_ns, 500000000);
    ASSERT_EQ(st.avg_mw[0], 4000);
    ASSERT_EQ(st.avg_mw[1], 2000);
    ASSERT_EQ(st.energy_uj[0], 2000000);
    ASSERT_EQ(st.profile, POWER_PROFILE_BALANCED);
    ASSERT_EQ(st.epp, 0x80);
    fake_now_ns += 1000000000; /* nothing used: 0 W */
    power_status(&st);
    ASSERT_EQ(st.avg_mw[0], 0);
    /* A reader that took its time before a racer stored a later one: an
     * empty interval, not an underflow */
    fake_now_ns -= 1000;
    power_status(&st);
    ASSERT_EQ(st.interval_ns, 0);
    ASSERT_EQ(st.avg_mw[0], 0);
    return 0;
}

TestCase power_tests[] = {
    TEST_ENTRY(power_mwait_hint_check),
    TEST_ENTRY(power_idle_select_by_residency),
    TEST_ENTRY(power_hwp_request_fields),
    TEST_ENTRY(power_rapl_maths),
    TEST_ENTRY(power_probe_test_cpus),
    TEST_ENTRY(power_probe_fallbacks),
    TEST_ENTRY(power_idle_hlt_counts_residency),
    TEST_ENTRY(power_idle_mwait_picks_state),
    TEST_ENTRY(power_hwp_every_cpu_its_own_limits),
    TEST_ENTRY(power_hwp_absent_or_refused),
    TEST_ENTRY(power_rapl_domains_and_wrap),
    TEST_ENTRY(power_profile_names_and_epp),
    TEST_ENTRY(power_set_profile_sets_epp),
    TEST_ENTRY(power_idle_uses_next_timer),
    TEST_ENTRY(power_status_average_watts),
};

int power_test_count = sizeof(power_tests) / sizeof(power_tests[0]);
