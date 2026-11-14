#ifndef ARCHOS_POWER_POWER_H
#define ARCHOS_POWER_POWER_H

#include <stdint.h>
#include <stddef.h>

/* Power profiles, idle and the energy readout (M1-21, ADR-0017).
 *
 * The one interface the rest of the kernel uses for power: the idle loop
 * sleeps through power_idle, /proc/arc/power reads power_status and sets
 * the profile by name. ADR-0017 fixes the final file names in M4; only
 * procfs changes then. A profile sets the CPUs' energy preference (HWP EPP)
 * in this block; M4-12 adds thread count, core choice and display timeout.
 *
 * Locks: status_lock (LOCK_LEVEL_POWER, leaf) around the last-read
 * snapshot and profile changes. Cost: power_idle adds one clock read to an
 * idle entry; status and profile changes are rare (a file read or write). */

typedef enum {
    POWER_PROFILE_PERFORMANCE = 0,   /* on AC: fastest response */
    POWER_PROFILE_BALANCED = 1,      /* default */
    POWER_PROFILE_SAVER = 2,         /* on battery */
    POWER_PROFILE_COUNT = 3,
} PowerProfile;

#define POWER_DOMAINS 3   /* package, cores, platform (RAPL) */

/* What /proc/arc/power shows */
typedef struct {
    PowerProfile profile;
    uint8_t epp;                       /* EPP the profile asks for */
    uint8_t hwp;                       /* CPU has HWP */
    uint8_t epp_supported;             /* ... and takes an EPP */
    uint32_t hwp_cpus;                 /* CPUs with HWP on */
    uint32_t hwp_failed;               /* CPUs where an HWP register refused */
    const char *idle_method;           /* "mwait" / "hlt" */
    const char *idle_why;              /* why not deeper ("" if nothing to say) */
    uint32_t idle_count;
    const char *idle_names[2];
    uint32_t energy_mask;              /* bit d: domain d present */
    uint64_t energy_uj[POWER_DOMAINS]; /* since boot */
    uint64_t avg_mw[POWER_DOMAINS];    /* average since the previous status read */
    uint64_t interval_ns;              /* length of that interval (0 on the first read) */
} PowerStatus;

/* Probe the hardware, read arc.idle, start in the balanced profile. Boot,
 * after clock_init (every CPU's timer queue). */
void power_init(void);

/* Idle loop: sleep this CPU until something happens, as deep as the next
 * timer allows. Interrupts off on entry, on at return; wake is a word whose
 * write should end the sleep early (the run queue's count) or NULL. */
void power_idle(const volatile void *wake);

/* Switch profile (every CPU's EPP). Returns 0 or -EINVAL. */
int power_set_profile(PowerProfile p);

/* The active profile. */
PowerProfile power_get_profile(void);

/* "performance", "balanced", "saver"; "?" for anything else. */
const char *power_profile_name(PowerProfile p);

/* Parse a profile name (len bytes, trailing newline and spaces ignored).
 * Returns 0 and the profile, or -1. */
int power_profile_parse(const char *s, size_t len, PowerProfile *out);

/* EPP for a profile (Linux's values: performance 0, balance_performance
 * 128, balance_power 192). */
uint8_t power_profile_epp(PowerProfile p);

/* Fill st, and start the next "since the previous read" interval. */
void power_status(PowerStatus *st);

/* Average milliwatts for uj microjoules over ns nanoseconds (0 if ns is 0). */
uint64_t power_avg_mw(uint64_t uj, uint64_t ns);

#endif /* ARCHOS_POWER_POWER_H */
