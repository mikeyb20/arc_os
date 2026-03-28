#ifndef ARCHOS_DRIVERS_VT_H
#define ARCHOS_DRIVERS_VT_H

#include <stdint.h>

/* Number of virtual terminals */
#define VT_COUNT 6

/* Character cell in VT screen buffer */
typedef struct {
    uint8_t  ch;
    uint32_t fg;
    uint32_t bg;
} VtCell;

/* Virtual terminal state */
typedef struct {
    VtCell  *buffer;      /* Screen buffer (rows * cols) */
    uint32_t cur_col;
    uint32_t cur_row;
    uint32_t fg_color;
    uint32_t bg_color;
    int      ansi_state;  /* ANSI parser state for this VT */
    int      ansi_params[8];
    int      ansi_param_count;
    int      bold;
} VirtualTerminal;

/* Initialize virtual terminal subsystem. Call after fb_console_init. */
void vt_init(uint32_t cols, uint32_t rows);

/* Switch to VT n (0-5). Called from keyboard handler on Alt+F1-F6. */
void vt_switch(int n);

/* Get the currently active VT index. */
int vt_active(void);

/* Write a character to VT n's buffer. If n is the active VT, also renders. */
void vt_putchar(int vt, char c);

#endif /* ARCHOS_DRIVERS_VT_H */
