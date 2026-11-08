#include "arch/x86_64/debug_exit.h"
#include "arch/x86_64/io.h"

void hal_debug_exit(uint32_t value) {
    outl(DEBUG_EXIT_PORT, value);
}
