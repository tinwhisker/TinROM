/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdint.h>

/* Packed 0x00RRGGBB so gp0_rgb() calls read the same as the constant. */
#define TINROM_COLOR_BACKGROUND 0x101828
#define TINROM_COLOR_PANEL      0x1c2a42
#define TINROM_COLOR_HEADER     0x2b3d5c
#define TINROM_COLOR_ACCENT     0x4a9eff
#define TINROM_COLOR_SELECTION  0x264673
#define TINROM_COLOR_WARNING    0xffb340
#define TINROM_COLOR_ERROR      0xff5a5a
#define TINROM_COLOR_OK         0x5ad18a
/* Text: the plain colour, and the one for things that cannot be chosen. */
#define TINROM_COLOR_TEXT       0xffffff
#define TINROM_COLOR_DIM        0x8a96aa

#define TINROM_MARGIN_X   12
#define TINROM_HEADER_H   22
#define TINROM_FOOTER_H   16
#define TINROM_ROW_HEIGHT 12

#define TINROM_RGB_R(c) ((uint8_t) (((c) >> 16) & 0xff))
#define TINROM_RGB_G(c) ((uint8_t) (((c) >>  8) & 0xff))
#define TINROM_RGB_B(c) ((uint8_t) ( (c)        & 0xff))
