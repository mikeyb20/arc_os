#ifndef ARCHOS_UAPI_TERMIOS_H
#define ARCHOS_UAPI_TERMIOS_H

#include <stdint.h>

/* Terminal ioctls and their structs, Linux x86_64 values (ADR-0001). */

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

_Static_assert(sizeof(struct winsize) == 8, "struct winsize must be 8 bytes");

/* The kernel's struct termios (TCGETS/TCSETS), not glibc's larger one. */
#define ARC_NCCS 19

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[ARC_NCCS];
};

_Static_assert(sizeof(struct termios) == 36, "kernel struct termios must be 36 bytes");
_Static_assert(__builtin_offsetof(struct termios, c_line) == 16, "c_line offset");
_Static_assert(__builtin_offsetof(struct termios, c_cc) == 17, "c_cc offset");

#endif /* ARCHOS_UAPI_TERMIOS_H */
