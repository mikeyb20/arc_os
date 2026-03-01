#include "arch/x86_64/pit.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/io.h"
#include "proc/sched.h"
#include "lib/kprintf.h"

static volatile uint64_t pit_ticks = 0;
static uint32_t pit_freq = 0;

/* Schedule every SCHED_QUANTUM ticks (100ms at 100 Hz) */
#define SCHED_QUANTUM 10

static void pit_handler(InterruptFrame *frame) {
    (void)frame;
    pit_ticks++;

    /* Print a heartbeat every second */
    if (pit_ticks % pit_freq == 0) {
        uint64_t seconds = pit_ticks / pit_freq;
        kprintf("[TIMER] %lu seconds\n", seconds);
    }

    /* Preemptive scheduling — interrupts already disabled by interrupt gate */
    if (pit_ticks % SCHED_QUANTUM == 0) {
        sched_schedule();
    }
}

void pit_init(uint32_t freq_hz) {
    pit_freq = freq_hz;

    /* Calculate divisor */
    uint16_t divisor = (uint16_t)(PIT_BASE_FREQ / freq_hz);

    /* Channel 0, access mode lobyte/hibyte, mode 2 (rate generator) */
    outb(PIT_COMMAND, 0x34);

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

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}

uint64_t pit_get_uptime_ms(void) {
    if (pit_freq == 0) return 0;
    return (pit_ticks * 1000) / pit_freq;
}
