/* arc_os — Local APIC access layer (M1-03)
 *
 * Owns: the APIC mode (xAPIC MMIO or x2APIC MSRs), register access, EOI, IPIs,
 * and the per-CPU timer as a one-shot clock event (M1-06: TSC-deadline mode,
 * else one-shot mode measured once against the TSC on the BSP).
 * Invariants: the mode is chosen once on the BSP before any AP starts and never
 * changes; every CPU runs in that mode (an AP left in xAPIC mode by the
 * bootloader is switched to x2APIC; xAPIC -> x2APIC is the only legal direction
 * without a reset). In xAPIC mode lapic_mmio is the HHDM address of the MMIO page.
 * Locks: none. Register access is per CPU; an xAPIC IPI is two register writes,
 * so lapic_send_ipi keeps interrupts off across them. */

#include "arch/x86_64/lapic.h"
#include "arch/x86_64/msr.h"
#include "arch/x86_64/cpuid.h"
#include "arch/x86_64/tsc.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/percpu.h"
#include "mm/vmm.h"
#include "arch/hal_lock.h"
#include "lib/kprintf.h"
#include <stddef.h>

#define CPUID1_ECX_X2APIC (1u << 21)

static volatile uint32_t *lapic_mmio;   /* xAPIC mode only */
static int x2apic_mode;
static int lapic_on;   /* set by lapic_init_bsp */

int lapic_x2apic(void) {
    return x2apic_mode;
}

int lapic_active(void) {
    return lapic_on;
}

uint32_t lapic_read(uint32_t reg) {
    if (x2apic_mode) return (uint32_t)rdmsr(MSR_X2APIC_FIRST + (reg >> 4));
    return lapic_mmio[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t val) {
    if (x2apic_mode) {
        wrmsr(MSR_X2APIC_FIRST + (reg >> 4), val);
        return;
    }
    lapic_mmio[reg / 4] = val;
}

/* Enable the APIC on this CPU in the chosen mode, then program it. */
static void lapic_enable_this_cpu(void) {
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (x2apic_mode) {
        if (!(base & APIC_BASE_EXTD)) {
            /* xAPIC -> x2APIC: EN must stay set; both bits in one write */
            wrmsr(MSR_APIC_BASE, base | APIC_BASE_EN | APIC_BASE_EXTD);
        }
    } else if (!(base & APIC_BASE_EN)) {
        wrmsr(MSR_APIC_BASE, base | APIC_BASE_EN);
    }

    /* Enable LAPIC via Spurious Interrupt Vector Register */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);

    /* Clear Error Status Register (write twice as per spec; x2APIC needs 0) */
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    /* Set Task Priority Register to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    /* Send EOI to clear any pending interrupts */
    lapic_eoi();
}

void lapic_init_bsp(uint64_t mmio_phys) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    uint64_t base = rdmsr(MSR_APIC_BASE);

    x2apic_mode = (ecx & CPUID1_ECX_X2APIC) || (base & APIC_BASE_EXTD);
    if (!x2apic_mode) {
        if (mmio_phys == 0) mmio_phys = base & APIC_BASE_ADDR_MASK;
        lapic_mmio = (volatile uint32_t *)(mmio_phys + vmm_get_hhdm_offset());
    }
    lapic_enable_this_cpu();
    lapic_on = 1;

    kprintf("[APIC] %s mode, BSP APIC ID %u, version 0x%x\n", x2apic_mode ? "x2APIC" : "xAPIC", lapic_id(),
            lapic_read(LAPIC_VERSION) & 0xFF);
}

void lapic_init_ap(void) {
    lapic_enable_this_cpu();
}

void lapic_eoi(void) {
    if (x2apic_mode) {
        wrmsr(MSR_X2APIC_FIRST + (LAPIC_EOI >> 4), 0);
    } else if (lapic_mmio) {
        lapic_mmio[LAPIC_EOI / 4] = 0;
    }
}

uint32_t lapic_id(void) {
    uint32_t id = lapic_read(LAPIC_ID);
    return x2apic_mode ? id : (id >> 24) & 0xFF;
}

/* Write the ICR. x2APIC: one 64-bit MSR write, destination in bits 63:32, no
 * delivery-status bit. xAPIC: high half, then low half (which sends), then wait
 * until the APIC accepted it. */
static void lapic_write_icr(uint32_t dest, uint32_t low) {
    if (x2apic_mode) {
        /* x2APIC MSR writes are not serializing: make earlier stores visible to
         * the target before it can take the interrupt */
        __asm__ volatile("mfence; lfence" : : : "memory");
        wrmsr(MSR_X2APIC_FIRST + (LAPIC_ICR_LOW >> 4), ((uint64_t)dest << 32) | low);
        return;
    }
    uint64_t flags = irq_save();
    lapic_mmio[LAPIC_ICR_HIGH / 4] = dest << 24;
    lapic_mmio[LAPIC_ICR_LOW / 4] = low;
    while (lapic_mmio[LAPIC_ICR_LOW / 4] & ICR_SEND_PENDING) {
        __asm__ volatile ("pause");
    }
    irq_restore(flags);
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector) {
    lapic_write_icr(apic_id, vector | ICR_FIXED | ICR_LEVEL_ASSERT);
}

