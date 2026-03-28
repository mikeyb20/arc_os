#ifndef ARCHOS_DRIVERS_FONT8X16_H
#define ARCHOS_DRIVERS_FONT8X16_H

#include <stdint.h>

#define FONT_WIDTH   8
#define FONT_HEIGHT  16
#define FONT_FIRST   0    /* First glyph index */
#define FONT_LAST    127  /* Last glyph index */

/* 8x16 bitmap font — 128 glyphs, 16 bytes per glyph.
 * Each byte represents one row of 8 pixels (MSB = leftmost). */
extern const uint8_t font8x16_data[128][16];

#endif /* ARCHOS_DRIVERS_FONT8X16_H */
