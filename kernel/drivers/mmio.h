#ifndef ARCHOS_DRIVERS_MMIO_H
#define ARCHOS_DRIVERS_MMIO_H

/* Device memory (M1-09): uncached mappings of device registers and the
 * accessors drivers read and write them with. */

#include <stdint.h>

/* Map size bytes of device memory at physical address phys, uncached, in the
 * kernel's MMIO window. Returns the address of phys (page offset kept), or
 * NULL when the window is full. Never unmapped; thread context. */
volatile void *mmio_map(uint64_t phys, uint64_t size);

/* Bytes of the MMIO window in use. */
uint64_t mmio_window_used(void);

static inline uint8_t mmio_read8(const volatile void *addr) {
    return *(const volatile uint8_t *)addr;
}
static inline uint16_t mmio_read16(const volatile void *addr) {
    return *(const volatile uint16_t *)addr;
}
static inline uint32_t mmio_read32(const volatile void *addr) {
    return *(const volatile uint32_t *)addr;
}
static inline uint64_t mmio_read64(const volatile void *addr) {
    return *(const volatile uint64_t *)addr;
}
static inline void mmio_write8(volatile void *addr, uint8_t v) {
    *(volatile uint8_t *)addr = v;
}
static inline void mmio_write16(volatile void *addr, uint16_t v) {
    *(volatile uint16_t *)addr = v;
}
static inline void mmio_write32(volatile void *addr, uint32_t v) {
    *(volatile uint32_t *)addr = v;
}
static inline void mmio_write64(volatile void *addr, uint64_t v) {
    *(volatile uint64_t *)addr = v;
}

#endif /* ARCHOS_DRIVERS_MMIO_H */
