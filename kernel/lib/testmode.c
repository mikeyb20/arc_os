#include "lib/testmode.h"
#include "lib/cmdline.h"
#include "lib/kprintf.h"
#include "arch/x86_64/debug_exit.h"

static bool g_active;
static char g_suite[TESTMODE_SUITE_MAX];
static int  g_init_pid = -1;
static void (*g_end_hook)(void);

void testmode_init(const char *line) {
    g_active = false;
    g_suite[0] = '\0';
    if (cmdline_get(line, "arc.test", g_suite, sizeof(g_suite)) && g_suite[0] != '\0') {
        g_active = true;
    }
}

bool testmode_active(void) {
    return g_active;
}

const char *testmode_suite(void) {
    return g_suite;
}

void testmode_set_init_pid(int pid) {
    g_init_pid = pid;
}

void testmode_set_end_hook(void (*fn)(void)) {
    g_end_hook = fn;
}

void testmode_process_exited(int pid, int status) {
    if (!g_active || pid != g_init_pid) return;
    if (g_end_hook != NULL) g_end_hook();
    int code = status;
    if (code < 0 || code > 0x3e) code = 0x3e;
    kprintf("[TEST] init (pid %d) exited with status %d; ending QEMU run\n", pid, status);
    testmode_exit(code);
}

void testmode_exit(int code) {
    hal_debug_exit((uint32_t)code & 0x7f);
    /* No isa-debug-exit device (real hardware, plain QEMU): just stop. */
    for (;;) __asm__ volatile ("cli; hlt");
}
