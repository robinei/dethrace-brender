/*
 * MMPX 2x pixel art magnification.
 *
 * Morgan McGuire and Mara Gagiu, "MMPX Style-Preserving Pixel Art Magnification",
 * Journal of Graphics Techniques, 2021. https://casual-effects.com/research/McGuire2021PixelArt/
 *
 * The output only contains colours present in the input, so palette, dithering and colour-keyed
 * (transparent) texels are preserved. Sampling wraps around the edges, matching GL_REPEAT textures.
 */
#include "mmpx.h"
#include <stdlib.h>

/* Neighbourhood reach of the rules; the source is copied into a buffer with a wrapped border this wide */
#define PAD 3

static uint32_t luma(uint32_t c) {
    uint32_t alpha = c >> 24;
    return (((c >> 16) & 0xff) + ((c >> 8) & 0xff) + (c & 0xff) + 1) * (256 - alpha);
}

static int all_eq2(uint32_t b, uint32_t a0, uint32_t a1) {
    return ((b ^ a0) | (b ^ a1)) == 0;
}

static int all_eq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2) {
    return ((b ^ a0) | (b ^ a1) | (b ^ a2)) == 0;
}

static int all_eq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    return ((b ^ a0) | (b ^ a1) | (b ^ a2) | (b ^ a3)) == 0;
}

static int any_eq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2) {
    return b == a0 || b == a1 || b == a2;
}

static int none_eq2(uint32_t b, uint32_t a0, uint32_t a1) {
    return b != a0 && b != a1;
}

static int none_eq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    return b != a0 && b != a1 && b != a2 && b != a3;
}

void MMPXScale2x(const uint32_t* src, uint32_t* dst, int width, int height) {
    const int pitch = width + 2 * PAD;
    uint32_t* padded = malloc(sizeof(uint32_t) * pitch * (height + 2 * PAD));
    const uint32_t* row;
    int x, y;

    if (padded == NULL) {
        return;
    }
    for (y = 0; y < height + 2 * PAD; y++) {
        const uint32_t* src_row = src + ((y - PAD + height * PAD) % height) * width;
        for (x = 0; x < pitch; x++) {
            padded[y * pitch + x] = src_row[(x - PAD + width * PAD) % width];
        }
    }

#define SRC(dx, dy) row[(dy) * pitch + (dx)]

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            row = padded + (y + PAD) * pitch + x + PAD;
            {
            uint32_t A = SRC(-1, -1), B = SRC(0, -1), C = SRC(1, -1);
            uint32_t D = SRC(-1, 0), E = SRC(0, 0), F = SRC(1, 0);
            uint32_t G = SRC(-1, 1), H = SRC(0, 1), I = SRC(1, 1);

            /* Output: J K
             *         L M */
            uint32_t J = E, K = E, L = E, M = E;

            if (((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0) {
                uint32_t P = SRC(0, -2), S = SRC(0, 2);
                uint32_t Q = SRC(-2, 0), R = SRC(2, 0);
                uint32_t Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);

                /* 1:1 slope rules */
                if ((D == B && D != H && D != F) && (El >= Dl || E == A) && any_eq3(E, A, C, G) && ((El < Dl) || A != D || E != P || E != Q)) {
                    J = D;
                }
                if ((B == F && B != D && B != H) && (El >= Bl || E == C) && any_eq3(E, A, C, I) && ((El < Bl) || C != B || E != P || E != R)) {
                    K = B;
                }
                if ((H == D && H != F && H != B) && (El >= Hl || E == G) && any_eq3(E, A, G, I) && ((El < Hl) || G != H || E != S || E != Q)) {
                    L = H;
                }
                if ((F == H && F != B && F != D) && (El >= Fl || E == I) && any_eq3(E, C, G, I) && ((El < Fl) || I != H || E != R || E != S)) {
                    M = F;
                }

                /* Intersection rules */
                if ((E != F && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && (F != SRC(3, 0))) {
                    K = M = F;
                }
                if ((E != D && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && (D != SRC(-3, 0))) {
                    J = L = D;
                }
                if ((E != H && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && (H != SRC(0, 3))) {
                    L = M = H;
                }
                if ((E != B && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && (B != SRC(0, -3))) {
                    J = K = B;
                }

                /* Triangle tip rules */
                if (Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) {
                    J = K = B;
                }
                if (Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) {
                    L = M = H;
                }
                if (Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) {
                    K = M = F;
                }
                if (Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) {
                    J = L = D;
                }

                /* 2:1 slope rules */
                if (H != B) {
                    if (H != A && H != E && H != C) {
                        if (all_eq3(H, G, F, R) && none_eq2(H, D, SRC(2, -1))) {
                            L = M;
                        }
                        if (all_eq3(H, I, D, Q) && none_eq2(H, F, SRC(-2, -1))) {
                            M = L;
                        }
                    }
                    if (B != I && B != G && B != E) {
                        if (all_eq3(B, A, F, R) && none_eq2(B, D, SRC(2, 1))) {
                            J = K;
                        }
                        if (all_eq3(B, C, D, Q) && none_eq2(B, F, SRC(-2, 1))) {
                            K = J;
                        }
                    }
                }
                if (F != D) {
                    if (D != I && D != E && D != C) {
                        if (all_eq3(D, A, H, S) && none_eq2(D, B, SRC(1, 2))) {
                            J = L;
                        }
                        if (all_eq3(D, G, B, P) && none_eq2(D, H, SRC(1, -2))) {
                            L = J;
                        }
                    }
                    if (F != E && F != A && F != G) {
                        if (all_eq3(F, C, H, S) && none_eq2(F, B, SRC(-1, 2))) {
                            K = M;
                        }
                        if (all_eq3(F, I, B, P) && none_eq2(F, H, SRC(-1, -2))) {
                            M = K;
                        }
                    }
                }
            }

            dst[(y * 2) * (width * 2) + x * 2] = J;
            dst[(y * 2) * (width * 2) + x * 2 + 1] = K;
            dst[(y * 2 + 1) * (width * 2) + x * 2] = L;
            dst[(y * 2 + 1) * (width * 2) + x * 2 + 1] = M;
            }
        }
    }
#undef SRC
    free(padded);
}
