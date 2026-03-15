/* arc_os — TTY subsystem implementation
 * Line-buffered input with ring buffer for completed lines. */

#include "drivers/tty.h"
#include "arch/x86_64/serial.h"
#include "proc/sched.h"
#include "proc/process.h"
#include "proc/signal.h"
#include "lib/kprintf.h"

/* Ring buffer for completed lines (power-of-2 size) */
#define TTY_BUF_SIZE 256
#define TTY_BUF_MASK (TTY_BUF_SIZE - 1)

/* Max length of a single line being typed */
#define TTY_LINE_MAX 256

/* Read ring buffer — completed lines are copied here */
static char read_buf[TTY_BUF_SIZE];
static volatile uint32_t read_head;  /* IRQ writes here */
static volatile uint32_t read_tail;  /* Process reads from here */

/* Count of complete lines available */
static volatile uint32_t lines_ready;

/* Line buffer — accumulates current line being typed */
static char line_buf[TTY_LINE_MAX];
static uint32_t line_pos;

/* PID of the foreground process (set on tty_read) */
static uint32_t tty_fg_pid;

void tty_init(void) {
    read_head = 0;
    read_tail = 0;
    lines_ready = 0;
    line_pos = 0;
    kprintf("[TTY] Initialized\n");
}

void tty_input_char(char c) {
    /* Ctrl+C — send SIGINT to foreground process */
    if (c == 0x03) {
        if (tty_fg_pid != 0) {
            sig_send(tty_fg_pid, SIGINT);
        }
        serial_putchar('^');
        serial_putchar('C');
        serial_putchar('\n');
        return;
    }

    if (c == '\b' || c == 127) {
        /* Backspace: remove last char from line buffer */
        if (line_pos > 0) {
            line_pos--;
            /* Echo backspace sequence: move back, overwrite with space, move back */
            serial_putchar('\b');
            serial_putchar(' ');
            serial_putchar('\b');
        }
        return;
    }

    if (c == '\n' || c == '\r') {
        /* Newline: copy line buffer + '\n' into read ring buffer */
        for (uint32_t i = 0; i < line_pos; i++) {
            read_buf[read_head & TTY_BUF_MASK] = line_buf[i];
            read_head++;
        }
        read_buf[read_head & TTY_BUF_MASK] = '\n';
        read_head++;
        line_pos = 0;
        lines_ready++;

        /* Echo newline */
        serial_putchar('\n');
        return;
    }

    /* Regular character: append to line buffer if room */
    if (line_pos < TTY_LINE_MAX - 1) {
        line_buf[line_pos++] = c;
        serial_putchar(c);
    }
}

int tty_read(void *buf, uint32_t count) {
    if (count == 0) return 0;

    /* Track foreground process for Ctrl+C */
    Process *cur = proc_current();
    if (cur != NULL) {
        tty_fg_pid = cur->pid;
    }

    /* Yield-loop until a complete line is available */
    while (lines_ready == 0) {
        sched_yield();
    }

    /* Copy from ring buffer up to count bytes, stop after '\n' */
    char *out = (char *)buf;
    uint32_t copied = 0;
    while (copied < count && read_tail != read_head) {
        char ch = read_buf[read_tail & TTY_BUF_MASK];
        read_tail++;
        out[copied++] = ch;
        if (ch == '\n') break;
    }

    /* If we consumed a full line (ended with '\n'), decrement counter */
    if (copied > 0 && out[copied - 1] == '\n') {
        lines_ready--;
    }

    return (int)copied;
}

void tty_set_fg_pid(uint32_t pid) {
    tty_fg_pid = pid;
}

int tty_write(const void *buf, uint32_t count) {
    const char *src = (const char *)buf;
    for (uint32_t i = 0; i < count; i++) {
        serial_putchar(src[i]);
    }
    return (int)count;
}
