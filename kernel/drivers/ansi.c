/* arc_os — ANSI escape code parser
 *
 * State machine: NORMAL → ESC → CSI → (dispatch)
 * Supports cursor movement, erase, SGR colors (16 + bright). */

#include "drivers/ansi.h"
#include "drivers/fb_console.h"

/* Parser states */
#define STATE_NORMAL 0
#define STATE_ESC    1
#define STATE_CSI    2

/* Max params in a CSI sequence (e.g., ESC[1;31m → 2 params) */
#define MAX_PARAMS   8

static int state;
static int params[MAX_PARAMS];
static int param_count;

/* Standard 8-color palette (0xRRGGBB) */
static const uint32_t ansi_colors[8] = {
    0x000000,  /* 0: Black */
    0xAA0000,  /* 1: Red */
    0x00AA00,  /* 2: Green */
    0xAA5500,  /* 3: Yellow/Brown */
    0x0000AA,  /* 4: Blue */
    0xAA00AA,  /* 5: Magenta */
    0x00AAAA,  /* 6: Cyan */
    0xAAAAAA,  /* 7: White */
};

/* Bright colors */
static const uint32_t ansi_bright[8] = {
    0x555555,  /* 0: Bright Black (Gray) */
    0xFF5555,  /* 1: Bright Red */
    0x55FF55,  /* 2: Bright Green */
    0xFFFF55,  /* 3: Bright Yellow */
    0x5555FF,  /* 4: Bright Blue */
    0xFF55FF,  /* 5: Bright Magenta */
    0x55FFFF,  /* 6: Bright Cyan */
    0xFFFFFF,  /* 7: Bright White */
};

/* Default colors */
#define DEFAULT_FG 0xAAAAAA
#define DEFAULT_BG 0x000000

static uint32_t cur_fg = DEFAULT_FG;
static uint32_t cur_bg = DEFAULT_BG;
static int bold;

void ansi_init(void) {
    state = STATE_NORMAL;
    param_count = 0;
    bold = 0;
    cur_fg = DEFAULT_FG;
    cur_bg = DEFAULT_BG;
}

void ansi_reset(void) {
    ansi_init();
}

static int get_param(int idx, int def) {
    if (idx < param_count && params[idx] > 0) return params[idx];
    return def;
}

/* Process SGR (Select Graphic Rendition) — ESC[n;...m */
static void handle_sgr(void) {
    if (param_count == 0) {
        /* ESC[m = reset */
        cur_fg = DEFAULT_FG;
        cur_bg = DEFAULT_BG;
        bold = 0;
        fb_console_set_colors(cur_fg, cur_bg);
        return;
    }

    for (int i = 0; i < param_count; i++) {
        int p = params[i];
        if (p == 0) {
            cur_fg = DEFAULT_FG;
            cur_bg = DEFAULT_BG;
            bold = 0;
        } else if (p == 1) {
            bold = 1;
            /* If current fg is a normal color, brighten it */
        } else if (p >= 30 && p <= 37) {
            cur_fg = bold ? ansi_bright[p - 30] : ansi_colors[p - 30];
        } else if (p == 39) {
            cur_fg = DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            cur_bg = ansi_colors[p - 40];
        } else if (p == 49) {
            cur_bg = DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            cur_fg = ansi_bright[p - 90];
        } else if (p >= 100 && p <= 107) {
            cur_bg = ansi_bright[p - 100];
        }
    }
    fb_console_set_colors(cur_fg, cur_bg);
}

/* Dispatch a CSI sequence based on the final character */
static void dispatch_csi(char final) {
    uint32_t cur_col, cur_row;
    fb_console_get_cursor(&cur_col, &cur_row);

    switch (final) {
    case 'A': /* Cursor Up */
        {
            int n = get_param(0, 1);
            if ((int)cur_row >= n) cur_row -= (uint32_t)n;
            else cur_row = 0;
            fb_console_set_cursor(cur_col, cur_row);
        }
        break;
    case 'B': /* Cursor Down */
        {
            int n = get_param(0, 1);
            fb_console_set_cursor(cur_col, cur_row + (uint32_t)n);
        }
        break;
    case 'C': /* Cursor Forward */
        {
            int n = get_param(0, 1);
            fb_console_set_cursor(cur_col + (uint32_t)n, cur_row);
        }
        break;
    case 'D': /* Cursor Back */
        {
            int n = get_param(0, 1);
            if ((int)cur_col >= n) cur_col -= (uint32_t)n;
            else cur_col = 0;
            fb_console_set_cursor(cur_col, cur_row);
        }
        break;
    case 'H': /* Cursor Position */
    case 'f':
        {
            int row = get_param(0, 1) - 1;
            int col = get_param(1, 1) - 1;
            if (row < 0) row = 0;
            if (col < 0) col = 0;
            fb_console_set_cursor((uint32_t)col, (uint32_t)row);
        }
        break;
    case 'J': /* Erase Display */
        {
            int mode = get_param(0, 0);
            fb_console_erase_display(mode);
        }
        break;
    case 'K': /* Erase Line */
        {
            int mode = get_param(0, 0);
            fb_console_erase_line(mode);
        }
        break;
    case 'm': /* SGR — color/attribute */
        handle_sgr();
        break;
    default:
        /* Unknown CSI sequence — ignore */
        break;
    }
}

void ansi_putchar(char c) {
    switch (state) {
    case STATE_NORMAL:
        if (c == '\033') {
            state = STATE_ESC;
        } else {
            fb_console_putchar(c);
        }
        break;

    case STATE_ESC:
        if (c == '[') {
            state = STATE_CSI;
            param_count = 0;
            for (int i = 0; i < MAX_PARAMS; i++) params[i] = 0;
        } else {
            /* Unknown escape — discard and return to normal */
            state = STATE_NORMAL;
        }
        break;

    case STATE_CSI:
        if (c >= '0' && c <= '9') {
            /* Accumulate parameter digit */
            if (param_count == 0) param_count = 1;
            params[param_count - 1] = params[param_count - 1] * 10 + (c - '0');
        } else if (c == ';') {
            /* Next parameter */
            if (param_count < MAX_PARAMS) param_count++;
        } else {
            /* Final character — dispatch */
            if (param_count == 0 && c != 'm') param_count = 0;
            dispatch_csi(c);
            state = STATE_NORMAL;
        }
        break;
    }
}
