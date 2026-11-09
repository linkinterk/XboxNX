/*
 * XboxNX — decodificador XPR0.
 *
 * Portado de hakuX (XprImage.kt, GPL-3.0-or-later): DXT1/3/5 y los formatos
 * RGB de 16 y 32 bits, en swizzle o lineales. XPR0: magia, tamaño total,
 * tamaño de cabecera (donde empiezan los píxeles), luego la cabecera del
 * recurso de textura (Common, Data, Lock, Format, Size).
 */
#include "xpr.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define XPR0 0x30525058u /* "XPR0" */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Los colores se guardan como ARGB en un uint32 y al final pasan a RGBA. */
static uint32_t rgb565(uint32_t v)
{
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}
static uint32_t argb1555(uint32_t v)
{
    uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
    uint32_t a = (v & 0x8000) ? 0xFF : 0;
    return (a << 24) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}
static uint32_t argb4444(uint32_t v)
{
    return ((((v >> 12) & 15) * 17) << 24) | ((((v >> 8) & 15) * 17) << 16) | ((((v >> 4) & 15) * 17) << 8) |
           ((v & 15) * 17);
}
static uint32_t mix(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t div)
{
    uint32_t out = 0xFF000000u;
    for (int s = 16; s >= 0; s -= 8) {
        uint32_t c = (wa * ((a >> s) & 0xFF) + wb * ((b >> s) & 0xFF)) / div;
        out |= c << s;
    }
    return out;
}

static bool dxt(const uint8_t *b, size_t len, size_t at, int w, int h, int kind, uint32_t *out)
{
    size_t block = kind == 1 ? 8 : 16;
    size_t bw = (size_t)(w + 3) / 4, bh = (size_t)(h + 3) / 4;
    if (at + bw * bh * block > len) {
        return false;
    }
    uint32_t alpha[16], rgb[4];
    size_t p = at;
    for (size_t by = 0; by < bh; by++) {
        for (size_t bx = 0; bx < bw; bx++) {
            if (kind == 3) {
                uint64_t bits = (uint64_t)rd32(b + p) | ((uint64_t)rd32(b + p + 4) << 32);
                for (int i = 0; i < 16; i++) {
                    alpha[i] = (uint32_t)((bits >> (4 * i)) & 0xF) * 17;
                }
                p += 8;
            } else if (kind == 5) {
                uint32_t a0 = b[p], a1 = b[p + 1];
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) {
                    bits |= (uint64_t)b[p + 2 + i] << (8 * i);
                }
                for (int i = 0; i < 16; i++) {
                    uint32_t c = (uint32_t)((bits >> (3 * i)) & 7);
                    if (c == 0) {
                        alpha[i] = a0;
                    } else if (c == 1) {
                        alpha[i] = a1;
                    } else if (a0 > a1) {
                        alpha[i] = ((8 - c) * a0 + (c - 1) * a1) / 7;
                    } else if (c == 6) {
                        alpha[i] = 0;
                    } else if (c == 7) {
                        alpha[i] = 255;
                    } else {
                        alpha[i] = ((6 - c) * a0 + (c - 1) * a1) / 5;
                    }
                }
                p += 8;
            } else {
                for (int i = 0; i < 16; i++) {
                    alpha[i] = 255;
                }
            }
            uint32_t c0 = rd16(b + p), c1 = rd16(b + p + 2), idx = rd32(b + p + 4);
            p += 8;
            rgb[0] = rgb565(c0);
            rgb[1] = rgb565(c1);
            bool four = kind != 1 || c0 > c1;
            rgb[2] = four ? mix(rgb[0], rgb[1], 2, 1, 3) : mix(rgb[0], rgb[1], 1, 1, 2);
            rgb[3] = four ? mix(rgb[0], rgb[1], 1, 2, 3) : 0;
            for (int i = 0; i < 16; i++) {
                int x = (int)bx * 4 + (i & 3), y = (int)by * 4 + (i >> 2);
                if (x >= w || y >= h) {
                    continue;
                }
                uint32_t code = (idx >> (2 * i)) & 3;
                uint32_t a = (!four && code == 3) ? 0 : alpha[i];
                out[y * w + x] = (a << 24) | (rgb[code] & 0xFFFFFF);
            }
        }
    }
    return true;
}

static uint32_t deposit(uint32_t v, uint32_t mask)
{
    uint32_t r = 0;
    while (mask) {
        uint32_t low = mask & (~mask + 1);
        if (v & 1) {
            r |= low;
        }
        v >>= 1;
        mask &= mask - 1;
    }
    return r;
}

typedef uint32_t (*Conv)(uint32_t);
static uint32_t conv_id(uint32_t v) { return v; }
static uint32_t conv_x8(uint32_t v) { return v | 0xFF000000u; }
static uint32_t conv_x1555(uint32_t v) { return argb1555(v | 0x8000); }

