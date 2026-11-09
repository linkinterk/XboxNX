/*
 * XboxNX — dibujo por software.
 */
#include "gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static Gfx g;

void gfx_target(uint32_t *px, int w, int h, int stride)
{
    g.px = px;
    g.w = w;
    g.h = h;
    g.stride = stride;
}

const Gfx *gfx_get(void) { return &g; }

static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t cov)
{
    /* cov: 0..255, multiplica al alfa de src */
    uint32_t a = (((src >> 24) & 0xFF) * cov + 127) / 255;
    if (a == 0) {
        return dst;
    }
    if (a == 255) {
        return src | 0xFF000000u;
    }
    uint32_t ia = 255 - a;
    uint32_t r = ((src & 0xFF) * a + (dst & 0xFF) * ia) / 255;
    uint32_t gg = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia) / 255;
    uint32_t b = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
    return r | (gg << 8) | (b << 16) | 0xFF000000u;
}

static inline void put(int x, int y, uint32_t c, uint32_t cov)
{
    if ((unsigned)x >= (unsigned)g.w || (unsigned)y >= (unsigned)g.h) {
        return;
    }
    uint32_t *p = &g.px[y * g.stride + x];
    *p = blend(*p, c, cov);
}

void gfx_clear(uint32_t c)
{
    for (int y = 0; y < g.h; y++) {
        uint32_t *row = &g.px[y * g.stride];
        for (int x = 0; x < g.w; x++) {
            row[x] = c;
        }
    }
}

void gfx_rect(int x, int y, int w, int h, uint32_t c)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g.w ? g.w : x + w, y1 = y + h > g.h ? g.h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &g.px[yy * g.stride];
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = blend(row[xx], c, 255);
        }
    }
}

static uint32_t lerp_color(uint32_t a, uint32_t b, int t, int n)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        int ca = (int)((a >> s) & 0xFF), cb = (int)((b >> s) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * t / (n ? n : 1)) << s;
    }
    return out;
}

void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    for (int i = 0; i < h; i++) {
        gfx_rect(x, y + i, w, 1, lerp_color(top, bottom, i, h - 1));
    }
}

/* Cobertura (0..255) de un píxel respecto a un rectángulo redondeado. */
static uint32_t rr_cov(float px, float py, float x, float y, float w, float h, float r)
{
    float cx = px < x + r ? x + r : (px > x + w - r ? x + w - r : px);
    float cy = py < y + r ? y + r : (py > y + h - r ? y + h - r : py);
    float dx = px - cx, dy = py - cy;
    float d = sqrtf(dx * dx + dy * dy) - r; /* <0 dentro */
    float cov = 0.5f - d;
    if (cov <= 0) {
        return 0;
    }
    if (cov >= 1) {
        return 255;
    }
    return (uint32_t)(cov * 255.0f);
}

void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c)
{
    if (r <= 0) {
        gfx_rect(x, y, w, h, c);
        return;
    }
    if (r * 2 > w) {
        r = w / 2;
    }
    if (r * 2 > h) {
        r = h / 2;
    }
    /* Centro sin esquinas: rápido. */
    gfx_rect(x + r, y, w - 2 * r, h, c);
    gfx_rect(x, y + r, r, h - 2 * r, c);
    gfx_rect(x + w - r, y + r, r, h - 2 * r, c);
    int corners[4][2] = {{x, y}, {x + w - r, y}, {x, y + h - r}, {x + w - r, y + h - r}};
    for (int k = 0; k < 4; k++) {
        for (int yy = corners[k][1]; yy < corners[k][1] + r; yy++) {
            for (int xx = corners[k][0]; xx < corners[k][0] + r; xx++) {
                uint32_t cov = rr_cov(xx + 0.5f, yy + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r);
                if (cov) {
                    put(xx, yy, c, cov);
                }
            }
        }
    }
}

void gfx_rrect_outline(int x, int y, int w, int h, int r, int t, uint32_t c)
{
    float ri = (float)(r - t > 0 ? r - t : 0);
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            bool edge = xx < x + r + t || xx >= x + w - r - t || yy < y + r + t || yy >= y + h - r - t;
            if (!edge) {
                continue;
            }
            uint32_t outer = rr_cov(xx + 0.5f, yy + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r);
            uint32_t inner = rr_cov(xx + 0.5f, yy + 0.5f, (float)(x + t), (float)(y + t), (float)(w - 2 * t),
                                    (float)(h - 2 * t), ri);
            if (outer > inner) {
                put(xx, yy, c, outer - inner);
            }
        }
    }
}

void gfx_circle(int cx, int cy, int r, uint32_t c)
{
    gfx_rrect(cx - r, cy - r, 2 * r, 2 * r, r, c);
}

