#ifndef ARCHOS_ARCH_X86_64_IO_H
#define ARCHOS_ARCH_X86_64_IO_H

#include <stdint.h>

/* Write a byte to an I/O port. */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Read a byte from an I/O port. */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Write a 16-bit word to an I/O port. */
static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

/* Read a 16-bit word from an I/O port. */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Write a 32-bit doubleword to an I/O port. */
static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

/* Read a 32-bit doubleword from an I/O port. */
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Small I/O delay — writes to port 0x80 (POST code port). */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif
