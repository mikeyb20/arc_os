#include "arch/x86_64/io.h"
#include "arch/x86_64/serial.h"

void serial_init(void) {
    /* Disable all interrupts */
    outb(SERIAL_COM1 + 1, 0x00);

    /* Enable DLAB (set baud rate divisor) */
    outb(SERIAL_COM1 + 3, 0x80);

    /* Set divisor to 12 (lo byte) — 9600 baud (115200 / 12) */
    outb(SERIAL_COM1 + 0, 0x0C);

    /* Divisor high byte */
    outb(SERIAL_COM1 + 1, 0x00);

    /* 8 bits, no parity, 1 stop bit (8N1), DLAB off */
    outb(SERIAL_COM1 + 3, 0x03);

    /* Enable FIFO, clear TX/RX buffers, 14-byte threshold */
    outb(SERIAL_COM1 + 2, 0xC7);

    /* IRQs enabled, RTS/DSR set (MCR) */
    outb(SERIAL_COM1 + 4, 0x0B);
}

void serial_putchar(char c) {
    /* Wait for transmit holding register to be empty (LSR bit 5) */
    while ((inb(SERIAL_COM1 + 5) & 0x20) == 0)
        ;
    outb(SERIAL_COM1, c);
}

void serial_puts(const char *s) {
    while (*s) {
        serial_putchar(*s);
        s++;
    }
}
