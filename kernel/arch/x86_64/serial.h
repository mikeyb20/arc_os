#ifndef ARCHOS_ARCH_X86_64_SERIAL_H
#define ARCHOS_ARCH_X86_64_SERIAL_H

#define SERIAL_COM1 0x3F8

/* Initialize COM1: 9600 baud, 8N1, FIFO enabled. */
void serial_init(void);

/* Write a single character to COM1 (blocks until transmitter ready). */
void serial_putchar(char c);

/* Write a null-terminated string to COM1. */
void serial_puts(const char *s);

#endif
