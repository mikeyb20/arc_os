#ifndef ARCHOS_ARCH_X86_64_HPET_H
#define ARCHOS_ARCH_X86_64_HPET_H

#include <stdint.h>

/* HPET (M1-06): only as a calibration reference for the TSC when the PIT is
 * missing or clock-gated. The HPET is never a clocksource or a timer here. */

/* Map the HPET at phys, start its main counter if firmware left it stopped,
 * and return the TSC rate in kHz measured over ms milliseconds, or 0 (no
 * usable HPET, or it didn't advance within max_cycles TSC cycles). */
uint64_t hpet_measure_tsc_khz(uint64_t phys, uint32_t ms, uint64_t max_cycles);

#endif /* ARCHOS_ARCH_X86_64_HPET_H */
