#ifndef XBR_H
#define XBR_H

#include <stdint.h>

/* Magnify a 32-bit (premultiplied) image 2x with xBR. dst must hold (2 * width) * (2 * height) pixels. */
void XBRScale2x(const uint32_t* src, uint32_t* dst, int width, int height);

#endif
