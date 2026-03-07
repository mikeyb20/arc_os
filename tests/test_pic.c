/* arc_os — Host-side tests for kernel/arch/x86_64/pic.c */

#include "test_framework.h"
#include <stdint.h>
#include <stdbool.h>

/* Guard kernel headers */
#define ARCHOS_ARCH_X86_64_IO_H   /* Has inline asm (inb/outb) */
#define ARCHOS_LIB_KPRINTF_H

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Mock I/O system */
typedef struct {
    uint16_t port;
    uint8_t  value;
} IoWrite;

#define IO_LOG_SIZE 256
static IoWrite io_log[IO_LOG_SIZE];
static int io_log_count;

/* Controllable inb return values — indexed by port & 0xFF */
static uint8_t inb_return[256];

static void outb(uint16_t port, uint8_t value) {
    if (io_log_count < IO_LOG_SIZE) {
        io_log[io_log_count].port = port;
        io_log[io_log_count].value = value;
        io_log_count++;
    }
}

static uint8_t inb(uint16_t port) {
    return inb_return[port & 0xFF];
}

static void io_wait(void) { /* no-op */ }

/* Include PIC header for constants (it doesn't have asm) */
#include "../kernel/arch/x86_64/pic.h"

/* Include the real pic.c */
#include "../kernel/arch/x86_64/pic.c"

/* --- Helpers --- */

static void reset_io(void) {
    memset(io_log, 0, sizeof(io_log));
    io_log_count = 0;
    memset(inb_return, 0, sizeof(inb_return));
}

/* Check if a specific (port, value) pair was written */
static bool io_was_written(uint16_t port, uint8_t value) {
    for (int i = 0; i < io_log_count; i++) {
        if (io_log[i].port == port && io_log[i].value == value)
            return true;
    }
    return false;
}

/* Find the last value written to a port, returns true if found */
static bool io_find_last_write(uint16_t port, uint8_t *out_value) {
    for (int i = io_log_count - 1; i >= 0; i--) {
        if (io_log[i].port == port) {
            *out_value = io_log[i].value;
            return true;
        }
    }
    return false;
}

/* Count writes to a port */
static int io_count_writes(uint16_t port) {
    int count = 0;
    for (int i = 0; i < io_log_count; i++) {
        if (io_log[i].port == port) count++;
    }
    return count;
}

/* --- Tests --- */

TEST(init_remaps_irqs) {
    reset_io();
    /* inb for saving existing masks — return 0xFF */
    inb_return[PIC1_DATA & 0xFF] = 0xFF;
    inb_return[PIC2_DATA & 0xFF] = 0xFF;

    pic_init();

    /* ICW1 to both PICs */
    ASSERT_TRUE(io_was_written(PIC1_COMMAND, 0x11));
    ASSERT_TRUE(io_was_written(PIC2_COMMAND, 0x11));

    /* ICW2: vector offsets */
    ASSERT_TRUE(io_was_written(PIC1_DATA, PIC1_OFFSET));
    ASSERT_TRUE(io_was_written(PIC2_DATA, PIC2_OFFSET));

    /* ICW3: cascade */
    ASSERT_TRUE(io_was_written(PIC1_DATA, 0x04));
    ASSERT_TRUE(io_was_written(PIC2_DATA, 0x02));

    /* ICW4: 8086 mode */
    ASSERT_TRUE(io_was_written(PIC1_DATA, 0x01));
    ASSERT_TRUE(io_was_written(PIC2_DATA, 0x01));

    /* Final masks = 0xFF (all masked) */
    uint8_t final_mask1, final_mask2;
    ASSERT_TRUE(io_find_last_write(PIC1_DATA, &final_mask1));
    ASSERT_TRUE(io_find_last_write(PIC2_DATA, &final_mask2));
    ASSERT_EQ(final_mask1, 0xFF);
    ASSERT_EQ(final_mask2, 0xFF);
    return 0;
}

TEST(send_eoi_irq_low) {
    reset_io();
    pic_send_eoi(0);
    /* Only PIC1 EOI, no PIC2 */
    ASSERT_TRUE(io_was_written(PIC1_COMMAND, PIC_EOI));
    ASSERT_EQ(io_count_writes(PIC2_COMMAND), 0);
    return 0;
}

TEST(send_eoi_irq_high) {
    reset_io();
    pic_send_eoi(8);
    /* Both PICs get EOI */
    ASSERT_TRUE(io_was_written(PIC2_COMMAND, PIC_EOI));
    ASSERT_TRUE(io_was_written(PIC1_COMMAND, PIC_EOI));
    return 0;
}

TEST(send_eoi_irq7) {
    reset_io();
    pic_send_eoi(7);
    /* IRQ 7 < 8, so only PIC1 */
    ASSERT_TRUE(io_was_written(PIC1_COMMAND, PIC_EOI));
    ASSERT_EQ(io_count_writes(PIC2_COMMAND), 0);
    return 0;
}

TEST(send_eoi_irq15) {
    reset_io();
    pic_send_eoi(15);
    /* IRQ 15 >= 8, so both PICs */
    ASSERT_TRUE(io_was_written(PIC2_COMMAND, PIC_EOI));
    ASSERT_TRUE(io_was_written(PIC1_COMMAND, PIC_EOI));
    return 0;
}

TEST(unmask_low_irq) {
    reset_io();
    /* inb(PIC1_DATA) returns 0xFF (all masked) */
    inb_return[PIC1_DATA & 0xFF] = 0xFF;
    pic_unmask(3);
    /* Should write 0xFF & ~(1<<3) = 0xF7 to PIC1_DATA */
    ASSERT_TRUE(io_was_written(PIC1_DATA, 0xF7));
    return 0;
}