void gfx_image(const XImage *img, int x, int y, int w, int h, int r)
{
    if (!img || !img->px || w <= 0 || h <= 0) {
        return;
    }
    for (int yy = 0; yy < h; yy++) {
        float sy = ((float)yy + 0.5f) * (float)img->h / (float)h - 0.5f;
        int y0 = (int)floorf(sy);
        float fy = sy - (float)y0;
        int ya = y0 < 0 ? 0 : (y0 >= img->h ? img->h - 1 : y0);
        int yb = y0 + 1 >= img->h ? img->h - 1 : (y0 + 1 < 0 ? 0 : y0 + 1);
        for (int xx = 0; xx < w; xx++) {
            uint32_t cov = 255;
            if (r > 0) {
                cov = rr_cov((float)xx + 0.5f, (float)yy + 0.5f, 0, 0, (float)w, (float)h, (float)r);
                if (!cov) {
                    continue;
                }
            }
            float sx = ((float)xx + 0.5f) * (float)img->w / (float)w - 0.5f;
            int x0 = (int)floorf(sx);
            float fx = sx - (float)x0;
            int xa = x0 < 0 ? 0 : (x0 >= img->w ? img->w - 1 : x0);
            int xb = x0 + 1 >= img->w ? img->w - 1 : (x0 + 1 < 0 ? 0 : x0 + 1);
            const uint8_t *p00 = img->px + (ya * img->w + xa) * 4, *p01 = img->px + (ya * img->w + xb) * 4;
            const uint8_t *p10 = img->px + (yb * img->w + xa) * 4, *p11 = img->px + (yb * img->w + xb) * 4;
            uint32_t out = 0;
            for (int ch = 0; ch < 4; ch++) {
                float top = p00[ch] + (p01[ch] - p00[ch]) * fx;
                float bot = p10[ch] + (p11[ch] - p10[ch]) * fx;
                int v = (int)(top + (bot - top) * fy + 0.5f);
                out |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << (ch * 8);
            }
            put(x + xx, y + yy, out, cov);
        }
    }
}

/* ---- texto ---- */

static stbtt_fontinfo g_font;
static bool g_font_ok;

typedef struct Glyph {
    uint32_t cp;
    int size;
    int w, h, xoff, yoff, adv;
    uint8_t *bmp;
    bool used;
} Glyph;

#define GLYPHS 4096
static Glyph g_glyphs[GLYPHS];

bool gfx_font_init(const uint8_t *ttf, size_t len)
{
    (void)len;
    int off = stbtt_GetFontOffsetForIndex(ttf, 0);
    g_font_ok = off >= 0 && stbtt_InitFont(&g_font, ttf, off);
    return g_font_ok;
}

static const Glyph *glyph(uint32_t cp, int size)
{
    uint32_t hsh = (cp * 2654435761u) ^ ((uint32_t)size * 40503u);
    for (unsigned i = 0; i < GLYPHS; i++) {
        Glyph *gl = &g_glyphs[(hsh + i) % GLYPHS];
        if (gl->used && gl->cp == cp && gl->size == size) {
            return gl;
        }
        if (!gl->used) {
            float sc = stbtt_ScaleForPixelHeight(&g_font, (float)size);
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, &lsb);
            gl->used = true;
            gl->cp = cp;
            gl->size = size;
            gl->adv = (int)lroundf((float)adv * sc);
            gl->bmp = stbtt_GetCodepointBitmap(&g_font, sc, sc, (int)cp, &gl->w, &gl->h, &gl->xoff, &gl->yoff);
            return gl;
        }
    }
    return NULL; /* caché llena: no dibuja */
}

static uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = s[0];
    int n = 0;
    if (c < 0x80) {
        n = 1;
    } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        n = 2;
    } else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        n = 3;
    } else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        c = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        n = 4;
    } else {
        c = '?';
        n = 1;
    }
    *ps += n;
    return c;
}

static int text_impl(int x, int y, int size, uint32_t c, const char *s, int maxchars_bytes, bool draw)
{
    if (!g_font_ok) {
        return 0;
    }
    float sc = stbtt_ScaleForPixelHeight(&g_font, (float)size);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    int base = y + (int)lroundf((float)asc * sc);
    int pen = x;
    const char *end = maxchars_bytes >= 0 ? s + maxchars_bytes : NULL;
    uint32_t prev = 0;
    while (*s && (!end || s < end)) {
        uint32_t cp = utf8_next(&s);
        if (prev) {
            pen += (int)lroundf((float)stbtt_GetCodepointKernAdvance(&g_font, (int)prev, (int)cp) * sc);
        }
        const Glyph *gl = glyph(cp, size);
        if (!gl) {
            break;
        }
        if (draw && gl->bmp) {
            for (int yy = 0; yy < gl->h; yy++) {
                for (int xx = 0; xx < gl->w; xx++) {
                    uint8_t cov = gl->bmp[yy * gl->w + xx];
                    if (cov) {
                        put(pen + gl->xoff + xx, base + gl->yoff + yy, c, cov);
                    }
                }
            }
        }
        pen += gl->adv;
        prev = cp;
    }
    return pen - x;
}

int gfx_text(int x, int y, int size, uint32_t c, const char *s)
{
    return text_impl(x, y, size, c, s, -1, true);
}

int gfx_text_width(int size, const char *s)
{
    return text_impl(0, 0, size, 0, s, -1, false);
}

int gfx_text_fit(int x, int y, int size, int maxw, uint32_t c, const char *s)
{
    int full = gfx_text_width(size, s);
    if (full <= maxw) {
        return gfx_text(x, y, size, c, s);
    }
    int dots = gfx_text_width(size, "...");
    /* Recorta por caracteres (UTF-8) hasta que quepa con "...". */
    int bytes = 0, best = 0;
    const char *p = s;
    while (*p) {
        const char *q = p;
        utf8_next(&q);
        int nb = (int)(q - s);
        if (text_impl(0, 0, size, 0, s, nb, false) + dots > maxw) {
            break;
        }
        best = nb;
        p = q;
        bytes = nb;
    }
    (void)bytes;
    int w = text_impl(x, y, size, c, s, best, true);
    return w + gfx_text(x + w, y, size, c, "...");
}

void gfx_text_center(int x, int w, int y, int size, uint32_t c, const char *s)
{
    int tw = gfx_text_width(size, s);
    gfx_text(x + (w - tw) / 2, y, size, c, s);
}
