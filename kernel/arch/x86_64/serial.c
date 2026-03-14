#include "arch/x86_64/io.h"
#include "arch/x86_64/serial.h"

void serial_init(void) {
    /* Disable all interrupts */
    outb(SERIAL_COM1 + SERIAL_REG_IER, 0x00);

    /* Enable DLAB (set baud rate divisor) */
    outb(SERIAL_COM1 + SERIAL_REG_LCR, SERIAL_LCR_DLAB);

    /* Set divisor to 12 (lo byte) — 9600 baud */
    outb(SERIAL_COM1 + SERIAL_REG_DLL, SERIAL_BAUD_9600);

    /* Divisor high byte */
    outb(SERIAL_COM1 + SERIAL_REG_DLH, 0x00);

    /* 8 bits, no parity, 1 stop bit (8N1), DLAB off */
    outb(SERIAL_COM1 + SERIAL_REG_LCR, SERIAL_LCR_8N1);

    /* Enable FIFO, clear TX/RX buffers, 14-byte threshold */
    outb(SERIAL_COM1 + SERIAL_REG_FCR, SERIAL_FCR_ENABLE);

    /* IRQs enabled, RTS/DSR set (MCR) */
    outb(SERIAL_COM1 + SERIAL_REG_MCR, SERIAL_MCR_RTS);
}

void serial_putchar(char c) {
    /* Wait for transmit holding register to be empty */
    while ((inb(SERIAL_COM1 + SERIAL_REG_LSR) & SERIAL_LSR_THRE) == 0)
        ;
    outb(SERIAL_COM1 + SERIAL_REG_DATA, c);
}

void serial_puts(const char *s) {
    while (*s) {
        serial_putchar(*s);
        s++;
    }
}
