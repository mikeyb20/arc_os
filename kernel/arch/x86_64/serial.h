#ifndef ARCHOS_ARCH_X86_64_SERIAL_H
#define ARCHOS_ARCH_X86_64_SERIAL_H

#define SERIAL_COM1 0x3F8

/* 8250/16550 UART register offsets (relative to COM base port) */
#define SERIAL_REG_DATA   0   /* Data (DLAB=0) */
#define SERIAL_REG_DLL    0   /* Divisor Latch Low (DLAB=1) */
#define SERIAL_REG_IER    1   /* Interrupt Enable (DLAB=0) */
#define SERIAL_REG_DLH    1   /* Divisor Latch High (DLAB=1) */
#define SERIAL_REG_FCR    2   /* FIFO Control */
#define SERIAL_REG_LCR    3   /* Line Control */
#define SERIAL_REG_MCR    4   /* Modem Control */
#define SERIAL_REG_LSR    5   /* Line Status */

/* Register values */
#define SERIAL_LSR_THRE   0x20  /* Transmit Holding Register Empty */
#define SERIAL_LCR_DLAB   0x80  /* Divisor Latch Access Bit */
#define SERIAL_LCR_8N1    0x03  /* 8 data bits, no parity, 1 stop bit */
#define SERIAL_FCR_ENABLE 0xC7  /* Enable FIFO, clear buffers, 14-byte threshold */
#define SERIAL_MCR_RTS    0x0B  /* RTS/DSR set, IRQs enabled */
#define SERIAL_BAUD_9600  12    /* Divisor for 9600 baud (115200 / 12) */

/* Initialize COM1: 9600 baud, 8N1, FIFO enabled. */
void serial_init(void);

/* Write a single character to COM1 (blocks until transmitter ready). */
void serial_putchar(char c);

/* Write a null-terminated string to COM1. */
void serial_puts(const char *s);

#endif
