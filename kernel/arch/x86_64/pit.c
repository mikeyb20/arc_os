/* arc_os — PIT (8254): calibration only (M1-06)
 *
 * Owns: PIT channel 2 as a 50 ms reference for the TSC at boot, and keeping
 * channel 0 quiet. The kernel's time comes from the TSC and each CPU's local
 * APIC timer; nothing ticks periodically (ADR-0017).
 * Locks: none (BSP, boot). */

#include "arch/x86_64/pit.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/cpuid.h"
#include "lib/kprintf.h"

void pit_stop(void) {
    /* Mode 0 with a count of 0 (65536): at most one more interrupt, then
     * OUT0 stays high and IRQ0 stays quiet (its line is masked as well). */
    outb(PIT_COMMAND, PIT_CMD_CHANNEL0 | PIT_CMD_LOHI | PIT_CMD_MODE0);
    outb(PIT_CHANNEL0, 0);
    outb(PIT_CHANNEL0, 0);
}

/* Channel 2 one-shot, gated through port 0x61 (speaker off), as Linux's
 * pit_calibrate_tsc does it. Mode 0 drives OUT2 high when the count ends. */
#define PIT_CHANNEL2       0x42
#define PIT_PORT_B         0x61
#define PIT_PORT_B_GATE2   0x01
#define PIT_PORT_B_SPEAKER 0x02
#define PIT_PORT_B_OUT2    0x20
#define PIT_CMD_CHANNEL2   0x80

uint64_t pit_measure_tsc(uint32_t ms, uint64_t max_cycles) {
    uint32_t count = (uint32_t)((uint64_t)PIT_BASE_FREQ * ms / 1000);
    if (count == 0 || count > 0xFFFF) return 0;

    uint8_t port_b = inb(PIT_PORT_B);
    outb(PIT_PORT_B, (uint8_t)((port_b & ~PIT_PORT_B_SPEAKER) | PIT_PORT_B_GATE2));
    outb(PIT_COMMAND, PIT_CMD_CHANNEL2 | PIT_CMD_LOHI | PIT_CMD_MODE0);
    outb(PIT_CHANNEL2, (uint8_t)(count & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)(count >> 8));   /* counting starts here */

    uint64_t start = cpu_rdtsc();
    uint64_t now = start;
    while ((inb(PIT_PORT_B) & PIT_PORT_B_OUT2) == 0) {
        now = cpu_rdtsc();
        if (now - start > max_cycles) {
            now = 0;   /* channel 2 never finished: no usable PIT */
            break;
        }
    }
    outb(PIT_PORT_B, port_b);
    return now == 0 ? 0 : now - start;
}
