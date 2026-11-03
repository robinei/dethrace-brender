/*
 * xBR 2x pixel art magnification (Hyllian's xBR level 2 rules).
 *
 * Unlike MMPX, xBR blends colours along detected edges, giving smoother, "painted" curves.
 * Blending is done on all four channels, which is correct for the premultiplied textures the GL
 * renderer uses (colour-keyed texels are 0x00000000): cut-out silhouettes get partial alpha
 * instead of fringes in the key colour. Sampling wraps around the edges, matching GL_REPEAT.
 *
 * Speed: the source is copied into a buffer with a wrapped border (no per-read modulo), and since
 * textures come from 8-bit palettes, each distinct colour gets a small index and every colour
 * comparison becomes a lookup in a precomputed difference table. Images with too many distinct
 * colours (e.g. the second pass of 4x) fall back to comparing colours directly.
 */
#include "xbr.h"
#include <stdlib.h>
#include <string.h>

/* Neighbourhood reach of the rules; the source is copied into a buffer with a wrapped border this wide */
#define PAD 2

/* Largest number of distinct colours handled with a difference table (table is MAX_INDEXED^2 entries) */
#define MAX_INDEXED 512

#define HASH_SIZE 4096 /* power of two, > MAX_INDEXED */

typedef struct {
    int32_t y, u, v, a;
} yuva;

static yuva to_yuva(uint32_t c) {
    int r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
    yuva out;
    out.y = (int32_t)(0.299 * r + 0.587 * g + 0.114 * b);
    out.u = (int32_t)(-0.169 * r - 0.331 * g + 0.5 * b) + 128;
    out.v = (int32_t)(0.5 * r - 0.419 * g - 0.081 * b) + 128;
    out.a = c >> 24;
    return out;
}

/* Colour distance in YUV (as in FFmpeg's xbr filter), with transparency counting as very different. */
static unsigned yuva_diff(const yuva* p, const yuva* q) {
    return abs(p->y - q->y) + abs(p->u - q->u) + abs(p->v - q->v) + abs(p->a - q->a);
}

static unsigned hash_colour(uint32_t c) {
    return (c * 2654435761u) >> 20;
}

/* dst + (src - dst) * m / 2^s on all four channels, two channels per operation (same rounding). */
static uint32_t blend(uint32_t dst, uint32_t src, uint32_t m, int s) {
    const uint32_t k = (1u << s) - m;
    const uint32_t rb = (((dst & 0x00ff00ff) * k + (src & 0x00ff00ff) * m) >> s) & 0x00ff00ff;
    const uint32_t ag = ((((dst >> 8) & 0x00ff00ff) * k + ((src >> 8) & 0x00ff00ff) * m) >> s) & 0x00ff00ff;
    return rb | (ag << 8);
}

/*
 * One corner of the 2x2 output block E, rotated by the caller. N3 is the corner being smoothed.
 * The P* values are whatever the caller's df()/eq() compare (palette indices or colours) and
 * COLOUR() turns one into a colour for blending.
 */
