/* arc_os — Host-side tests for kernel/drivers/bus.c (driver model core, M1-09)
 *
 * The real bus.c runs on the real mutex (test_mutex.c's build of mutex.c). A
 * fake bus matches devices to drivers by a number; drivers record the order
 * their hooks ran in, so binding and the suspend rollback order are checked. */

#include "test_framework.h"
#include <stdint.h>
#include <string.h>

#define ARCHOS_LIB_KPRINTF_H
static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}

#include "../kernel/drivers/bus.c"

typedef struct {
    Driver drv;
    int kind;           /* matches FakeDev.kind */
    int probe_rc;
    int suspend_rc;     /* returned by suspend for the device named fail_on */
    const char *fail_on;
} FakeDrv;

typedef struct {
    Device dev;
    int kind;
} FakeDev;

static char log_buf[256];

static void log_ev(const char *what, const Device *d) {
    strncat(log_buf, what, sizeof(log_buf) - strlen(log_buf) - 1);
    strncat(log_buf, d->name, sizeof(log_buf) - strlen(log_buf) - 1);
    strncat(log_buf, " ", sizeof(log_buf) - strlen(log_buf) - 1);
}

static int fake_match(Device *dev, Driver *drv) {
    return container_of(dev, FakeDev, dev)->kind == container_of(drv, FakeDrv, drv)->kind;
}

static int fake_probe(Device *dev) {
    log_ev("p:", dev);
    return 0;
}

static int fake_probe_fail(Device *dev) {
    log_ev("x:", dev);
    return -5;
}

static int fake_suspend(Device *dev, int how) {
    (void)how;
    FakeDrv *fd = container_of(dev->driver, FakeDrv, drv);
    if (fd->fail_on != NULL && strcmp(fd->fail_on, dev->name) == 0) return fd->suspend_rc;
    log_ev("s:", dev);
    return 0;
}

static int fake_resume(Device *dev, int how) {
    (void)how;
    log_ev("r:", dev);
    return 0;
}

static void fake_remove(Device *dev) {
    log_ev("rm:", dev);
}

/* Fresh state: bus.c keeps a static bus list, which each test resets */
static void reset(void) {
    buses = NULL;
    log_buf[0] = 0;
}

static void init_bus(Bus *b, const char *name) {
    memset(b, 0, sizeof(*b));
    b->name = name;
    b->match = fake_match;
    bus_register(b);
}

static void init_dev(FakeDev *d, const char *name, int kind) {
    memset(d, 0, sizeof(*d));
    d->dev.name = name;
    d->kind = kind;
}

static void init_drv(FakeDrv *d, const char *name, int kind) {
    memset(d, 0, sizeof(*d));
    d->drv.name = name;
    d->drv.probe = fake_probe;
    d->drv.remove = fake_remove;
    d->drv.suspend = fake_suspend;
    d->drv.resume = fake_resume;
    d->kind = kind;
}

/* Devices added before and after the driver both bind; others stay free */
static int test_bus_bind_both_orders(void) {
    reset();
    Bus b;
    init_bus(&b, "fake");
    FakeDev d1, d2, d3;
    FakeDrv drv;
    init_dev(&d1, "a", 1);
    init_dev(&d2, "b", 2);
    init_dev(&d3, "c", 1);
    init_drv(&drv, "one", 1);
    bus_add_device(&b, &d1.dev);
    bus_add_device(&b, &d2.dev);
    ASSERT_EQ(driver_register(&b, &drv.drv), 1);
    ASSERT_TRUE(d1.dev.driver == &drv.drv);
    ASSERT_TRUE(d2.dev.driver == NULL);
    bus_add_device(&b, &d3.dev);
    ASSERT_TRUE(d3.dev.driver == &drv.drv);
    ASSERT_STR_EQ(device_driver_name(&d1.dev), "one");
    ASSERT_TRUE(device_driver_name(&d2.dev) == NULL);
    ASSERT_STR_EQ(log_buf, "p:a p:c ");
    return 0;
}

/* A failed probe leaves the device free for the next driver */
static int test_bus_probe_failure(void) {
    reset();
    Bus b;
    init_bus(&b, "fake");
    FakeDev d;
    FakeDrv bad, good;
    init_dev(&d, "a", 1);
    init_drv(&bad, "bad", 1);
    bad.drv.probe = fake_probe_fail;
    init_drv(&good, "good", 1);
    bus_add_device(&b, &d.dev);
    ASSERT_EQ(driver_register(&b, &bad.drv), 0);
    ASSERT_TRUE(d.dev.driver == NULL);
    ASSERT_EQ(driver_register(&b, &good.drv), 1);
    ASSERT_TRUE(d.dev.driver == &good.drv);
    /* a bound device is not offered to later drivers */
    FakeDrv third;
    init_drv(&third, "third", 1);
    ASSERT_EQ(driver_register(&b, &third.drv), 0);
    ASSERT_STR_EQ(log_buf, "x:a p:a ");
    return 0;
}

