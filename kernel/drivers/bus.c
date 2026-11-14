/* arc_os — Driver model core (M1-09, ADR-0006)
 *
 * Owns: the list of buses, each bus's device and driver lists, and binding
 * (match, then probe). Buses own their devices' memory; drivers are static.
 * Invariants: a device is on exactly one bus list, after any device it sits
 * behind (a bus adds a parent before its children); dev->driver is set only
 * after a successful probe and cleared before remove returns; a driver is
 * probed against a device only while the device is unbound.
 * Power order: suspend runs children before parents (newest bus first, newest
 * device first), resume the exact reverse.
 * Locks: device_lock (sleeping, LOCK_LEVEL_DEVICE) around every list walk,
 * binding, and probe/remove/suspend/resume call.
 * Cost: binding is O(devices x drivers) at registration time only; no hot
 * path walks these lists. */

#include "drivers/bus.h"
#include "proc/mutex.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

static Mutex device_lock = MUTEX_INIT(LOCK_LEVEL_DEVICE);
static Bus *buses;

/* Try drv on dev (unbound, same bus); device_lock held. Returns 1 if bound. */
static int try_bind(Device *dev, Driver *drv) {
    Bus *bus = dev->bus;
    if (dev->driver != NULL || !bus->match(dev, drv)) return 0;
    int rc = bus->probe != NULL ? bus->probe(dev, drv) : (drv->probe != NULL ? drv->probe(dev) : -1);
    if (rc != 0) {
        kprintf("[BUS] %s: %s probe failed (%d)\n", dev->name, drv->name, rc);
        dev->driver_data = NULL;   /* whatever the failed probe left */
        return 0;
    }
    dev->driver = drv;
    kprintf("[BUS] %s: bound to %s\n", dev->name, drv->name);
    return 1;
}

void bus_register(Bus *bus) {
    mutex_lock(&device_lock);
    for (Bus *b = buses; b != NULL; b = b->next) {
        if (b == bus) {   /* twice would loop the list */
            mutex_unlock(&device_lock);
            return;
        }
    }
    bus->devices = bus->devices_tail = NULL;
    bus->drivers = NULL;
    bus->next = buses;
    buses = bus;
    mutex_unlock(&device_lock);
}

void bus_add_device(Bus *bus, Device *dev) {
    mutex_lock(&device_lock);
    dev->bus = bus;
    dev->driver = NULL;
    dev->next = NULL;
    dev->prev = bus->devices_tail;
    if (bus->devices_tail != NULL) {
        bus->devices_tail->next = dev;
    } else {
        bus->devices = dev;
    }
    bus->devices_tail = dev;
    for (Driver *drv = bus->drivers; drv != NULL && dev->driver == NULL; drv = drv->next) {
        try_bind(dev, drv);
    }
    mutex_unlock(&device_lock);
}

int driver_register(Bus *bus, Driver *drv) {
    int bound = 0;
    mutex_lock(&device_lock);
    for (Driver *d = bus->drivers; d != NULL; d = d->next) {
        if (d == drv) {   /* twice would loop the list */
            mutex_unlock(&device_lock);
            return 0;
        }
    }
    drv->bus = bus;
    drv->next = bus->drivers;
    bus->drivers = drv;
    for (Device *dev = bus->devices; dev != NULL; dev = dev->next) {
        bound += try_bind(dev, drv);
    }
    mutex_unlock(&device_lock);
    return bound;
}

void device_unbind(Device *dev) {
    mutex_lock(&device_lock);
    Driver *drv = dev->driver;
    if (drv != NULL) {
        if (dev->bus->remove != NULL) {
            dev->bus->remove(dev);
        } else if (drv->remove != NULL) {
            drv->remove(dev);
        }
        dev->driver = NULL;
        dev->driver_data = NULL;
    }
    mutex_unlock(&device_lock);
}

/* suspend (resume = 0) or resume (resume = 1) one bound device; lock held. A
 * bound driver without the hook refuses (ADR-0017: every driver has both). */
static int pm_call(Device *dev, int how, int resume) {
    Driver *drv = dev->driver;
    if (drv == NULL) return 0;
    int (*fn)(Device *, int) = resume ? drv->resume : drv->suspend;
    return fn != NULL ? fn(dev, how) : -EOPNOTSUPP;
}

/* Resume, oldest first, the devices of a bus from d to its last (the reverse
 * of suspend order); lock held. Returns the first error. */
static int resume_bus_from(Device *d, int how) {
    int rc = 0;
    for (; d != NULL; d = d->next) {
        int r = pm_call(d, how, 1);
        if (r != 0 && rc == 0) rc = r;
    }
    return rc;
}

/* Resume every device of the buses before stop (all buses when stop is NULL),
 * in the reverse of suspend order: oldest bus first, oldest device first; lock
 * held. Bus lists are short (a handful), so the backwards walk is quadratic in
 * buses only. Returns the first error. */
static int resume_buses_before(Bus *stop, int how) {
    int rc = 0;
    for (Bus *end = stop; end != buses;) {
        Bus *b = buses;
        while (b->next != end) b = b->next;
        int r = resume_bus_from(b->devices, how);
        if (r != 0 && rc == 0) rc = r;
        end = b;
    }
    return rc;
}

int bus_suspend_all(int how) {
    mutex_lock(&device_lock);
    for (Bus *bus = buses; bus != NULL; bus = bus->next) {
        for (Device *dev = bus->devices_tail; dev != NULL; dev = dev->prev) {
            int rc = pm_call(dev, how, 0);
            if (rc == 0) continue;
            kprintf("[BUS] %s: suspend refused (%d)\n", dev->name, rc);
            resume_bus_from(dev->next, how);
            resume_buses_before(bus, how);
            mutex_unlock(&device_lock);
            return rc;
        }
    }
    mutex_unlock(&device_lock);
    return 0;
}

int bus_resume_all(int how) {
    mutex_lock(&device_lock);
    int rc = resume_buses_before(NULL, how);
    mutex_unlock(&device_lock);
    return rc;
}