#define FILT2(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, N0, N1, N2, N3)  \
    do {                                                                                                          \
        if (PE != PH && PE != PF) {                                                                               \
            const unsigned e = df(PE, PC) + df(PE, PG) + df(PI, H5) + df(PI, F4) + (df(PH, PF) << 2);             \
            const unsigned i = df(PH, PD) + df(PH, I5) + df(PF, I4) + df(PF, PB) + (df(PE, PI) << 2);             \
            if (e <= i) {                                                                                         \
                const uint32_t px = COLOUR(df(PE, PF) <= df(PE, PH) ? PF : PH);                                   \
                if (e < i && ((!eq(PF, PB) && !eq(PH, PD)) || (eq(PE, PI) && (!eq(PF, I4) && !eq(PH, I5)))        \
                                 || eq(PE, PG) || eq(PE, PC))) {                                                  \
                    const unsigned ke = df(PF, PG);                                                               \
                    const unsigned ki = df(PH, PC);                                                               \
                    const int left = (ke << 1) <= ki && PE != PG && PD != PG;                                     \
                    const int up = ke >= (ki << 1) && PE != PC && PB != PC;                                       \
                    if (left && up) {                                                                             \
                        E[N3] = blend(E[N3], px, 7, 3);                                                           \
                        E[N2] = blend(E[N2], px, 1, 2);                                                           \
                        E[N1] = E[N2];                                                                            \
                    } else if (left) {                                                                            \
                        E[N3] = blend(E[N3], px, 3, 2);                                                           \
                        E[N2] = blend(E[N2], px, 1, 2);                                                           \
                    } else if (up) {                                                                              \
                        E[N3] = blend(E[N3], px, 3, 2);                                                           \
                        E[N1] = blend(E[N1], px, 1, 2);                                                           \
                    } else {                                                                                      \
                        E[N3] = blend(E[N3], px, 1, 1);                                                           \
                    }                                                                                             \
                } else {                                                                                          \
                    E[N3] = blend(E[N3], px, 1, 1);                                                               \
                }                                                                                                 \
            }                                                                                                     \
        }                                                                                                         \
    } while (0)

/* The 5x5 neighbourhood (minus corners) around row[0], and the four rotated corner filters. */
#define XBR_PIXEL(T, row, pitch, out, out_pitch)                                                                  \
    do {                                                                                                          \
        /*     A1 B1 C1                                                                                           \
         *  A0 PA PB PC C4                                                                                        \
         *  D0 PD PE PF F4                                                                                        \
         *  G0 PG PH PI I4                                                                                        \
         *     G5 H5 I5     */                                                                                    \
        const T A1 = row[-2 * pitch - 1], B1 = row[-2 * pitch], C1 = row[-2 * pitch + 1];                        \
        const T A0 = row[-pitch - 2], PA = row[-pitch - 1], PB = row[-pitch], PC = row[-pitch + 1], C4 = row[-pitch + 2]; \
        const T D0 = row[-2], PD = row[-1], PE = row[0], PF = row[1], F4 = row[2];                               \
        const T G0 = row[pitch - 2], PG = row[pitch - 1], PH = row[pitch], PI = row[pitch + 1], I4 = row[pitch + 2]; \
        const T G5 = row[2 * pitch - 1], H5 = row[2 * pitch], I5 = row[2 * pitch + 1];                           \
        uint32_t E[4]; /* 0 1                                                                                     \
                        * 2 3 */                                                                                  \
        E[0] = E[1] = E[2] = E[3] = COLOUR(PE);                                                                   \
        FILT2(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, 0, 1, 2, 3);   \
        FILT2(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0, 2, 0, 3, 1);   \
        FILT2(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I4, I5, 3, 2, 1, 0);   \
        FILT2(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C1, C4, 1, 3, 0, 2);   \
        (out)[0] = E[0];                                                                                          \
        (out)[1] = E[1];                                                                                          \
        (out)[out_pitch] = E[2];                                                                                  \
        (out)[(out_pitch) + 1] = E[3];                                                                            \
    } while (0)

/* Fast path: palette indices, comparisons through a difference table */
static void xbr_indexed(const uint16_t* padded, int pitch, const uint32_t* colours, const uint16_t* diff, int count,
    uint32_t* dst, int width, int height) {
    int x, y;

#define df(A, B) diff[(A) * count + (B)]
#define eq(A, B) (df(A, B) < 155)
#define COLOUR(A) colours[A]
    for (y = 0; y < height; y++) {
        const uint16_t* row = padded + (y + PAD) * pitch + PAD;
        uint32_t* out = dst + (y * 2) * (width * 2);
        for (x = 0; x < width; x++, row++, out += 2) {
            XBR_PIXEL(uint16_t, row, pitch, out, width * 2);
        }
    }
#undef df
#undef eq
#undef COLOUR
}

