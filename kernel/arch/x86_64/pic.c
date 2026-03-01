#include "arch/x86_64/pic.h"
#include "arch/x86_64/io.h"
#include "lib/kprintf.h"

void pic_init(void) {
    /* Save existing masks */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: start initialization sequence (cascade mode, ICW4 needed) */
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    /* ICW2: set vector offsets */
    outb(PIC1_DATA, PIC1_OFFSET);  /* IRQ 0-7  → 32-39 */
    io_wait();
    outb(PIC2_DATA, PIC2_OFFSET);  /* IRQ 8-15 → 40-47 */
    io_wait();

    /* ICW3: tell PIC1 there is a slave at IRQ2, tell PIC2 its cascade identity */
    outb(PIC1_DATA, 0x04);  /* IRQ2 has slave */
    io_wait();
    outb(PIC2_DATA, 0x02);  /* Slave identity = 2 */
    io_wait();

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    /* Mask all IRQs (will be unmasked individually as handlers register) */
    (void)mask1;
    (void)mask2;
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    kprintf("[HAL] PIC remapped (IRQ 0-7 -> %d-%d, IRQ 8-15 -> %d-%d)\n",
            PIC1_OFFSET, PIC1_OFFSET + 7, PIC2_OFFSET, PIC2_OFFSET + 7);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_unmask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t mask = inb(port);
    mask &= ~(1 << irq);
    outb(port, mask);
}

void pic_mask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t mask = inb(port);
    mask |= (1 << irq);
    outb(port, mask);
}

uint16_t pic_get_isr(void) {
    outb(PIC1_COMMAND, PIC_READ_ISR);
    outb(PIC2_COMMAND, PIC_READ_ISR);
    return ((uint16_t)inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND);
}

bool pic_is_spurious(uint8_t irq) {
    uint16_t isr = pic_get_isr();

    if (irq == 7) {
        /* Spurious IRQ 7: check if PIC1 ISR bit 7 is set */
        if (!(isr & (1 << 7))) {
            return true;  /* Spurious — don't send EOI */
        }
    } else if (irq == 15) {
        /* Spurious IRQ 15: check if PIC2 ISR bit 7 is set */
        if (!(isr & (1 << 15))) {
            /* Still need to send EOI to PIC1 (cascade) */
            outb(PIC1_COMMAND, PIC_EOI);
            return true;
        }
    }

    return false;
}