/* Unbind runs remove and clears the binding */
static int test_bus_unbind(void) {
    reset();
    Bus b;
    init_bus(&b, "fake");
    FakeDev d;
    FakeDrv drv;
    init_dev(&d, "a", 1);
    init_drv(&drv, "one", 1);
    bus_add_device(&b, &d.dev);
    driver_register(&b, &drv.drv);
    d.dev.driver_data = &d;
    device_unbind(&d.dev);
    ASSERT_TRUE(d.dev.driver == NULL);
    ASSERT_TRUE(d.dev.driver_data == NULL);
    device_unbind(&d.dev);   /* unbound: nothing runs */
    ASSERT_STR_EQ(log_buf, "p:a rm:a ");
    return 0;
}

/* Suspend runs in bus order, resume in reverse; unbound devices are skipped */
static int test_bus_suspend_resume_order(void) {
    reset();
    Bus b1, b2;
    init_bus(&b1, "first");
    init_bus(&b2, "second");   /* newest bus is walked first */
    FakeDev a, c, x, y;
    FakeDrv drv;
    init_dev(&a, "a", 1);
    init_dev(&c, "c", 9);   /* never bound */
    init_dev(&x, "x", 1);
    init_dev(&y, "y", 1);
    init_drv(&drv, "one", 1);
    bus_add_device(&b1, &a.dev);
    bus_add_device(&b1, &c.dev);
    bus_add_device(&b2, &x.dev);
    bus_add_device(&b2, &y.dev);
    driver_register(&b1, &drv.drv);
    FakeDrv drv2;
    init_drv(&drv2, "two", 1);
    driver_register(&b2, &drv2.drv);
    log_buf[0] = 0;
    ASSERT_EQ(bus_suspend_all(PM_SYSTEM), 0);
    ASSERT_STR_EQ(log_buf, "s:x s:y s:a ");
    log_buf[0] = 0;
    ASSERT_EQ(bus_resume_all(PM_SYSTEM), 0);
    ASSERT_STR_EQ(log_buf, "r:a r:y r:x ");
    return 0;
}

/* A refusal resumes exactly the devices already suspended, newest first */
static int test_bus_suspend_rollback(void) {
    reset();
    Bus b1, b2;
    init_bus(&b1, "first");
    init_bus(&b2, "second");
    FakeDev a, b, x, y;
    FakeDrv d1, d2;
    init_dev(&a, "a", 1);
    init_dev(&b, "b", 1);
    init_dev(&x, "x", 2);
    init_dev(&y, "y", 2);
    init_drv(&d1, "one", 1);
    init_drv(&d2, "two", 2);
    d1.fail_on = "b";
    d1.suspend_rc = -16;
    bus_add_device(&b1, &a.dev);
    bus_add_device(&b1, &b.dev);
    bus_add_device(&b2, &x.dev);
    bus_add_device(&b2, &y.dev);
    driver_register(&b1, &d1.drv);
    driver_register(&b2, &d2.drv);
    log_buf[0] = 0;
    ASSERT_EQ(bus_suspend_all(PM_RUNTIME), -16);
    ASSERT_STR_EQ(log_buf, "s:x s:y s:a r:a r:y r:x ");
    return 0;
}

/* A bound driver without suspend refuses (ADR-0017), and the first device is
 * resumed again */
static int test_bus_missing_hook_refuses(void) {
    reset();
    Bus b;
    init_bus(&b, "fake");
    FakeDev a, c;
    FakeDrv ok, nohook;
    init_dev(&a, "a", 1);
    init_dev(&c, "c", 2);
    init_drv(&ok, "ok", 1);
    init_drv(&nohook, "nohook", 2);
    nohook.drv.suspend = NULL;
    bus_add_device(&b, &a.dev);
    bus_add_device(&b, &c.dev);
    driver_register(&b, &ok.drv);
    driver_register(&b, &nohook.drv);
    log_buf[0] = 0;
    ASSERT_EQ(bus_suspend_all(PM_SYSTEM), -EOPNOTSUPP);
    ASSERT_STR_EQ(log_buf, "s:a r:a ");
    return 0;
}

TestCase bus_tests[] = {
    {"bus_bind_both_orders", test_bus_bind_both_orders},
    {"bus_probe_failure", test_bus_probe_failure},
    {"bus_unbind", test_bus_unbind},
    {"bus_suspend_resume_order", test_bus_suspend_resume_order},
    {"bus_suspend_rollback", test_bus_suspend_rollback},
    {"bus_missing_hook_refuses", test_bus_missing_hook_refuses},
};

int bus_test_count = sizeof(bus_tests) / sizeof(bus_tests[0]);
