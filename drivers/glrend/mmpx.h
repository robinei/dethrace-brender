#ifndef MMPX_H
#define MMPX_H

#include <stdint.h>

/* Magnify a 32-bit image 2x with MMPX. dst must hold (2 * width) * (2 * height) pixels. */
void MMPXScale2x(const uint32_t* src, uint32_t* dst, int width, int height);

#endif
