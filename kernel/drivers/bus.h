#ifndef ARCHOS_DRIVERS_BUS_H
#define ARCHOS_DRIVERS_BUS_H

/* Driver model (M1-09, ADR-0006).
 *
 * A bus (PCI now, USB in M4-02) enumerates devices and holds the drivers
 * registered for it. Whenever a device or a driver is added, the bus's match
 * function pairs them and the driver's probe runs; probe returning 0 binds
 * the device to that driver. A driver also has remove, suspend and resume
 * (ADR-0017); a driver that cannot suspend yet returns -EOPNOTSUPP.
 *
 * Locking: one sleeping lock (LOCK_LEVEL_DEVICE) guards every bus list and
 * binding. probe, remove, suspend and resume run under it and may sleep; they
 * must not register drivers or devices themselves. */

#include <stdint.h>
#include <stddef.h>

typedef struct Bus Bus;
typedef struct Device Device;
typedef struct Driver Driver;
struct IrqDesc;

#ifndef container_of
/* The struct of type `type` whose member `member` is at ptr. */
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Why suspend/resume run (second argument) */
#define PM_SYSTEM  0   /* the whole system sleeps or wakes (S3 / s2idle; later) */
#define PM_RUNTIME 1   /* this device idles while the system runs (link power, APST) */

struct Driver {
    const char *name;
    Bus *bus;                                /* set by driver_register */
    int (*probe)(Device *dev);               /* 0 binds; -errno leaves the device free */
    void (*remove)(Device *dev);
    int (*suspend)(Device *dev, int how);    /* 0, or -errno (-EOPNOTSUPP: not yet) */
    int (*resume)(Device *dev, int how);
    Driver *next;                            /* on bus->drivers */
};

struct Device {
    const char *name;            /* stable name from the bus ("0000:00:04.0") */
    Bus *bus;
    Driver *driver;              /* NULL while unbound */
    void *driver_data;           /* the bound driver's state */
    uint64_t dma_mask;           /* highest bus address the device reaches (dma.h); bus default 64-bit */
    struct IrqDesc **irqs;       /* interrupts the bus gave the device (irq.h) */
    uint16_t nr_irqs;
    Device *parent;              /* the device this one sits behind (a bridge, a USB host); NULL at the root.
                                    Set by the bus before bus_add_device; nothing sets it yet (M4-02) */
    Device *next, *prev;         /* on bus->devices */
};

struct Bus {
    const char *name;
    int (*match)(Device *dev, Driver *drv);   /* 1 if drv can drive dev */
    int (*probe)(Device *dev, Driver *drv);   /* typed probe wrapper; NULL = drv->probe */
    void (*remove)(Device *dev);              /* typed remove wrapper; NULL = drv->remove */
    Device *devices, *devices_tail;           /* in the order the bus found them */
    Driver *drivers;
    Bus *next;
};

/* Make a bus known (its lists start empty; a second call does nothing). */
void bus_register(Bus *bus);

/* Add a device the bus found and offer it to every registered driver. */
void bus_add_device(Bus *bus, Device *dev);

/* Register a driver on bus and offer it every unbound device. Returns how
 * many devices it bound. */
int driver_register(Bus *bus, Driver *drv);

/* Unbind dev from its driver (remove runs). */
void device_unbind(Device *dev);

/* Call suspend of every bound driver, children before parents: the newest bus
 * first, each bus's devices newest first (a bus adds a parent before what sits
 * behind it). Resume runs the exact reverse. Returns 0, or the first error;
 * suspend stops there and resumes the devices already suspended. */
int bus_suspend_all(int how);
int bus_resume_all(int how);

/* The bound driver's name, or NULL. */
static inline const char *device_driver_name(const Device *dev) {
    return dev->driver != NULL ? dev->driver->name : NULL;
}

#endif /* ARCHOS_DRIVERS_BUS_H */