static bool plain(const uint8_t *b, size_t len, size_t at, int w, int h, int bpp, bool swizzled, size_t pitch,
                  Conv conv, uint32_t *out)
{
    size_t stride = (!swizzled && pitch > 0) ? pitch : (size_t)w * (size_t)bpp;
    if (at + stride * (size_t)(h - 1) + (size_t)w * (size_t)bpp > len) {
        return false;
    }
    uint32_t xm = 0, ym = 0;
    if (swizzled) {
        uint32_t bit = 1;
        for (uint32_t i = 1; i < (uint32_t)w || i < (uint32_t)h; i <<= 1) {
            if (i < (uint32_t)w) {
                xm |= bit;
                bit <<= 1;
            }
            if (i < (uint32_t)h) {
                ym |= bit;
                bit <<= 1;
            }
        }
        /* Con swizzle la textura ocupa w*h*bpp seguidos. */
        if (at + (size_t)w * (size_t)h * (size_t)bpp > len) {
            return false;
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t off = swizzled ? at + (size_t)(deposit((uint32_t)y, ym) | deposit((uint32_t)x, xm)) * (size_t)bpp
                                  : at + (size_t)y * stride + (size_t)x * (size_t)bpp;
            uint32_t v = bpp == 4 ? rd32(b + off) : rd16(b + off);
            out[y * w + x] = conv(v);
        }
    }
    return true;
}

int xpr_decode(const uint8_t *b, size_t len, XImage *img)
{
    memset(img, 0, sizeof(*img));
    if (len < 0x20 || rd32(b) != XPR0) {
        return -1;
    }
    size_t at = (size_t)rd32(b + 8) + rd32(b + 0x10);
    uint32_t format = rd32(b + 0x18), size = rd32(b + 0x1C);
    int w, h;
    size_t pitch = 0;
    if (size != 0) {
        w = (int)(size & 0xFFF) + 1;
        h = (int)((size >> 12) & 0xFFF) + 1;
        pitch = (size_t)(((size >> 24) & 0xFF) + 1) * 64;
    } else {
        w = 1 << ((format >> 20) & 0xF);
        h = 1 << ((format >> 24) & 0xF);
    }
    if (w < 1 || w > 1024 || h < 1 || h > 1024 || at >= len) {
        return -2;
    }
    uint32_t *argb = calloc((size_t)w * (size_t)h, 4);
    if (!argb) {
        return -3;
    }
    bool ok;
    switch ((format >> 8) & 0xFF) {
    case 0x0C: ok = dxt(b, len, at, w, h, 1, argb); break;
    case 0x0E: ok = dxt(b, len, at, w, h, 3, argb); break;
    case 0x0F: ok = dxt(b, len, at, w, h, 5, argb); break;
    case 0x02: ok = plain(b, len, at, w, h, 2, true, pitch, argb1555, argb); break;
    case 0x03: ok = plain(b, len, at, w, h, 2, true, pitch, conv_x1555, argb); break;
    case 0x04: ok = plain(b, len, at, w, h, 2, true, pitch, argb4444, argb); break;
    case 0x05: ok = plain(b, len, at, w, h, 2, true, pitch, rgb565, argb); break;
    case 0x06: ok = plain(b, len, at, w, h, 4, true, pitch, conv_id, argb); break;
    case 0x07: ok = plain(b, len, at, w, h, 4, true, pitch, conv_x8, argb); break;
    case 0x10: ok = plain(b, len, at, w, h, 2, false, pitch, argb1555, argb); break;
    case 0x11: ok = plain(b, len, at, w, h, 2, false, pitch, rgb565, argb); break;
    case 0x12: ok = plain(b, len, at, w, h, 4, false, pitch, conv_id, argb); break;
    case 0x1C: ok = plain(b, len, at, w, h, 2, false, pitch, conv_x1555, argb); break;
    case 0x1D: ok = plain(b, len, at, w, h, 2, false, pitch, argb4444, argb); break;
    case 0x1E: ok = plain(b, len, at, w, h, 4, false, pitch, conv_x8, argb); break;
    default: ok = false; break;
    }
    if (!ok) {
        free(argb);
        return -4;
    }
    /* ARGB -> bytes RGBA */
    uint8_t *px = (uint8_t *)argb;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        uint32_t v = argb[i];
        px[i * 4 + 0] = (uint8_t)(v >> 16);
        px[i * 4 + 1] = (uint8_t)(v >> 8);
        px[i * 4 + 2] = (uint8_t)v;
        px[i * 4 + 3] = (uint8_t)(v >> 24);
    }
    img->w = w;
    img->h = h;
    img->px = px;
    return 0;
}

void ximage_free(XImage *img)
{
    free(img->px);
    img->px = NULL;
    img->w = img->h = 0;
}
