/* arc_os — Virtual Terminal subsystem
 *
 * 6 virtual terminals, each with independent screen buffer.
 * Alt+F1-F6 switches between them.
 * Active VT renders to the framebuffer; inactive VTs update buffers only. */

#include "drivers/vt.h"
#include "drivers/fb_console.h"
#include "drivers/font8x16.h"
#include "mm/kmalloc.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

static VirtualTerminal vts[VT_COUNT];
static int active_vt;
static uint32_t screen_cols;
static uint32_t screen_rows;
static int vt_initialized;

void vt_init(uint32_t cols, uint32_t rows) {
    screen_cols = cols;
    screen_rows = rows;
    active_vt = 0;

    for (int i = 0; i < VT_COUNT; i++) {
        size_t buf_size = (size_t)cols * rows * sizeof(VtCell);
        vts[i].buffer = (VtCell *)kmalloc(buf_size, GFP_ZERO);
        if (!vts[i].buffer) {
            kprintf("[VT] Failed to allocate buffer for VT%d\n", i + 1);
            continue;
        }
        vts[i].cur_col = 0;
        vts[i].cur_row = 0;
        vts[i].fg_color = 0xAAAAAA;
        vts[i].bg_color = 0x000000;
        vts[i].ansi_state = 0;
        vts[i].ansi_param_count = 0;
        vts[i].bold = 0;

        /* Initialize all cells to space with default colors */
        for (uint32_t j = 0; j < cols * rows; j++) {
            vts[i].buffer[j].ch = ' ';
            vts[i].buffer[j].fg = 0xAAAAAA;
            vts[i].buffer[j].bg = 0x000000;
        }
    }

    vt_initialized = 1;
    kprintf("[VT] Initialized %d virtual terminals (%lux%lu)\n",
            VT_COUNT, (uint64_t)cols, (uint64_t)rows);
}

/* Redraw the active VT's buffer to the framebuffer */
static void vt_redraw(void) {
    VirtualTerminal *vt = &vts[active_vt];
    if (!vt->buffer) return;

    fb_console_clear();

    for (uint32_t row = 0; row < screen_rows; row++) {
        for (uint32_t col = 0; col < screen_cols; col++) {
            VtCell *cell = &vt->buffer[row * screen_cols + col];
            if (cell->ch != ' ' || cell->bg != 0) {
                fb_console_set_colors(cell->fg, cell->bg);
                fb_console_set_cursor(col, row);
                fb_console_putchar(cell->ch);
            }
        }
    }

    /* Restore cursor position and colors */
    fb_console_set_colors(vt->fg_color, vt->bg_color);
    fb_console_set_cursor(vt->cur_col, vt->cur_row);
}

void vt_switch(int n) {
    if (!vt_initialized) return;
    if (n < 0 || n >= VT_COUNT) return;
    if (n == active_vt) return;

    /* Save current VT cursor from fb_console */
    VirtualTerminal *old = &vts[active_vt];
    fb_console_get_cursor(&old->cur_col, &old->cur_row);

    active_vt = n;

    /* Redraw new VT */
    vt_redraw();

    kprintf("[VT] Switched to VT%d\n", n + 1);
}

int vt_active(void) {
    return active_vt;
}

/* Store a character in the VT buffer at current cursor position */
static void vt_buffer_putchar(VirtualTerminal *vt, char c) {
    if (!vt->buffer) return;

    if (c == '\n') {
        vt->cur_col = 0;
        vt->cur_row++;
        if (vt->cur_row >= screen_rows) {
            /* Scroll buffer up */
            memmove(vt->buffer, vt->buffer + screen_cols,
                    (size_t)(screen_rows - 1) * screen_cols * sizeof(VtCell));
            /* Clear last row */
            for (uint32_t c2 = 0; c2 < screen_cols; c2++) {
                VtCell *cell = &vt->buffer[(screen_rows - 1) * screen_cols + c2];
                cell->ch = ' ';
                cell->fg = vt->fg_color;
                cell->bg = vt->bg_color;
            }
            vt->cur_row = screen_rows - 1;
        }
        return;
    }

    if (c == '\r') { vt->cur_col = 0; return; }
    if (c == '\b') {
        if (vt->cur_col > 0) vt->cur_col--;
        return;
    }
    if (c == '\t') {
        vt->cur_col = (vt->cur_col + 8) & ~7u;
        if (vt->cur_col >= screen_cols) {
            vt->cur_col = 0;
            vt->cur_row++;
            if (vt->cur_row >= screen_rows) vt->cur_row = screen_rows - 1;
        }
        return;
    }

    if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) {
        VtCell *cell = &vt->buffer[vt->cur_row * screen_cols + vt->cur_col];
        cell->ch = (uint8_t)c;
        cell->fg = vt->fg_color;
        cell->bg = vt->bg_color;
        vt->cur_col++;
        if (vt->cur_col >= screen_cols) {
            vt->cur_col = 0;
            vt->cur_row++;
            if (vt->cur_row >= screen_rows) {
                memmove(vt->buffer, vt->buffer + screen_cols,
                        (size_t)(screen_rows - 1) * screen_cols * sizeof(VtCell));
                for (uint32_t c2 = 0; c2 < screen_cols; c2++) {
                    VtCell *cell2 = &vt->buffer[(screen_rows - 1) * screen_cols + c2];
                    cell2->ch = ' ';
                    cell2->fg = vt->fg_color;
                    cell2->bg = vt->bg_color;
                }
                vt->cur_row = screen_rows - 1;
            }
        }
    }
}

void vt_putchar(int vt_idx, char c) {
    if (!vt_initialized || vt_idx < 0 || vt_idx >= VT_COUNT) return;
    vt_buffer_putchar(&vts[vt_idx], c);
}