TEST(unmask_high_irq) {
    reset_io();
    /* inb(PIC2_DATA) returns 0xFF */
    inb_return[PIC2_DATA & 0xFF] = 0xFF;
    pic_unmask(10);  /* IRQ 10 = PIC2 bit 2 */
    /* Should write 0xFF & ~(1<<2) = 0xFB to PIC2_DATA */
    ASSERT_TRUE(io_was_written(PIC2_DATA, 0xFB));
    return 0;
}

TEST(mask_low_irq) {
    reset_io();
    inb_return[PIC1_DATA & 0xFF] = 0x00;  /* All unmasked */
    pic_mask(5);
    /* Should write 0x00 | (1<<5) = 0x20 to PIC1_DATA */
    ASSERT_TRUE(io_was_written(PIC1_DATA, 0x20));
    return 0;
}

TEST(mask_high_irq) {
    reset_io();
    inb_return[PIC2_DATA & 0xFF] = 0x00;
    pic_mask(12);  /* IRQ 12 = PIC2 bit 4 */
    /* Should write 0x00 | (1<<4) = 0x10 to PIC2_DATA */
    ASSERT_TRUE(io_was_written(PIC2_DATA, 0x10));
    return 0;
}

TEST(is_spurious_irq7_real) {
    reset_io();
    /* PIC1 ISR bit 7 set → real IRQ 7 */
    inb_return[PIC1_COMMAND & 0xFF] = 0x80;  /* bit 7 set */
    inb_return[PIC2_COMMAND & 0xFF] = 0x00;
    ASSERT_FALSE(pic_is_spurious(7));
    return 0;
}

TEST(is_spurious_irq7_spurious) {
    reset_io();
    /* PIC1 ISR bit 7 clear → spurious */
    inb_return[PIC1_COMMAND & 0xFF] = 0x00;  /* bit 7 clear */
    inb_return[PIC2_COMMAND & 0xFF] = 0x00;
    ASSERT_TRUE(pic_is_spurious(7));
    /* No EOI should be sent for spurious IRQ 7 */
    ASSERT_FALSE(io_was_written(PIC1_COMMAND, PIC_EOI));
    return 0;
}

TEST(is_spurious_irq15_real) {
    reset_io();
    /* PIC2 ISR bit 7 set → real IRQ 15 */
    inb_return[PIC1_COMMAND & 0xFF] = 0x00;
    inb_return[PIC2_COMMAND & 0xFF] = 0x80;  /* bit 7 set */
    ASSERT_FALSE(pic_is_spurious(15));
    return 0;
}

TEST(is_spurious_irq15_spurious) {
    reset_io();
    /* PIC2 ISR bit 7 clear → spurious, but cascade EOI to PIC1 */
    inb_return[PIC1_COMMAND & 0xFF] = 0x00;
    inb_return[PIC2_COMMAND & 0xFF] = 0x00;  /* bit 7 clear */
    ASSERT_TRUE(pic_is_spurious(15));
    /* Should send EOI to PIC1 (cascade) but not PIC2 */
    ASSERT_TRUE(io_was_written(PIC1_COMMAND, PIC_EOI));
    return 0;
}

TEST(is_spurious_normal_irq) {
    reset_io();
    /* IRQ 5 — not 7 or 15, so never spurious */
    inb_return[PIC1_COMMAND & 0xFF] = 0x00;
    inb_return[PIC2_COMMAND & 0xFF] = 0x00;
    ASSERT_FALSE(pic_is_spurious(5));
    return 0;
}

TEST(get_isr) {
    reset_io();
    inb_return[PIC1_COMMAND & 0xFF] = 0x42;
    inb_return[PIC2_COMMAND & 0xFF] = 0x81;

    uint16_t isr = pic_get_isr();
    /* Combined: (PIC2 << 8) | PIC1 = 0x8142 */
    ASSERT_EQ(isr, 0x8142);
    return 0;
}

TEST(unmask_preserves_others) {
    reset_io();
    /* Start with only bit 3 masked */
    inb_return[PIC1_DATA & 0xFF] = 0x08;
    pic_unmask(0);  /* Clear bit 0 */
    /* Should write 0x08 & ~(1<<0) = 0x08 (bit 0 already clear) */
    ASSERT_TRUE(io_was_written(PIC1_DATA, 0x08));
    return 0;
}

TEST(mask_preserves_others) {
    reset_io();
    /* Start with bits 1 and 3 set */
    inb_return[PIC1_DATA & 0xFF] = 0x0A;
    pic_mask(5);  /* Set bit 5 */
    /* Should write 0x0A | (1<<5) = 0x2A */
    ASSERT_TRUE(io_was_written(PIC1_DATA, 0x2A));
    return 0;
}

/* --- Test suite export --- */

TestCase pic_tests[] = {
    TEST_ENTRY(init_remaps_irqs),
    TEST_ENTRY(send_eoi_irq_low),
    TEST_ENTRY(send_eoi_irq_high),
    TEST_ENTRY(send_eoi_irq7),
    TEST_ENTRY(send_eoi_irq15),
    TEST_ENTRY(unmask_low_irq),
    TEST_ENTRY(unmask_high_irq),
    TEST_ENTRY(mask_low_irq),
    TEST_ENTRY(mask_high_irq),
    TEST_ENTRY(is_spurious_irq7_real),
    TEST_ENTRY(is_spurious_irq7_spurious),
    TEST_ENTRY(is_spurious_irq15_real),
    TEST_ENTRY(is_spurious_irq15_spurious),
    TEST_ENTRY(is_spurious_normal_irq),
    TEST_ENTRY(get_isr),
    TEST_ENTRY(unmask_preserves_others),
    TEST_ENTRY(mask_preserves_others),
};

int pic_test_count = sizeof(pic_tests) / sizeof(pic_tests[0]);
