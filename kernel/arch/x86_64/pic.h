#ifndef ARCHOS_ARCH_X86_64_PIC_H
#define ARCHOS_ARCH_X86_64_PIC_H

#include <stdint.h>
#include <stdbool.h>

/* 8259 PIC I/O ports */
#define PIC1_COMMAND  0x20
#define PIC1_DATA     0x21
#define PIC2_COMMAND  0xA0
#define PIC2_DATA     0xA1

/* PIC commands */
#define PIC_EOI       0x20  /* End of interrupt */
#define PIC_READ_ISR  0x0B  /* Read In-Service Register */

/* IRQ remapping offsets */
#define PIC1_OFFSET   32    /* IRQ 0-7  → vectors 32-39 */
#define PIC2_OFFSET   40    /* IRQ 8-15 → vectors 40-47 */

/* Initialize both PICs and remap IRQs to vectors 32-47. All IRQs masked. */
void pic_init(void);

/* Send End-Of-Interrupt for the given IRQ (0-15). */
void pic_send_eoi(uint8_t irq);

/* Unmask (enable) a specific IRQ line. */
void pic_unmask(uint8_t irq);

/* Mask (disable) a specific IRQ line. */
void pic_mask(uint8_t irq);

/* Read the ISR (In-Service Register) to detect spurious IRQs. */
uint16_t pic_get_isr(void);

/* Check if an IRQ is spurious. Returns true if spurious (no EOI needed). */
bool pic_is_spurious(uint8_t irq);

#endif /* ARCHOS_ARCH_X86_64_PIC_H */