static unsigned colour_diff(uint32_t x, uint32_t y) {
    yuva p, q;
    if (x == y) {
        return 0;
    }
    p = to_yuva(x);
    q = to_yuva(y);
    return yuva_diff(&p, &q);
}

/* Fallback: colours compared directly */
static void xbr_direct(const uint32_t* padded, int pitch, uint32_t* dst, int width, int height) {
    int x, y;

#define df(A, B) colour_diff(A, B)
#define eq(A, B) (df(A, B) < 155)
#define COLOUR(A) (A)
    for (y = 0; y < height; y++) {
        const uint32_t* row = padded + (y + PAD) * pitch + PAD;
        uint32_t* out = dst + (y * 2) * (width * 2);
        for (x = 0; x < width; x++, row++, out += 2) {
            XBR_PIXEL(uint32_t, row, pitch, out, width * 2);
        }
    }
#undef df
#undef eq
#undef COLOUR
}

void XBRScale2x(const uint32_t* src, uint32_t* dst, int width, int height) {
    const int pitch = width + 2 * PAD;
    const int padded_size = pitch * (height + 2 * PAD);
    uint32_t hash_colours[HASH_SIZE];
    int16_t hash_index[HASH_SIZE];
    uint32_t colours[MAX_INDEXED];
    uint16_t* indices = malloc(sizeof(uint16_t) * width * height);
    int count = 0, i, j, x, y;

    if (indices == NULL) {
        return;
    }

    /* Give each distinct colour an index */
    memset(hash_index, -1, sizeof(hash_index));
    for (i = 0; i < width * height && count <= MAX_INDEXED; i++) {
        unsigned slot = hash_colour(src[i]) & (HASH_SIZE - 1);
        while (hash_index[slot] >= 0 && hash_colours[slot] != src[i]) {
            slot = (slot + 1) & (HASH_SIZE - 1);
        }
        if (hash_index[slot] < 0) {
            if (count == MAX_INDEXED) {
                count++; /* too many colours */
                break;
            }
            hash_colours[slot] = src[i];
            hash_index[slot] = (int16_t)count;
            colours[count++] = src[i];
        }
        indices[i] = (uint16_t)hash_index[slot];
    }

    if (count <= MAX_INDEXED) {
        uint16_t* padded = malloc(sizeof(uint16_t) * padded_size);
        uint16_t* diff = malloc(sizeof(uint16_t) * count * count);
        yuva yuvs[MAX_INDEXED];

        if (padded != NULL && diff != NULL) {
            for (i = 0; i < count; i++) {
                yuvs[i] = to_yuva(colours[i]);
            }
            for (i = 0; i < count; i++) {
                diff[i * count + i] = 0;
                for (j = i + 1; j < count; j++) {
                    diff[i * count + j] = diff[j * count + i] = (uint16_t)yuva_diff(&yuvs[i], &yuvs[j]);
                }
            }
            for (y = 0; y < height + 2 * PAD; y++) {
                const uint16_t* src_row = indices + ((y - PAD + height * PAD) % height) * width;
                for (x = 0; x < pitch; x++) {
                    padded[y * pitch + x] = src_row[(x - PAD + width * PAD) % width];
                }
            }
            xbr_indexed(padded, pitch, colours, diff, count, dst, width, height);
        }
        free(padded);
        free(diff);
    } else {
        uint32_t* padded = malloc(sizeof(uint32_t) * padded_size);

        if (padded != NULL) {
            for (y = 0; y < height + 2 * PAD; y++) {
                const uint32_t* src_row = src + ((y - PAD + height * PAD) % height) * width;
                for (x = 0; x < pitch; x++) {
                    padded[y * pitch + x] = src_row[(x - PAD + width * PAD) % width];
                }
            }
            xbr_direct(padded, pitch, dst, width, height);
        }
        free(padded);
    }
    free(indices);
}
