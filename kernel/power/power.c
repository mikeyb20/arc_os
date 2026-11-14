/* arc_os — power profiles, idle and the energy readout (power.h; M1-21)
 *
 * Owns: the active profile and its EPP, the idle loop's choice of sleep
 * length, the status snapshot behind /proc/arc/power (energy since boot,
 * average watts since the previous read). The hardware side is the HAL's
 * (arch/x86_64/power.c).
 * Locks: status_lock (LOCK_LEVEL_POWER, leaf; the HAL's RAPL lock is
 * never taken under it). Cost: see power.h. */

#include "power/power.h"
#include "arch/hal.h"
#include "proc/spinlock.h"
#include "time/clock.h"
#include "time/timer.h"
#include "lib/cmdline.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "uapi/errno.h"

static const char *const profile_names[POWER_PROFILE_COUNT] = {"performance", "balanced", "saver"};
static const uint8_t profile_epp[POWER_PROFILE_COUNT] = {0x00, 0x80, 0xC0};

static Spinlock status_lock = SPINLOCK_INIT(LOCK_LEVEL_POWER, "power status");
static PowerProfile profile = POWER_PROFILE_BALANCED;
static uint64_t last_uj[POWER_DOMAINS];
static uint64_t last_ns;

_Static_assert(POWER_DOMAINS == POWER_RAPL_DOMAINS, "one energy figure per RAPL domain");

const char *power_profile_name(PowerProfile p) {
    return (unsigned)p < POWER_PROFILE_COUNT ? profile_names[p] : "?";
}

uint8_t power_profile_epp(PowerProfile p) {
    return (unsigned)p < POWER_PROFILE_COUNT ? profile_epp[p] : profile_epp[POWER_PROFILE_BALANCED];
}

int power_profile_parse(const char *s, size_t len, PowerProfile *out) {
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == ' ')) len--;
    for (uint32_t i = 0; i < POWER_PROFILE_COUNT; i++) {
        if (strlen(profile_names[i]) == len && strncmp(s, profile_names[i], len) == 0) {
            *out = (PowerProfile)i;
            return 0;
        }
    }
    return -1;
}

uint64_t power_avg_mw(uint64_t uj, uint64_t ns) {
    /* mW = uJ * 1000 / us. No 128-bit divide in the kernel (no libgcc);
     * uJ * 1000 overflows only past 18 GJ in one interval */
    uint64_t us = ns / 1000;
    if (us == 0) return 0;
    return uj <= UINT64_MAX / 1000 ? uj * 1000 / us : uj / us * 1000;
}

void power_idle(const volatile void *wake) {
    uint64_t next = timer_next_event();
    uint64_t sleep_ns = UINT64_MAX;
    if (next != TIMER_NEVER) {
        uint64_t now = clock_mono_ns();
        sleep_ns = next > now ? next - now : 0;
    }
    hal_idle_enter(sleep_ns, wake);
}

int power_set_profile(PowerProfile p) {
    if ((unsigned)p >= POWER_PROFILE_COUNT) return -EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&status_lock, &flags);   /* profile and EPP change together */
    profile = p;
    hal_power_set_epp(profile_epp[p]);
    spin_unlock_irqrestore(&status_lock, flags);
    kprintf("[POWER] Profile %s (EPP %u)\n", profile_names[p], profile_epp[p]);
    return 0;
}

PowerProfile power_get_profile(void) {
    return __atomic_load_n(&profile, __ATOMIC_RELAXED);
}

void power_status(PowerStatus *st) {
    const PowerHwInfo *hw = hal_power_info();
    uint64_t uj[POWER_DOMAINS];
    st->energy_mask = hal_power_energy(uj);
    uint64_t now = clock_mono_ns();

    uint64_t flags;
    spin_lock_irqsave(&status_lock, &flags);
    st->profile = profile;
    st->epp = profile_epp[profile];
    /* A racing reader may have stored a later time: never go negative */
    st->interval_ns = last_ns != 0 && now > last_ns ? now - last_ns : 0;
    for (uint32_t d = 0; d < POWER_DOMAINS; d++) {
        st->energy_uj[d] = uj[d];
        /* A reader racing this one may have stored a later sample: never go negative */
        uint64_t delta = uj[d] >= last_uj[d] ? uj[d] - last_uj[d] : 0;
        st->avg_mw[d] = power_avg_mw(delta, st->interval_ns);
        if (uj[d] > last_uj[d]) last_uj[d] = uj[d];
    }
    if (now > last_ns) last_ns = now;
    spin_unlock_irqrestore(&status_lock, flags);

    st->hwp = hw->hwp;
    st->epp_supported = hw->epp;
    st->hwp_cpus = __atomic_load_n(&hw->hwp_cpus, __ATOMIC_RELAXED);
    st->hwp_failed = __atomic_load_n(&hw->hwp_failed, __ATOMIC_RELAXED);
    st->idle_method = hw->idle_method;
    st->idle_why = hw->idle_why;
    st->idle_count = hw->idle_count;
    for (uint32_t i = 0; i < 2; i++) st->idle_names[i] = i < hw->idle_count ? hw->idle[i].name : "";
}

void power_init(void) {
    char val[8];
    int idle = POWER_IDLE_AUTO;
    if (cmdline_get(cmdline_raw(), "arc.idle", val, sizeof(val))) {
        if (strcmp(val, "hlt") == 0)
            idle = POWER_IDLE_HLT;
        else if (strcmp(val, "c1") == 0)
            idle = POWER_IDLE_C1;
        else
            kprintf("[POWER] arc.idle=%s not known (hlt, c1): automatic\n", val);
    }
    hal_power_init(idle, profile_epp[profile]);
    kprintf("[POWER] Profile %s\n", profile_names[profile]);
}
