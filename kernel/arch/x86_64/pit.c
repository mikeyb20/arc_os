#include "arch/x86_64/pit.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/cpuid.h"
#include "lib/kprintf.h"

static volatile uint64_t pit_ticks = 0;
static uint32_t pit_freq = 0;
static void (*tick_hook)(void);

static void pit_handler(InterruptFrame *frame) {
    (void)frame;
    pit_ticks++;
    if (tick_hook) tick_hook();

    /* Print a heartbeat every second */
    if (pit_ticks % pit_freq == 0) {
        uint64_t seconds = pit_ticks / pit_freq;
        kdebug("[TIMER] %lu seconds\n", seconds);
    }
}

void pit_init(uint32_t freq_hz) {
    pit_freq = freq_hz;

    /* Calculate divisor */
    uint16_t divisor = (uint16_t)(PIT_BASE_FREQ / freq_hz);

    /* Channel 0, access mode lobyte/hibyte, mode 2 (rate generator) */
    outb(PIT_COMMAND, PIT_CMD_CHANNEL0 | PIT_CMD_LOHI | PIT_CMD_MODE2);

    /* Send divisor */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    io_wait();
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    /* Register IRQ 0 handler (vector 32) */
    isr_register_handler(IRQ_BASE + 0, pit_handler);

    /* Unmask IRQ 0 */
    pic_unmask(0);

    kprintf("[HAL] PIT initialized at %lu Hz (divisor=%u)\n",
            (uint64_t)freq_hz, (uint32_t)divisor);
}

void pit_set_tick_hook(void (*fn)(void)) {
    tick_hook = fn;
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}

uint64_t pit_get_uptime_ms(void) {
    if (pit_freq == 0) return 0;
    return (pit_ticks * 1000) / pit_freq;
}

/* Channel 2 one-shot, gated through port 0x61 (speaker off), as Linux's
 * pit_calibrate_tsc does it. Mode 0 drives OUT2 high when the count ends. */
#define PIT_CHANNEL2       0x42
#define PIT_PORT_B         0x61
#define PIT_PORT_B_GATE2   0x01
#define PIT_PORT_B_SPEAKER 0x02
#define PIT_PORT_B_OUT2    0x20
#define PIT_CMD_CHANNEL2   0x80
#define PIT_CMD_MODE0      0x00

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