void lapic_send_ipi_all_excluding_self(uint32_t vector) {
    lapic_write_icr(0, vector | ICR_FIXED | ICR_DEST_ALL_EX | ICR_LEVEL_ASSERT);
}

/* ---- Clock events (M1-06) ----
 * TSC-deadline: the LVT is put in deadline mode once per CPU, then each
 * program is one MSR write (fenced: Intel SDM 10.5.4.1, the MSR write may
 * otherwise pass the LVT write). One-shot: divide 16, the count from the TSC
 * delta through apic_per_tsc (APIC ticks per TSC cycle << 32), measured once
 * on the BSP; the APIC timer runs from the same clock on every CPU. */
#define LAPIC_TIMER_MEASURE_NS       10000000ULL   /* 10 ms */
#define LAPIC_TIMER_LVT_MASKED       (1u << 16)
#define LAPIC_TIMER_LVT_TSC_DEADLINE (2u << 17)
#define LAPIC_TIMER_GUESS_HZ         (1000000000ULL / 16)   /* not measurable: a 1 GHz APIC clock */
static uint64_t timer_hz;
static uint64_t apic_per_tsc;
static int deadline_mode;
static void (*timer_fn)(void);

static void lapic_timer_handler(InterruptFrame *frame) {
    (void)frame;
    this_cpu()->irq_stat[PERCPU_STAT_TIMER]++;
    lapic_eoi();   /* first: the handler may switch threads */
    if (timer_fn != NULL) timer_fn();
}

/* APIC timer ticks per second at divide 16, counted against the TSC */
static uint64_t lapic_timer_measure(void) {
    uint64_t wait = tsc_ns_to_cycles(LAPIC_TIMER_MEASURE_NS);
    lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_LVT_MASKED | LAPIC_TIMER_VEC);   /* one-shot, masked */
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);
    uint64_t start = cpu_rdtsc();
    while (cpu_rdtsc() - start < wait) {
        __asm__ volatile("pause");
    }
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
    lapic_write(LAPIC_TIMER_ICR, 0);
    return (uint64_t)elapsed * (1000000000ULL / LAPIC_TIMER_MEASURE_NS);
}

void lapic_timer_setup(void (*fn)(void), int use_deadline) {
    timer_fn = fn;
    isr_register_handler(LAPIC_TIMER_VEC, lapic_timer_handler);
    deadline_mode = use_deadline;
    if (!deadline_mode) {
        timer_hz = lapic_timer_measure();
        if (timer_hz == 0) {
            timer_hz = LAPIC_TIMER_GUESS_HZ;
            kprintf("[LAPIC] WARNING: timer not measured, assuming %lu Hz\n", timer_hz);
        }
        uint64_t tsc_hz = tsc_info()->khz * 1000;
        apic_per_tsc = tsc_hz != 0 ? (timer_hz << 32) / tsc_hz : 0;
    }
    kprintf("[LAPIC] Timer: %s", lapic_timer_mode());
    if (!deadline_mode) kprintf(", %lu Hz at divide 16", timer_hz);
    kprintf(" (vector 0x%x)\n", LAPIC_TIMER_VEC);
}

void lapic_timer_cpu_init(void) {
    if (deadline_mode) {
        lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_LVT_TSC_DEADLINE | LAPIC_TIMER_VEC);
        __asm__ volatile("mfence; lfence" ::: "memory");
        wrmsr(MSR_TSC_DEADLINE, 0);
    } else {
        lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
        lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_VEC);   /* one-shot, unmasked */
        lapic_write(LAPIC_TIMER_ICR, 0);                 /* stopped */
    }
}

void lapic_timer_program(uint64_t tsc) {
    if (deadline_mode) {
        if (tsc == 0) tsc = 1;   /* 0 would disarm it: 1 is in the past, so it fires at once */
        __asm__ volatile("mfence; lfence" ::: "memory");
        wrmsr(MSR_TSC_DEADLINE, tsc);
        return;
    }
    uint64_t now = cpu_rdtsc();
    uint64_t delta = tsc > now ? tsc - now : 0;
    uint64_t count = (uint64_t)(((unsigned __int128)delta * apic_per_tsc) >> 32);
    if (count == 0) count = 1;
    if (count > 0xFFFFFFFF) count = 0xFFFFFFFF;   /* fires early; the timer queue reprograms */
    lapic_write(LAPIC_TIMER_ICR, (uint32_t)count);   /* (re)starts the count */
}

void lapic_timer_stop(void) {
    if (deadline_mode) {
        __asm__ volatile("mfence; lfence" ::: "memory");
        wrmsr(MSR_TSC_DEADLINE, 0);
    } else {
        lapic_write(LAPIC_TIMER_ICR, 0);
    }
}

const char *lapic_timer_mode(void) {
    return deadline_mode ? "TSC-deadline" : "LAPIC one-shot";
}

uint64_t lapic_timer_hz(void) {
    return timer_hz;
}
