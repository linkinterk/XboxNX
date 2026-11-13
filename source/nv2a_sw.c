/*
 * XboxNX — dibujo del NV2A por software (etapa 4b). Ver nv2a_sw.h.
 *
 * Los nombres y valores de los métodos de Kelvin siguen la documentación
 * pública del NV2A (nouveau, xemu). Las coordenadas de salida del vertex
 * program ya están en píxeles de la superficie (el XDK agrega la
 * transformación del viewport con c[-38] y c[-37]); oPos.w es la w del
 * espacio de recorte, para interpolar con perspectiva.
 */
#include "nv2a_sw.h"
#include "prof.h"
#include "vsh_glsl.h"

/* lo que más tiempo gasta del emulador: con -O3 ~30% más rápido */
#pragma GCC optimize("O3")

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "workers.h"
#include "gpu.h"
#include "vsh/nv2a_vsh_disassembler.h"
#include "vsh/nv2a_vsh_emulator.h"

#define MAX_VERTS (1u << 18)
#define PROG_SLOTS 136

typedef struct OutVert {
    float pos[4];        /* x, y en píxeles; z en la escala del Z; w de recorte */
    float col[2][4];     /* oD0, oD1 */
    float tex[4][4];
    float fog;
} OutVert;

static struct {
    uint8_t *ram;
    uint32_t ram_size;
    SwDmaFn dma;
    /* objetos DMA */
    uint32_t dma_a, dma_b, dma_color, dma_zeta, dma_va, dma_vb;
    bool has_color, has_zeta; /* la base de un DMA suele ser 0: toda la memoria */
    /* superficies */
    uint32_t surf_format, surf_pitch, color_off, zeta_off, clip_h, clip_v;
    uint32_t zs_clear, clear_h, clear_v;
    /* el vertex program */
    uint32_t pmem[PROG_SLOTS * 4];
    unsigned pload, pword, pstart;
    bool pdirty, pok;
    Nv2aVshProgram prog;
    uint64_t pkey; /* el hash del programa (para su vertex shader en el GPU) */
    float consts[192 * 4];
    unsigned cload, cword;
    uint32_t xform_mode;
    /* atributos de los vértices */
    float cur[16][4];
    uint32_t va_off[16], va_fmt[16];
    float (*in)[16][4];
    unsigned nin, cap;
    uint32_t prim;
    bool begun;
    uint32_t *inl;
    unsigned ninl, inl_cap;
    /* texturas */
    uint32_t t_off[4], t_fmt[4], t_addr[4], t_ctl0[4], t_ctl1[4], t_filter[4], t_rect[4], t_pal[4];
    uint32_t shader_prog;
    /* combiners */
    uint32_t c_icw[8], a_icw[8], c_ocw[8], a_ocw[8], factor0[8], factor1[8], cctl, fcw0, fcw1;
    uint32_t fog_color;
    /* rasterización */
    bool blend, atest, dtest, dmask, cull;
    uint32_t sfac, dfac, beq, afunc, aref, dfunc, cull_face, front_face, cmask, bcolor;
    SwStats st;
} S;

/* S.prog ya se intentó decodificar para el programa puesto (S.pok dice si
 * salió). Fuera de S: el estado capturado (sw_state_get) no cambia de forma. */
static bool g_pparsed;

void sw_init(uint8_t *ram, uint32_t ram_size, SwDmaFn dma)
{
    if (S.pok) {
        nv2a_vsh_program_destroy(&S.prog);
    }
    free(S.in);
    free(S.inl);
    memset(&S, 0, sizeof(S));
    S.ram = ram;
    S.ram_size = ram_size;
    workers_init();
    S.dma = dma;
    S.dmask = true;
    S.cmask = 0x01010101u;
    S.sfac = 1;
    S.dfac = 0;
    S.beq = 0x8006;
    S.dfunc = 0x201;
    S.afunc = 0x207;
    S.cull_face = 0x405;
    S.front_face = 0x901;
    for (int i = 0; i < 16; i++) {
        S.cur[i][3] = 1.0f;
    }
    S.pdirty = true;
}

const SwStats *sw_stats(void) { return &S.st; }

/* El estado de Kelvin (para capturar un cuadro con lo que venía de antes). */
size_t sw_state_size(void) { return sizeof(S); }
void sw_state_get(void *out) { memcpy(out, &S, sizeof(S)); }
void sw_state_set(const void *in)
{
    uint8_t *ram = S.ram;
    uint32_t ram_size = S.ram_size;
    SwDmaFn dma = S.dma;
    float(*vin)[16][4] = S.in;
    unsigned cap = S.cap, inl_cap = S.inl_cap;
    uint32_t *inl = S.inl;
    memcpy(&S, in, sizeof(S));
    S.ram = ram, S.ram_size = ram_size, S.dma = dma;
    S.in = vin, S.cap = cap, S.inl = inl, S.inl_cap = inl_cap;
    S.nin = S.ninl = 0;
    S.begun = false;
    memset(&S.prog, 0, sizeof(S.prog));
    S.pok = false;
    S.pdirty = true;
    memset(&S.st, 0, sizeof(S.st));
}

/* Saltar cuadros: sin dibujar (ni transformar vértices) hasta que se diga. */
static bool g_skip;
void sw_set_skip(bool skip) { g_skip = skip; }
bool sw_skipping(void) { return g_skip; }

/* Etapa 5: las pruebas de visibilidad (oclusión): cuántos píxeles pasaron la
 * prueba de Z desde CLEAR_REPORT_VALUE mientras ZPASS_PIXEL_COUNT_ENABLE está
 * puesto. Halo 2 las usa para las luces y sus destellos (con 0, nunca se ven). */
static bool g_zpass_on;
static uint64_t g_zpass_sw;

uint32_t sw_zpass_result(void)
{
    uint64_t n = g_zpass_sw + gpu_query_collect();
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

static uint8_t *mem(uint32_t pa, uint32_t len)
{
    pa &= S.ram_size - 1;
    return pa + len <= S.ram_size ? S.ram + pa : NULL;
}

static float f32(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

/* ------------------------------------------------------------ atributos */

/* Un atributo de un arreglo: tipos del NV2A 0 UB_D3D (BGRA), 1 S1, 2 F,
 * 4 UB_OGL (RGBA), 5 S32K, 6 CMP (11:11:10 con signo). */
static void attr_conv(uint32_t fmt, const uint8_t *p, float o[4])
{
    unsigned type = fmt & 0xF, size = (fmt >> 4) & 0xF;
    o[0] = o[1] = o[2] = 0;
    o[3] = 1;
    switch (type) {
    case 0:
        if (size == 4) {
            o[0] = p[2] / 255.0f, o[1] = p[1] / 255.0f, o[2] = p[0] / 255.0f, o[3] = p[3] / 255.0f;
        } else {
            for (unsigned i = 0; i < size && i < 4; i++) {
                o[i] = p[i] / 255.0f;
            }
        }
        break;
    case 4:
        for (unsigned i = 0; i < size && i < 4; i++) {
            o[i] = p[i] / 255.0f;
        }
        break;
    case 1:
        for (unsigned i = 0; i < size && i < 4; i++) {
            int16_t s;
            memcpy(&s, p + 2 * i, 2);
            o[i] = (2.0f * s + 1.0f) / 65535.0f;
        }
        break;
    case 5:
        for (unsigned i = 0; i < size && i < 4; i++) {
            int16_t s;
            memcpy(&s, p + 2 * i, 2);
            o[i] = s;
        }
        break;
    case 2:
        for (unsigned i = 0; i < size && i < 4; i++) {
            memcpy(&o[i], p + 4 * i, 4);
        }
        break;
    case 6: {
        uint32_t w;
        memcpy(&w, p, 4);
        int32_t x = (int32_t)(w << 21) >> 21, y = (int32_t)(w << 10) >> 21, z = (int32_t)w >> 22;
        o[0] = x / 1023.0f, o[1] = y / 1023.0f, o[2] = z / 511.0f;
        break;
    }
    default:
        break;
    }
}

static unsigned attr_bytes(uint32_t fmt)
{
    unsigned type = fmt & 0xF, size = (fmt >> 4) & 0xF;
    if (!size) {
        return 0;
    }
    if (type == 6) {
        return 4;
    }
    return size * (type == 2 ? 4u : (type == 1 || type == 5) ? 2u : 1u);
}

static uint32_t *g_idx;
static unsigned g_nidx, g_idx_cap;
static void idx_materialize(void);

/* El lugar del siguiente vértice (ya con los valores actuales); NULL: no cabe.
 * Etapa 5: se llena ahí mismo, sin copias de paso. */
static float (*next_vertex(void))[4]
{
    if (g_nidx) {
        idx_materialize(); /* los índices pendientes van antes */
    }
    if (S.nin >= S.cap) {
        if (S.cap >= MAX_VERTS) {
            return NULL;
        }
        unsigned nc = S.cap ? S.cap * 2 : 1024;
        void *n = realloc(S.in, (size_t)nc * sizeof(*S.in));
        if (!n) {
            return NULL;
        }
        S.in = n;
        S.cap = nc;
    }
    float(*v)[4] = S.in[S.nin++];
    memcpy(v, S.cur, sizeof(float) * 64);
    return v;
}

static bool push_vertex(const float v[16][4])
{
    float(*d)[4] = next_vertex();
    if (!d) {
        return false;
    }
    if ((const float *)v != &S.cur[0][0]) {
        memcpy(d, v, sizeof(float) * 64);
    }
    return true;
}

/* Etapa 5: los índices de ARRAY_ELEMENT / DRAW_ARRAYS se guardan y los
 * vértices se arman al dibujar: solo los atributos que el vertex shader usa,
 * cada vértice una vez (con índices para la GPU). Si hay que hacerlo como
 * antes (la CPU), se arman todos (idx_materialize). */

static void array_vertex_fetch(uint32_t i);

/* El vértice i de los arreglos (los atributos sin arreglo: su valor actual). */
static void array_vertex(uint32_t i)
{
    if (g_skip) {
        return;
    }
    if (S.nin) { /* ya hay vértices armados (mezcla): como antes */
        array_vertex_fetch(i);
        return;
    }
    if (g_nidx >= g_idx_cap) {
        if (g_idx_cap >= MAX_VERTS) {
            return;
        }
        unsigned nc = g_idx_cap ? g_idx_cap * 2 : 4096;
        void *n = realloc(g_idx, (size_t)nc * 4);
        if (!n) {
            array_vertex_fetch(i);
            return;
        }
        g_idx = n;
        g_idx_cap = nc;
    }
    g_idx[g_nidx++] = i;
}

static void array_vertex_fetch(uint32_t i)
{
    float(*v)[4] = next_vertex();
    if (!v) {
        return;
    }
    for (int a = 0; a < 16; a++) {
        uint32_t fmt = S.va_fmt[a];
        unsigned n = attr_bytes(fmt);
        if (!n) {
            continue;
        }
        uint32_t base = (S.va_off[a] & 0x80000000u) ? S.dma_vb : S.dma_va;
        uint32_t stride = fmt >> 8;
        const uint8_t *p = mem(base + (S.va_off[a] & 0x7FFFFFFFu) + i * stride, n);
        if (p) {
            attr_conv(fmt, p, v[a]);
        }
    }
}

static void idx_materialize(void)
{
    unsigned n = g_nidx;
    g_nidx = 0;
    for (unsigned k = 0; k < n; k++) {
        array_vertex_fetch(g_idx[k]);
    }
}

/* Los vértices de INLINE_ARRAY: los atributos con tamaño, en orden. */
static void inline_vertices(void)
{
    unsigned vbytes = 0;
    for (int a = 0; a < 16; a++) {
        vbytes += attr_bytes(S.va_fmt[a]);
    }
    if (!vbytes) {
        return;
    }
    const uint8_t *p = (const uint8_t *)S.inl;
    unsigned total = S.ninl * 4;
    for (unsigned at = 0; at + vbytes <= total; at += vbytes) {
        float v[16][4];
        memcpy(v, S.cur, sizeof(v));
        unsigned o = at;
        for (int a = 0; a < 16; a++) {
            unsigned n = attr_bytes(S.va_fmt[a]);
            if (n) {
                attr_conv(S.va_fmt[a], p + o, v[a]);
                o += n;
            }
        }
        push_vertex(v);
    }
    S.ninl = 0;
}

/* --------------------------------------------------------------- texturas */

typedef struct Tex {
    const uint8_t *base;
    uint32_t w, h, pitch, fmt;
    bool linear, normalized, bilinear;
    unsigned au, av;
    uint32_t *xm, *ym;   /* direcciones swizzle por x y por y */
    bool cube;
    uint32_t face_bytes; /* cubo: lo que ocupa cada cara con sus mipmaps */
    uint32_t bytes;      /* todo lo que ocupa (para la GPU: el hash del contenido) */
    const uint8_t *pal;  /* P8: la paleta (A8R8G8B8) */
    unsigned pal_n;
} Tex;

/* Las tablas swizzle de cada etapa de textura: cada etapa las suyas (si se
 * compartían, una textura de otro tamaño en otra etapa liberaba las de la
 * primera y esta leía memoria liberada: direcciones basura, cierre). */
static struct {
    uint32_t *x, *y;
    uint32_t w, h;
} g_swz[4];

static void swz_tables(int s, uint32_t w, uint32_t h, uint32_t **xm, uint32_t **ym)
{
    if (w != g_swz[s].w || h != g_swz[s].h || !g_swz[s].x || !g_swz[s].y) {
        free(g_swz[s].x);
        free(g_swz[s].y);
        uint32_t *tx = malloc(w * 4), *ty = malloc(h * 4);
        g_swz[s].x = tx;
        g_swz[s].y = ty;
        g_swz[s].w = w;
        g_swz[s].h = h;
        uint32_t mx = 0, my = 0;
        for (uint32_t i = 1, bit = 1; i < w || i < h; i <<= 1) {
            if (i < w) {
                mx |= bit;
                bit <<= 1;
            }
            if (i < h) {
                my |= bit;
                bit <<= 1;
            }
        }
        for (uint32_t x = 0; tx && x < w; x++) {
            uint32_t r = 0;
            for (uint32_t b = 1, sh = 0; b <= mx; b <<= 1) {
                if (mx & b) {
                    r |= ((x >> sh++) & 1) ? b : 0;
                }
            }
            tx[x] = r;
        }
        for (uint32_t y = 0; ty && y < h; y++) {
            uint32_t r = 0;
            for (uint32_t b = 1, sh = 0; b <= my; b <<= 1) {
                if (my & b) {
                    r |= ((y >> sh++) & 1) ? b : 0;
                }
            }
            ty[y] = r;
        }
    }
    *xm = g_swz[s].x;
    *ym = g_swz[s].y;
}

static bool fmt_linear(uint32_t f)
{
    return (f >= 0x10 && f <= 0x13) || (f >= 0x1B && f <= 0x20) || f == 0x24 || f == 0x25 || f == 0x3F ||
           f == 0x40 || f == 0x41 || (f >= 0x2E && f <= 0x31);
}

static unsigned fmt_bpp(uint32_t f)
{
    switch (f) {
    case 0x00: case 0x01: case 0x13: case 0x1B: case 0x19: case 0x1F: case 0x0B: return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1C: case 0x1D:
    case 0x1A: case 0x20: case 0x24: case 0x25: case 0x28: case 0x29: return 2;
    default: return 4;
    }
}

static bool tex_setup(int s, Tex *t)
{
    uint32_t fmtw = S.t_fmt[s];
    uint32_t base = (fmtw & 3) == 2 ? S.dma_b : S.dma_a;
    t->fmt = (fmtw >> 8) & 0xFF;
    t->linear = fmt_linear(t->fmt);
    if (t->linear) {
        t->w = S.t_rect[s] >> 16;
        t->h = S.t_rect[s] & 0xFFFF;
        t->pitch = S.t_ctl1[s] >> 16;
        t->normalized = false;
    } else {
        t->w = 1u << ((fmtw >> 20) & 0xF);
        t->h = 1u << ((fmtw >> 24) & 0xF);
        t->pitch = 0;
        t->normalized = true;
    }
    if (!t->w || !t->h || t->w > 4096 || t->h > 4096) {
        return false;
    }
    uint32_t bytes;
    bool dxt = t->fmt == 0x0C || t->fmt == 0x0E || t->fmt == 0x0F;
    if (dxt) {
        bytes = ((t->w + 3) / 4) * ((t->h + 3) / 4) * (t->fmt == 0x0C ? 8 : 16);
    } else if (t->linear) {
        if (t->pitch < t->w * fmt_bpp(t->fmt)) {
            return false; /* el pitch no alcanza para una fila: se leería fuera */
        }
        bytes = t->pitch * t->h;
    } else {
        bytes = t->w * t->h * fmt_bpp(t->fmt);
    }
    /* cubo: seis caras seguidas, cada una con su cadena de mipmaps,
     * alineadas a 128 bytes */
    t->cube = (fmtw >> 2) & 1;
    t->face_bytes = 0;
    if (t->cube && !t->linear) {
        unsigned levels = (fmtw >> 16) & 0xF;
        uint32_t fb = 0, w = t->w, h = t->h;
        for (unsigned l = 0; l < (levels ? levels : 1); l++) {
            if (dxt) {
                fb += ((w + 3) / 4) * ((h + 3) / 4) * (t->fmt == 0x0C ? 8 : 16);
            } else {
                fb += w * h * fmt_bpp(t->fmt);
            }
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        t->face_bytes = (fb + 127) & ~127u;
        bytes = t->face_bytes * 6;
    } else {
        t->cube = false;
    }
    t->bytes = bytes;
    t->base = mem(base + S.t_off[s], bytes);
    if (!t->base) {
        return false;
    }
    t->pal = NULL;
    if (t->fmt == 0x0B) {
        uint32_t pv = S.t_pal[s];
        t->pal_n = 256u >> ((pv >> 2) & 3);
        t->pal = mem(((pv & 3) ? S.dma_b : S.dma_a) + (pv & ~0x3Fu), t->pal_n * 4);
    }
    if (!t->linear && !dxt) {
        swz_tables(s, t->w, t->h, &t->xm, &t->ym);
        if (!t->xm || !t->ym) {
            return false;
        }
    }
    t->au = S.t_addr[s] & 0xF;
    t->av = (S.t_addr[s] >> 8) & 0xF;
    t->bilinear = ((S.t_filter[s] >> 24) & 0xF) == 2;
    return true;
}

static void rgb565(uint16_t p, float c[4])
{
    c[0] = (p >> 11) / 31.0f, c[1] = ((p >> 5) & 63) / 63.0f, c[2] = (p & 31) / 31.0f, c[3] = 1;
}

static void dxt_texel(const Tex *t, uint32_t x, uint32_t y, float c[4])
{
    uint32_t bw = (t->w + 3) / 4;
    unsigned bs = t->fmt == 0x0C ? 8 : 16;
    const uint8_t *b = t->base + ((y / 4) * bw + x / 4) * bs;
    const uint8_t *cb = t->fmt == 0x0C ? b : b + 8;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    unsigned idx = (cb[4 + (y & 3)] >> (2 * (x & 3))) & 3;
    float a[4], bb[4];
    rgb565(c0, a);
    rgb565(c1, bb);
    float alpha = 1;
    if (idx == 0) {
        memcpy(c, a, sizeof(a));
    } else if (idx == 1) {
        memcpy(c, bb, sizeof(bb));
    } else if (c0 > c1 || t->fmt != 0x0C) {
        float f = idx == 2 ? 2.0f / 3 : 1.0f / 3;
        for (int i = 0; i < 3; i++) {
            c[i] = a[i] * f + bb[i] * (1 - f);
        }
    } else if (idx == 2) {
        for (int i = 0; i < 3; i++) {
            c[i] = (a[i] + bb[i]) / 2;
        }
    } else {
        c[0] = c[1] = c[2] = 0;
        alpha = 0;
    }
    if (t->fmt == 0x0E) {
        unsigned n = (b[(y & 3) * 2 + (x & 3) / 2] >> (4 * (x & 1))) & 0xF;
        alpha = n / 15.0f;
    } else if (t->fmt == 0x0F) {
        unsigned a0 = b[0], a1 = b[1];
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) {
            bits |= (uint64_t)b[2 + i] << (8 * i);
        }
        unsigned ai = (bits >> (3 * ((y & 3) * 4 + (x & 3)))) & 7;
        float v;
        if (ai == 0) {
            v = (float)a0;
        } else if (ai == 1) {
            v = (float)a1;
        } else if (a0 > a1) {
            v = ((8 - ai) * a0 + (ai - 1) * a1) / 7.0f;
        } else if (ai < 6) {
            v = ((6 - ai) * a0 + (ai - 1) * a1) / 5.0f;
        } else {
            v = ai == 6 ? 0 : 255;
        }
        alpha = v / 255.0f;
    }
    c[3] = alpha;
}

static void texel(const Tex *t, uint32_t x, uint32_t y, float c[4])
{
    if (t->fmt == 0x0C || t->fmt == 0x0E || t->fmt == 0x0F) {
        dxt_texel(t, x, y, c);
        return;
    }
    unsigned bpp = fmt_bpp(t->fmt);
    const uint8_t *p;
    if (t->linear) {
        if (t->fmt == 0x24 || t->fmt == 0x25) { /* YUY2 / UYVY: dos píxeles en 4 bytes */
            const uint8_t *q = t->base + y * t->pitch + (x & ~1u) * 2;
            float Y, U, V;
            if (t->fmt == 0x24) {
                Y = q[(x & 1) * 2], U = q[1], V = q[3];
            } else {
                Y = q[1 + (x & 1) * 2], U = q[0], V = q[2];
            }
            Y = (Y - 16) * 1.164f, U -= 128, V -= 128;
            c[0] = clamp01((Y + 1.596f * V) / 255);
            c[1] = clamp01((Y - 0.392f * U - 0.813f * V) / 255);
            c[2] = clamp01((Y + 2.017f * U) / 255);
            c[3] = 1;
            return;
        }
        p = t->base + y * t->pitch + x * bpp;
    } else {
        p = t->base + (t->xm[x] | t->ym[y]) * bpp;
    }
    uint32_t v = bpp == 4 ? (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24)
                          : bpp == 2 ? (uint32_t)(p[0] | p[1] << 8) : p[0];
    switch (t->fmt) {
    case 0x00: case 0x13: c[0] = c[1] = c[2] = v / 255.0f, c[3] = 1; break;          /* Y8 */
    case 0x01: case 0x1B: c[0] = c[1] = c[2] = c[3] = v / 255.0f; break;             /* AY8 */
    case 0x19: case 0x1F: c[0] = c[1] = c[2] = 1, c[3] = v / 255.0f; break;          /* A8 */
    case 0x1A: case 0x20: c[0] = c[1] = c[2] = (v & 0xFF) / 255.0f, c[3] = (v >> 8) / 255.0f; break;
    case 0x02: case 0x10:                                                             /* A1R5G5B5 */
        c[0] = ((v >> 10) & 31) / 31.0f, c[1] = ((v >> 5) & 31) / 31.0f, c[2] = (v & 31) / 31.0f;
        c[3] = (v >> 15) ? 1.0f : 0.0f;
        break;
    case 0x03: case 0x1C:
        c[0] = ((v >> 10) & 31) / 31.0f, c[1] = ((v >> 5) & 31) / 31.0f, c[2] = (v & 31) / 31.0f, c[3] = 1;
        break;
    case 0x04: case 0x1D:
        c[0] = ((v >> 8) & 15) / 15.0f, c[1] = ((v >> 4) & 15) / 15.0f, c[2] = (v & 15) / 15.0f;
        c[3] = (v >> 12) / 15.0f;
        break;
    case 0x05: case 0x11: rgb565((uint16_t)v, c); break;
    case 0x07: case 0x1E:
        c[0] = ((v >> 16) & 255) / 255.0f, c[1] = ((v >> 8) & 255) / 255.0f, c[2] = (v & 255) / 255.0f, c[3] = 1;
        break;
    case 0x3A: case 0x3F: /* A8B8G8R8 */
        c[0] = (v & 255) / 255.0f, c[1] = ((v >> 8) & 255) / 255.0f, c[2] = ((v >> 16) & 255) / 255.0f;
        c[3] = (v >> 24) / 255.0f;
        break;
    case 0x3B: case 0x40: /* B8G8R8A8 */
        c[3] = (v & 255) / 255.0f;
        c[0] = ((v >> 8) & 255) / 255.0f, c[1] = ((v >> 16) & 255) / 255.0f, c[2] = (v >> 24) / 255.0f;
        break;
    case 0x3C: case 0x41: /* R8G8B8A8 */
        c[0] = (v >> 24) / 255.0f, c[1] = ((v >> 16) & 255) / 255.0f, c[2] = ((v >> 8) & 255) / 255.0f;
        c[3] = (v & 255) / 255.0f;
        break;
    case 0x28: /* G8B8 */
        c[0] = 0, c[1] = (v >> 8) / 255.0f, c[2] = (v & 255) / 255.0f, c[3] = 1;
        break;
    case 0x0B: /* con paleta (A8R8G8B8) */
        if (t->pal) {
            const uint8_t *e = t->pal + (v < t->pal_n ? v : t->pal_n - 1) * 4;
            c[0] = e[2] / 255.0f, c[1] = e[1] / 255.0f, c[2] = e[0] / 255.0f, c[3] = e[3] / 255.0f;
        } else {
            c[0] = c[1] = c[2] = v / 255.0f, c[3] = 1;
        }
        break;
    default: /* A8R8G8B8 y los demás de 32 bits */
        c[0] = ((v >> 16) & 255) / 255.0f, c[1] = ((v >> 8) & 255) / 255.0f, c[2] = (v & 255) / 255.0f;
        c[3] = (v >> 24) / 255.0f;
        break;
    }
}

static int32_t addr_mode(int32_t i, uint32_t n, unsigned mode)
{
    switch (mode) {
    case 1: /* WRAP */
        i %= (int32_t)n;
        return i < 0 ? i + (int32_t)n : i;
    case 2: { /* MIRROR */
        int32_t p = (int32_t)n * 2;
        i %= p;
        if (i < 0) {
            i += p;
        }
        return i >= (int32_t)n ? p - 1 - i : i;
    }
    default: /* los clamp */
        return i < 0 ? 0 : i >= (int32_t)n ? (int32_t)n - 1 : i;
    }
}

static void sample(const Tex *t, float u, float v, float c[4])
{
    if (t->normalized) {
        u *= t->w;
        v *= t->h;
    }
    if (!isfinite(u) || !isfinite(v)) {
        u = v = 0;
    }
    u = fmaxf(fminf(u, 1e8f), -1e8f);
    v = fmaxf(fminf(v, 1e8f), -1e8f);
    if (!t->bilinear) {
        int32_t x = addr_mode((int32_t)floorf(u), t->w, t->au), y = addr_mode((int32_t)floorf(v), t->h, t->av);
        texel(t, (uint32_t)x, (uint32_t)y, c);
        return;
    }
    u -= 0.5f;
    v -= 0.5f;
    float fu = floorf(u), fv = floorf(v), du = u - fu, dv = v - fv;
    int32_t x0 = addr_mode((int32_t)fu, t->w, t->au), x1 = addr_mode((int32_t)fu + 1, t->w, t->au);
    int32_t y0 = addr_mode((int32_t)fv, t->h, t->av), y1 = addr_mode((int32_t)fv + 1, t->h, t->av);
    float a[4], b[4], d[4], e[4];
    texel(t, (uint32_t)x0, (uint32_t)y0, a);
    texel(t, (uint32_t)x1, (uint32_t)y0, b);
    texel(t, (uint32_t)x0, (uint32_t)y1, d);
    texel(t, (uint32_t)x1, (uint32_t)y1, e);
    for (int i = 0; i < 4; i++) {
        float top = a[i] + (b[i] - a[i]) * du, bot = d[i] + (e[i] - d[i]) * du;
        c[i] = top + (bot - top) * dv;
    }
}

/* -------------------------------------------------------------- combiners */

static void unpack_argb(uint32_t v, float c[4])
{
    c[0] = ((v >> 16) & 255) / 255.0f, c[1] = ((v >> 8) & 255) / 255.0f, c[2] = (v & 255) / 255.0f;
    c[3] = (v >> 24) / 255.0f;
}

static float map_in(float x, unsigned map)
{
    switch (map) {
    case 0: return x < 0 ? 0 : x;                       /* UNSIGNED_IDENTITY */
    case 1: return 1 - clamp01(x);                      /* UNSIGNED_INVERT */
    case 2: return 2 * (x < 0 ? 0 : x) - 1;             /* EXPAND_NORMAL */
    case 3: return -2 * (x < 0 ? 0 : x) + 1;            /* EXPAND_NEGATE */
    case 4: return (x < 0 ? 0 : x) - 0.5f;              /* HALFBIAS_NORMAL */
    case 5: return -(x < 0 ? 0 : x) + 0.5f;             /* HALFBIAS_NEGATE */
    case 6: return x;                                   /* SIGNED_IDENTITY */
    default: return -x;                                 /* SIGNED_NEGATE */
    }
}

/* Un registro de entrada (8 bits: registro 3:0, alfa 4, mapeo 7:5) para la
 * parte RGB (rgb = 1) o la de alfa. */
static void comb_in(float R[16][4], unsigned sel, bool rgb, float *o)
{
    const float *r = R[sel & 0xF];
    bool alpha = (sel >> 4) & 1;
    unsigned map = (sel >> 5) & 7;
    if (rgb) {
        for (int i = 0; i < 3; i++) {
            o[i] = map_in(alpha ? r[3] : r[i], map);
        }
    } else {
        o[0] = map_in(alpha ? r[3] : r[2], map);
    }
}

static float comb_op(float x, unsigned op)
{
    switch (op) {
    case 1: x -= 0.5f; break;
    case 2: x *= 2; break;
    case 3: x = (x - 0.5f) * 2; break;
    case 4: x *= 4; break;
    case 6: x *= 0.5f; break;
    default: break;
    }
    return x < -1 ? -1 : x > 1 ? 1 : x;
}

static void combine(float R[16][4], float out[4])
{
    unsigned n = S.cctl & 0xFF;
    if (n > 8) {
        n = 8;
    }
    bool per_stage = (S.cctl >> 12) & 1;
    bool mux_msb = (S.cctl >> 8) & 1;
    for (unsigned s = 0; s < n; s++) {
        unpack_argb(S.factor0[per_stage ? s : 0], R[1]);
        unpack_argb(S.factor1[per_stage ? s : 0], R[2]);
        /* RGB */
        float a[3], b[3], c[3], d[3];
        uint32_t icw = S.c_icw[s], ocw = S.c_ocw[s];
        comb_in(R, icw >> 24, true, a);
        comb_in(R, (icw >> 16) & 0xFF, true, b);
        comb_in(R, (icw >> 8) & 0xFF, true, c);
        comb_in(R, icw & 0xFF, true, d);
        float ab[3], cd[3], sum[3];
        bool abdot = (ocw >> 13) & 1, cddot = (ocw >> 12) & 1, mux = (ocw >> 14) & 1;
        unsigned op = (ocw >> 15) & 7;
        float dab = a[0] * b[0] + a[1] * b[1] + a[2] * b[2], dcd = c[0] * d[0] + c[1] * d[1] + c[2] * d[2];
        float r0a = R[12][3];
        bool sel_cd = mux_msb ? r0a >= 0.5f : ((int)(r0a * 255) & 1);
        for (int i = 0; i < 3; i++) {
            ab[i] = comb_op(abdot ? dab : a[i] * b[i], op);
            cd[i] = comb_op(cddot ? dcd : c[i] * d[i], op);
            sum[i] = comb_op(mux ? (sel_cd ? (cddot ? dcd : c[i] * d[i]) : (abdot ? dab : a[i] * b[i]))
                                 : (abdot ? dab : a[i] * b[i]) + (cddot ? dcd : c[i] * d[i]),
                             op);
        }
        /* alfa */
        float aa, ba, ca, da;
        uint32_t aicw = S.a_icw[s], aocw = S.a_ocw[s];
        comb_in(R, aicw >> 24, false, &aa);
        comb_in(R, (aicw >> 16) & 0xFF, false, &ba);
        comb_in(R, (aicw >> 8) & 0xFF, false, &ca);
        comb_in(R, aicw & 0xFF, false, &da);
        unsigned aop = (aocw >> 15) & 7;
        float aab = comb_op(aa * ba, aop), acd = comb_op(ca * da, aop);
        float asum = comb_op(((aocw >> 14) & 1) ? (sel_cd ? ca * da : aa * ba) : aa * ba + ca * da, aop);
        /* escribir (las salidas de RGB y luego las de alfa) */
        unsigned dcd_r = ocw & 0xF, dab_r = (ocw >> 4) & 0xF, dsum_r = (ocw >> 8) & 0xF;
        if (dcd_r) {
            memcpy(R[dcd_r], cd, sizeof(cd));
            if ((ocw >> 19) & 1) {
                R[dcd_r][3] = cd[2];
            }
        }
        if (dab_r) {
            memcpy(R[dab_r], ab, sizeof(ab));
            if ((ocw >> 18) & 1) {
                R[dab_r][3] = ab[2];
            }
        }
        if (dsum_r) {
            memcpy(R[dsum_r], sum, sizeof(sum));
        }
        unsigned acd_r = aocw & 0xF, aab_r = (aocw >> 4) & 0xF, asum_r = (aocw >> 8) & 0xF;
        if (acd_r) {
            R[acd_r][3] = acd;
        }
        if (aab_r) {
            R[aab_r][3] = aab;
        }
        if (asum_r) {
            R[asum_r][3] = asum;
        }
    }
    /* el combinador final: A*B + (1-A)*C + D, alfa G */
    for (int i = 0; i < 3; i++) {
        R[14][i] = clamp01(R[5][i]) + clamp01(R[12][i]); /* V1 + R0 */
    }
    float e[3], f[3];
    comb_in(R, S.fcw1 >> 24, true, e);
    comb_in(R, (S.fcw1 >> 16) & 0xFF, true, f);
    for (int i = 0; i < 3; i++) {
        R[15][i] = e[i] * f[i];
    }
    float A[3], B[3], C[3], D[3], G;
    /* en el final el mapeo solo puede invertir (bit 5) */
    comb_in(R, (S.fcw0 >> 24) & 0x3F, true, A);
    comb_in(R, (S.fcw0 >> 16) & 0x3F, true, B);
    comb_in(R, (S.fcw0 >> 8) & 0x3F, true, C);
    comb_in(R, S.fcw0 & 0x3F, true, D);
    comb_in(R, (S.fcw1 >> 8) & 0x3F, false, &G);
    for (int i = 0; i < 3; i++) {
        A[i] = clamp01(A[i]);
        out[i] = clamp01(A[i] * clamp01(B[i]) + (1 - A[i]) * clamp01(C[i]) + clamp01(D[i]));
    }
    out[3] = clamp01(G);
}

/* --------------------------------------------------------------- píxeles */

static bool cmp_func(uint32_t f, float a, float b)
{
    switch (f) {
    case 0x200: return false;
    case 0x201: return a < b;
    case 0x202: return a == b;
    case 0x203: return a <= b;
    case 0x204: return a > b;
    case 0x205: return a != b;
    case 0x206: return a >= b;
    default: return true;
    }
}

static float bfactor(uint32_t f, const float s[4], const float d[4], int i)
{
    float cc[4];
    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 0x300: return s[i];
    case 0x301: return 1 - s[i];
    case 0x302: return s[3];
    case 0x303: return 1 - s[3];
    case 0x304: return d[3];
    case 0x305: return 1 - d[3];
    case 0x306: return d[i];
    case 0x307: return 1 - d[i];
    case 0x308: return i == 3 ? 1 : fminf(s[3], 1 - d[3]);
    case 0x8001: unpack_argb(S.bcolor, cc); return cc[i];
    case 0x8002: unpack_argb(S.bcolor, cc); return 1 - cc[i];
    case 0x8003: unpack_argb(S.bcolor, cc); return cc[3];
    case 0x8004: unpack_argb(S.bcolor, cc); return 1 - cc[3];
    default: return 1;
    }
}

typedef struct Target {
    uint8_t *color;
    uint32_t pitch, bpp, fmt;
    uint8_t *zeta;
    uint32_t zpitch, zfmt;
    float zmax;
    int x0, y0, x1, y1; /* recorte */
} Target;

static void read_px(const Target *t, const uint8_t *p, float d[4])
{
    if (t->bpp == 4) {
        uint32_t v;
        memcpy(&v, p, 4);
        unpack_argb(v, d);
        if (t->fmt != 8) {
            d[3] = 1;
        }
    } else {
        uint16_t v;
        memcpy(&v, p, 2);
        if (t->fmt == 3) {
            rgb565(v, d);
        } else {
            d[0] = ((v >> 10) & 31) / 31.0f, d[1] = ((v >> 5) & 31) / 31.0f, d[2] = (v & 31) / 31.0f, d[3] = 1;
        }
    }
}

static void write_px(const Target *t, uint8_t *p, const float c[4])
{
    if (t->bpp == 4) {
        uint32_t v = (uint32_t)(clamp01(c[3]) * 255 + 0.5f) << 24 | (uint32_t)(clamp01(c[0]) * 255 + 0.5f) << 16 |
                     (uint32_t)(clamp01(c[1]) * 255 + 0.5f) << 8 | (uint32_t)(clamp01(c[2]) * 255 + 0.5f);
        uint32_t m = (S.cmask & 0x01000000u ? 0xFF000000u : 0) | (S.cmask & 0x00010000u ? 0x00FF0000u : 0) |
                     (S.cmask & 0x00000100u ? 0x0000FF00u : 0) | (S.cmask & 0x00000001u ? 0x000000FFu : 0);
        uint32_t old;
        memcpy(&old, p, 4);
        v = (old & ~m) | (v & m);
        memcpy(p, &v, 4);
    } else {
        uint16_t v;
        if (t->fmt == 3) {
            v = (uint16_t)((uint32_t)(clamp01(c[0]) * 31 + 0.5f) << 11 | (uint32_t)(clamp01(c[1]) * 63 + 0.5f) << 5 |
                           (uint32_t)(clamp01(c[2]) * 31 + 0.5f));
        } else {
            v = (uint16_t)(0x8000 | (uint32_t)(clamp01(c[0]) * 31 + 0.5f) << 10 |
                           (uint32_t)(clamp01(c[1]) * 31 + 0.5f) << 5 | (uint32_t)(clamp01(c[2]) * 31 + 0.5f));
        }
        memcpy(p, &v, 2);
    }
}

static Tex g_tex[4];
static bool g_tex_on[4];
static unsigned g_tex_mode[4];

/* Lo que cuenta cada hilo del dibujo (se suma al final). */
typedef struct Count {
    uint64_t pixels;
    unsigned depth_fail, alpha_fail;
    uint32_t first_px;
} Count;

static void shade(const Target *t, int x, int y, const float l[3], const OutVert *v[3], float z, Count *k)
{
    uint8_t *cp = t->color + (uint32_t)y * t->pitch + (uint32_t)x * t->bpp;
    /* profundidad */
    uint8_t *zp = NULL;
    uint32_t zv = 0;
    if (t->zeta && S.dtest) {
        zp = t->zeta + (uint32_t)y * t->zpitch + (uint32_t)x * (t->zfmt == 1 ? 2 : 4);
        float zc = !(z > 0) ? 0 : z > t->zmax ? t->zmax : z;
        uint32_t old;
        if (t->zfmt == 1) {
            uint16_t o16;
            memcpy(&o16, zp, 2);
            old = o16;
            zv = (uint32_t)zc;
        } else {
            memcpy(&old, zp, 4);
            old >>= 8;
            zv = (uint32_t)zc;
        }
        if (!cmp_func(S.dfunc, (float)zv, (float)old)) {
            k->depth_fail++;
            return;
        }
    }
    float R[16][4];
    memset(R, 0, sizeof(R));
    for (int i = 0; i < 4; i++) {
        R[4][i] = clamp01(l[0] * v[0]->col[0][i] + l[1] * v[1]->col[0][i] + l[2] * v[2]->col[0][i]);
        R[5][i] = clamp01(l[0] * v[0]->col[1][i] + l[1] * v[1]->col[1][i] + l[2] * v[2]->col[1][i]);
    }
    unpack_argb(S.fog_color, R[3]);
    R[3][3] = 1; /* sin niebla todavía */
    for (int s = 0; s < 4; s++) {
        if (!g_tex_on[s]) {
            continue;
        }
        float tc[4];
        for (int i = 0; i < 4; i++) {
            tc[i] = l[0] * v[0]->tex[s][i] + l[1] * v[1]->tex[s][i] + l[2] * v[2]->tex[s][i];
        }
        if (g_tex_mode[s] == 4) { /* PASS_THROUGH */
            for (int i = 0; i < 4; i++) {
                R[8 + s][i] = clamp01(tc[i]);
            }
            continue;
        }
        if (g_tex_mode[s] == 3 && g_tex[s].cube) { /* CUBE_MAP: la cara del eje mayor */
            float rx = tc[0], ry = tc[1], rz = tc[2], ax = fabsf(rx), ay = fabsf(ry), az = fabsf(rz);
            float sc, tcc, ma;
            unsigned face;
            if (ax >= ay && ax >= az) {
                face = rx >= 0 ? 0 : 1, ma = ax, sc = rx >= 0 ? -rz : rz, tcc = -ry;
            } else if (ay >= az) {
                face = ry >= 0 ? 2 : 3, ma = ay, sc = rx, tcc = ry >= 0 ? rz : -rz;
            } else {
                face = rz >= 0 ? 4 : 5, ma = az, sc = rz >= 0 ? rx : -rx, tcc = -ry;
            }
            if (!(ma > 0)) {
                ma = 1;
            }
            Tex f = g_tex[s];
            f.base += face * f.face_bytes;
            f.normalized = true;
            sample(&f, (sc / ma + 1) * 0.5f, (tcc / ma + 1) * 0.5f, R[8 + s]);
            continue;
        }
        float u = tc[0], w = tc[1];
        if (g_tex_mode[s] == 1 && tc[3] != 0 && tc[3] != 1) {
            u /= tc[3];
            w /= tc[3];
        }
        sample(&g_tex[s], u, w, R[8 + s]);
    }
    R[12][3] = R[8][3]; /* r0.a empieza con la alfa de la textura 0 */
    float c[4];
    if (S.cctl & 0xFF) {
        combine(R, c);
    } else {
        for (int i = 0; i < 4; i++) {
            c[i] = g_tex_on[0] ? R[8][i] * R[4][i] : R[4][i];
        }
    }
    if (S.atest && !cmp_func(S.afunc, floorf(c[3] * 255 + 0.5f), (float)(S.aref & 0xFF))) {
        k->alpha_fail++;
        return;
    }
    if (k->first_px == 0xFFFFFFFFu) {
        k->first_px = (uint32_t)(clamp01(c[3]) * 255) << 24 | (uint32_t)(clamp01(c[0]) * 255) << 16 |
                             (uint32_t)(clamp01(c[1]) * 255) << 8 | (uint32_t)(clamp01(c[2]) * 255);
    }
    if (S.blend) {
        float d[4], o[4];
        read_px(t, cp, d);
        for (int i = 0; i < 4; i++) {
            float sf = bfactor(S.sfac, c, d, i), df = bfactor(S.dfac, c, d, i);
            float a = c[i] * sf, b = d[i] * df;
            switch (S.beq) {
            case 0x800A: o[i] = a - b; break;
            case 0x800B: o[i] = b - a; break;
            case 0x8007: o[i] = fminf(c[i], d[i]); break;
            case 0x8008: o[i] = fmaxf(c[i], d[i]); break;
            default: o[i] = a + b; break;
            }
        }
        write_px(t, cp, o);
    } else {
        write_px(t, cp, c);
    }
    if (zp && S.dmask) {
        if (t->zfmt == 1) {
            uint16_t z16 = (uint16_t)zv;
            memcpy(zp, &z16, 2);
        } else {
            uint32_t old;
            memcpy(&old, zp, 4);
            uint32_t nz = (zv << 8) | (old & 0xFF);
            memcpy(zp, &nz, 4);
        }
    }
    k->pixels++;
}

/* Los triángulos de un dibujo se preparan primero (recorte, ocultos,
 * rectángulo) y después se pintan todos juntos repartidos entre los hilos
 * de ayuda: cada hilo es dueño de las filas y % n == parte, así cada píxel
 * lo pinta siempre el mismo hilo y en el orden de los triángulos. Antes se
 * despertaba a los hilos por cada triángulo grande (caro) y los chicos iban
 * en un solo núcleo. */
typedef struct TriJob {
    OutVert v[3];
    float iw[3], inv;
    int ix0, ix1, iy0, iy1;
} TriJob;

static struct {
    TriJob *t;
    size_t n, cap;
    uint64_t area;
    const Target *tg;
    Count cnt[8];
} g_tj;

static void raster(const Target *t, const OutVert *a, const OutVert *b, const OutVert *c)
{
    float x0 = a->pos[0], y0 = a->pos[1], x1 = b->pos[0], y1 = b->pos[1], x2 = c->pos[0], y2 = c->pos[1];
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (!(fabsf(area) > 1e-8f) || !isfinite(area)) {
        return;
    }
    if (S.cull) {
        bool cw = area > 0; /* y hacia abajo */
        bool front = S.front_face == 0x900 ? cw : !cw;
        if (S.cull_face == 0x408 || (S.cull_face == 0x404 && front) || (S.cull_face == 0x405 && !front)) {
            S.st.last.culled++;
            return;
        }
    }
    float minx = fminf(x0, fminf(x1, x2)), maxx = fmaxf(x0, fmaxf(x1, x2));
    float miny = fminf(y0, fminf(y1, y2)), maxy = fmaxf(y0, fmaxf(y1, y2));
    minx = fmaxf(minx, -1e6f), miny = fmaxf(miny, -1e6f), maxx = fminf(maxx, 1e6f), maxy = fminf(maxy, 1e6f);
    int ix0 = (int)floorf(minx), ix1 = (int)ceilf(maxx), iy0 = (int)floorf(miny), iy1 = (int)ceilf(maxy);
    if (ix0 < t->x0) ix0 = t->x0;
    if (iy0 < t->y0) iy0 = t->y0;
    if (ix1 > t->x1) ix1 = t->x1;
    if (iy1 > t->y1) iy1 = t->y1;
    if (ix0 >= ix1 || iy0 >= iy1) {
        return;
    }
    if (g_tj.n == g_tj.cap) {
        size_t nc = g_tj.cap ? g_tj.cap * 2 : 1024;
        void *p = realloc(g_tj.t, nc * sizeof(TriJob));
        if (!p) {
            return;
        }
        g_tj.t = p;
        g_tj.cap = nc;
    }
    S.st.triangles++;
    TriJob *j = &g_tj.t[g_tj.n++];
    j->v[0] = *a, j->v[1] = *b, j->v[2] = *c;
    for (int i = 0; i < 3; i++) {
        float w = j->v[i].pos[3];
        j->iw[i] = (w > 1e-20f || w < -1e-20f) && isfinite(w) ? 1.0f / w : 1.0f;
    }
    j->inv = 1.0f / area;
    j->ix0 = ix0, j->ix1 = ix1, j->iy0 = iy0, j->iy1 = iy1;
    g_tj.area += (uint64_t)(ix1 - ix0) * (uint64_t)(iy1 - iy0);
}

static void raster_rows(void *arg, int part, int n);

/* Pinta los triángulos preparados del dibujo. */
static void raster_flush(const Target *t)
{
    if (!g_tj.n) {
        return;
    }
    int n = g_tj.area > 4096 ? workers_count() : 1;
    if (n > 8) {
        n = 8;
    }
    memset(g_tj.cnt, 0, sizeof(g_tj.cnt));
    for (int i = 0; i < 8; i++) {
        g_tj.cnt[i].first_px = 0xFFFFFFFFu;
    }
    g_tj.tg = t;
    if (n > 1) {
        workers_run(raster_rows, NULL);
    } else {
        raster_rows(NULL, 0, 1);
    }
    for (int i = 0; i < n; i++) {
        S.st.pixels += g_tj.cnt[i].pixels;
        S.st.last.depth_fail += g_tj.cnt[i].depth_fail;
        S.st.last.alpha_fail += g_tj.cnt[i].alpha_fail;
        if (S.st.last.first_px == 0xFFFFFFFFu && g_tj.cnt[i].first_px != 0xFFFFFFFFu) {
            S.st.last.first_px = g_tj.cnt[i].first_px;
        }
    }
    g_tj.n = 0;
    g_tj.area = 0;
}

/* Un triángulo recortado contra el plano cercano (w > 0). El vertex shader
 * del XDK ya dividió x, y, z entre w; para recortar se vuelve al espacio
 * homogéneo (x·w, y·w, z·w, w), donde todo es lineal. Sin esto, los
 * triángulos que cruzan la cámara salían como franjas estiradas. */
#define OV_FLOATS ((int)(sizeof(OutVert) / sizeof(float)))

static void ov_hom(const OutVert *v, float *h)
{
    memcpy(h, v, sizeof(OutVert));
    h[0] *= v->pos[3], h[1] *= v->pos[3], h[2] *= v->pos[3];
}

static void ov_back(const float *h, OutVert *v)
{
    memcpy(v, h, sizeof(OutVert));
    float w = h[3];
    v->pos[0] = h[0] / w, v->pos[1] = h[1] / w, v->pos[2] = h[2] / w;
}

static void tri(const Target *t, const OutVert *a, const OutVert *b, const OutVert *c)
{
    const OutVert *v[3] = {a, b, c};
    float wmax = fmaxf(fabsf(a->pos[3]), fmaxf(fabsf(b->pos[3]), fabsf(c->pos[3])));
    if (!isfinite(wmax)) {
        return;
    }
    float eps = wmax * 1e-6f;
    if (!(eps > 0)) {
        eps = 1e-30f;
    }
    int in = 0;
    for (int i = 0; i < 3; i++) {
        in += v[i]->pos[3] > eps;
    }
    if (in == 3) {
        raster(t, a, b, c);
        return;
    }
    if (in == 0) {
        return;
    }
    float hv[3][OV_FLOATS], out[4][OV_FLOATS];
    for (int i = 0; i < 3; i++) {
        ov_hom(v[i], hv[i]);
    }
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const float *p = hv[i], *q = hv[(i + 1) % 3];
        bool pin = p[3] > eps, qin = q[3] > eps;
        if (pin) {
            memcpy(out[n++], p, sizeof(out[0]));
        }
        if (pin != qin) {
            float tt = (p[3] - eps) / (p[3] - q[3]);
            for (int k = 0; k < OV_FLOATS; k++) {
                out[n][k] = p[k] + (q[k] - p[k]) * tt;
            }
            out[n][3] = eps;
            n++;
        }
    }
    OutVert o[4];
    for (int i = 0; i < n; i++) {
        ov_back(out[i], &o[i]);
    }
    for (int i = 1; i + 1 < n; i++) {
        raster(t, &o[0], &o[i], &o[i + 1]);
    }
}

/* Las filas y % n == part de todos los triángulos preparados. */
static void raster_rows(void *arg, int part, int n)
{
    (void)arg;
    if (part >= 8) {
        return;
    }
    const Target *t = g_tj.tg;
    Count *k = &g_tj.cnt[part];
    for (size_t ti = 0; ti < g_tj.n; ti++) {
        const TriJob *j = &g_tj.t[ti];
        const OutVert *a = &j->v[0], *b = &j->v[1], *c = &j->v[2];
        const OutVert *v[3] = {a, b, c};
        float x0 = a->pos[0], y0 = a->pos[1], x1 = b->pos[0], y1 = b->pos[1], x2 = c->pos[0], y2 = c->pos[1];
        const float *iw = j->iw;
        float inv = j->inv;
        int ix0 = j->ix0, ix1 = j->ix1;
        int y = j->iy0 + (((part - j->iy0) % n) + n) % n;
        for (; y < j->iy1; y += n) {
            float py = y + 0.5f;
            /* las funciones de borde son lineales en x: se avanzan sumando */
            float px = ix0 + 0.5f;
            float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
            float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
            float d0 = -(y2 - py) * inv + (y1 - py) * inv, d1 = -(y0 - py) * inv + (y2 - py) * inv;
            for (int x = ix0; x < ix1; x++, w0 += d0, w1 += d1) {
                float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) {
                    continue;
                }
                float z = w0 * a->pos[2] + w1 * b->pos[2] + w2 * c->pos[2];
                float p0 = w0 * iw[0], p1 = w1 * iw[1], p2 = w2 * iw[2], ps = p0 + p1 + p2;
                float l[3] = {w0, w1, w2};
                if (ps != 0 && isfinite(ps)) {
                    float r = 1.0f / ps;
                    l[0] = p0 * r, l[1] = p1 * r, l[2] = p2 * r;
                }
                shade(t, x, y, l, v, z, k);
            }
        }
    }
}

/* ------------------------------------------------------------- un dibujo */

static bool target(Target *t)
{
    memset(t, 0, sizeof(*t));
    unsigned fmt = S.surf_format & 0xF, type = (S.surf_format >> 8) & 0xF, aa = (S.surf_format >> 12) & 0xF;
    t->fmt = fmt;
    t->bpp = (fmt == 1 || fmt == 2 || fmt == 3) ? 2 : (fmt >= 4 && fmt <= 8) ? 4 : 0;
    t->pitch = S.surf_pitch & 0xFFFF;
    S.st.surf_format = S.surf_format, S.st.surf_pitch = S.surf_pitch, S.st.clip_h = S.clip_h, S.st.clip_v = S.clip_v;
    S.st.no_target_why = !t->bpp ? 1 : !t->pitch ? 2 : type != 1 ? 3 : !S.has_color ? 4 : 0;
    if (S.st.no_target_why) {
        return false;
    }
    unsigned fx = aa ? 2 : 1, fy = aa == 2 ? 2 : 1;
    t->x0 = (int)(S.clip_h & 0xFFFF) * (int)fx;
    t->y0 = (int)(S.clip_v & 0xFFFF) * (int)fy;
    t->x1 = t->x0 + (int)((S.clip_h >> 16) * fx);
    t->y1 = t->y0 + (int)((S.clip_v >> 16) * fy);
    if (t->x1 > (int)(t->pitch / t->bpp)) {
        t->x1 = (int)(t->pitch / t->bpp);
    }
    if (t->x1 <= t->x0 || t->y1 <= t->y0) {
        S.st.no_target_why = 5;
        return false;
    }
    t->color = mem(S.dma_color + S.color_off, t->pitch * (uint32_t)t->y1);
    if (!t->color) {
        S.st.no_target_why = 6;
        return false;
    }
    t->zfmt = (S.surf_format >> 4) & 0xF;
    t->zpitch = S.surf_pitch >> 16;
    if (S.has_zeta && t->zpitch && (t->zfmt == 1 || t->zfmt == 2) &&
        t->zpitch >= (uint32_t)t->x1 * (t->zfmt == 1 ? 2u : 4u)) {
        t->zeta = mem(S.dma_zeta + S.zeta_off, t->zpitch * (uint32_t)t->y1);
        t->zmax = t->zfmt == 1 ? 65535.0f : 16777215.0f;
    }
    return true;
}

/* El vertex program puesto: su clave (el hash de sus palabras hasta la marca
 * de fin). Etapa 5: el juego vuelve a cargar el programa muchas veces por
 * cuadro; solo se saca la clave, y el programa se decodifica (prog_parse)
 * cuando de verdad hace falta: la primera vez que se ve, o si lo va a correr
 * la CPU. */
static bool run_program(void)
{
    if (S.pdirty) {
        if (S.pok) {
            nv2a_vsh_program_destroy(&S.prog);
            S.pok = false;
        }
        g_pparsed = false;
        unsigned start = S.pstart < PROG_SLOTS ? S.pstart : 0;
        const uint32_t *w = S.pmem + start * 4;
        unsigned n = 0, max = PROG_SLOTS - start;
        while (n + 1 < max && !(w[n * 4 + 3] & 1u)) {
            n++;
        }
        n++;
        uint64_t h = 1469598103934665603ull;
        for (unsigned i = 0; i < n * 4; i++) {
            h = (h ^ w[i]) * 1099511628211ull;
        }
        S.pkey = h | 1;
        S.pdirty = false;
    }
    return true;
}

/* El programa decodificado en S.prog. false: no es válido. */
static bool prog_parse(void)
{
    if (!g_pparsed) {
        unsigned start = S.pstart < PROG_SLOTS ? S.pstart : 0;
        S.pok = nv2a_vsh_parse_program(&S.prog, S.pmem + start * 4, PROG_SLOTS - start) == NV2AVPR_SUCCESS;
        if (!S.pok) {
            S.st.vs_errors++;
        }
        g_pparsed = true;
    }
    return S.pok;
}

static void transform(const float in[16][4], OutVert *o)
{
    /* el estado a mano: las constantes son las del GPU, sin copiarlas */
    float inr[16 * 4], outr[13 * 4], tmp[12 * 4], a0[4];
    memcpy(inr, in, sizeof(inr));
    memset(outr, 0, sizeof(outr));
    memset(tmp, 0, sizeof(tmp));
    memset(a0, 0, sizeof(a0));
    for (int r = 0; r < 13; r++) {
        outr[r * 4 + 3] = 1.0f;
    }
    outr[3 * 4] = NAN; /* para saber si escribió oD0 */
    Nv2aVshExecutionState st = {inr, outr, tmp, S.consts, a0, NULL};
    nv2a_vsh_emu_execute(&st, &S.prog);
    const float *outp = outr;
    const float *out = outp;
    memcpy(o->pos, out, sizeof(o->pos));
    if (isnan(out[3 * 4])) {
        o->col[0][0] = o->col[0][1] = o->col[0][2] = o->col[0][3] = 1;
    } else {
        memcpy(o->col[0], out + 3 * 4, sizeof(o->col[0]));
    }
    memcpy(o->col[1], out + 4 * 4, sizeof(o->col[1]));
    for (int s = 0; s < 4; s++) {
        memcpy(o->tex[s], out + (9 + s) * 4, sizeof(o->tex[s]));
    }
    o->fog = out[5 * 4];
}

/* ---- etapa 4c: el dibujo a la GPU ---- */

/* Una cara de la textura de la etapa, a ARGB (para subirla a la GPU). */
static inline uint32_t pk8(float x) { return (uint32_t)(clamp01(x) * 255 + 0.5f); }

/* DXT1/3/5 por bloques de 4x4 (las mismas cuentas que dxt_texel, una vez por bloque) */
static void decode_dxt(const Tex *t, uint32_t *out)
{
    uint32_t bw = (t->w + 3) / 4, bh = (t->h + 3) / 4;
    unsigned bs = t->fmt == 0x0C ? 8 : 16;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = t->base + (by * bw + bx) * bs;
            const uint8_t *cb = t->fmt == 0x0C ? b : b + 8;
            uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
            float a[4], bb[4], pal[4][4];
            rgb565(c0, a);
            rgb565(c1, bb);
            float pa[4] = {1, 1, 1, 1};
            memcpy(pal[0], a, sizeof(a));
            memcpy(pal[1], bb, sizeof(bb));
            if (c0 > c1 || t->fmt != 0x0C) {
                for (int k = 2; k < 4; k++) {
                    float f = k == 2 ? 2.0f / 3 : 1.0f / 3;
                    for (int i = 0; i < 3; i++) {
                        pal[k][i] = a[i] * f + bb[i] * (1 - f);
                    }
                }
            } else {
                for (int i = 0; i < 3; i++) {
                    pal[2][i] = (a[i] + bb[i]) / 2;
                    pal[3][i] = 0;
                }
                pa[3] = 0;
            }
            uint32_t rgb[4];
            for (int k = 0; k < 4; k++) {
                rgb[k] = pk8(pal[k][0]) << 16 | pk8(pal[k][1]) << 8 | pk8(pal[k][2]);
            }
            uint32_t a5[8] = {0};
            if (t->fmt == 0x0F) {
                unsigned a0 = b[0], a1 = b[1];
                for (unsigned ai = 0; ai < 8; ai++) {
                    float v;
                    if (ai == 0) {
                        v = (float)a0;
                    } else if (ai == 1) {
                        v = (float)a1;
                    } else if (a0 > a1) {
                        v = ((8 - ai) * a0 + (ai - 1) * a1) / 7.0f;
                    } else if (ai < 6) {
                        v = ((6 - ai) * a0 + (ai - 1) * a1) / 5.0f;
                    } else {
                        v = ai == 6 ? 0 : 255;
                    }
                    a5[ai] = pk8(v / 255.0f);
                }
            }
            uint64_t abits = 0;
            if (t->fmt == 0x0F) {
                for (int i = 0; i < 6; i++) {
                    abits |= (uint64_t)b[2 + i] << (8 * i);
                }
            }
            for (uint32_t yy = 0; yy < 4; yy++) {
                uint32_t y = by * 4 + yy;
                if (y >= t->h) {
                    break;
                }
                for (uint32_t xx = 0; xx < 4; xx++) {
                    uint32_t x = bx * 4 + xx;
                    if (x >= t->w) {
                        break;
                    }
                    unsigned idx = (cb[4 + yy] >> (2 * xx)) & 3;
                    uint32_t al;
                    if (t->fmt == 0x0E) {
                        unsigned n = (b[yy * 2 + xx / 2] >> (4 * (xx & 1))) & 0xF;
                        al = pk8(n / 15.0f);
                    } else if (t->fmt == 0x0F) {
                        al = a5[(abits >> (3 * (yy * 4 + xx))) & 7];
                    } else {
                        al = pk8(pa[idx]);
                    }
                    out[y * t->w + x] = al << 24 | rgb[idx];
                }
            }
        }
    }
}

/* Las de 16 y 32 bits más comunes sin pasar por float (tablas con las mismas
 * cuentas que texel). false: no es una de estas. */
static bool decode_fast(const Tex *t, uint32_t *out)
{
    static uint8_t l5[32], l6[64], l4[16];
    static bool init;
    if (!init) {
        for (int i = 0; i < 32; i++) {
            l5[i] = (uint8_t)pk8(i / 31.0f);
        }
        for (int i = 0; i < 64; i++) {
            l6[i] = (uint8_t)pk8(i / 63.0f);
        }
        for (int i = 0; i < 16; i++) {
            l4[i] = (uint8_t)pk8(i / 15.0f);
        }
        init = true;
    }
    unsigned f = t->fmt, bpp = fmt_bpp(f);
    int kind;
    switch (f) {
    case 0x02: case 0x10: kind = 1; break;   /* A1R5G5B5 */
    case 0x03: case 0x1C: kind = 2; break;   /* X1R5G5B5 */
    case 0x04: case 0x1D: kind = 3; break;   /* A4R4G4B4 */
    case 0x05: case 0x11: kind = 4; break;   /* R5G6B5 */
    case 0x07: case 0x1E: kind = 5; break;   /* X8R8G8B8 */
    case 0x06: case 0x12: kind = 6; break;   /* A8R8G8B8 */
    default: return false;
    }
    if (bpp != (kind <= 4 ? 2u : 4u)) {
        return false;
    }
    for (uint32_t y = 0; y < t->h; y++) {
        const uint8_t *row = t->linear ? t->base + y * t->pitch : NULL;
        uint32_t ym = t->linear ? 0 : t->ym[y];
        for (uint32_t x = 0; x < t->w; x++) {
            const uint8_t *p = row ? row + x * bpp : t->base + (t->xm[x] | ym) * bpp;
            uint32_t v, o;
            if (bpp == 4) {
                v = (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
                o = kind == 5 ? (v | 0xFF000000u) : v;
            } else {
                v = (uint32_t)(p[0] | p[1] << 8);
                switch (kind) {
                case 1:
                    o = (v >> 15 ? 0xFF000000u : 0) | (uint32_t)l5[(v >> 10) & 31] << 16 |
                        (uint32_t)l5[(v >> 5) & 31] << 8 | l5[v & 31];
                    break;
                case 2:
                    o = 0xFF000000u | (uint32_t)l5[(v >> 10) & 31] << 16 | (uint32_t)l5[(v >> 5) & 31] << 8 |
                        l5[v & 31];
                    break;
                case 3:
                    o = (uint32_t)l4[v >> 12] << 24 | (uint32_t)l4[(v >> 8) & 15] << 16 |
                        (uint32_t)l4[(v >> 4) & 15] << 8 | l4[v & 15];
                    break;
                default:
                    o = 0xFF000000u | (uint32_t)l5[v >> 11] << 16 | (uint32_t)l6[(v >> 5) & 63] << 8 | l5[v & 31];
                    break;
                }
            }
            *out++ = o;
        }
    }
    return true;
}

static void gpu_decode(int stage, int face, uint32_t *out)
{
    Tex t = g_tex[stage];
    t.base += (uint32_t)face * t.face_bytes;
    if (t.fmt == 0x0C || t.fmt == 0x0E || t.fmt == 0x0F) {
        decode_dxt(&t, out);
        return;
    }
    if (decode_fast(&t, out)) {
        return;
    }
    for (uint32_t y = 0; y < t.h; y++) {
        for (uint32_t x = 0; x < t.w; x++) {
            float c[4];
            texel(&t, x, y, c);
            *out++ = (uint32_t)(clamp01(c[3]) * 255 + 0.5f) << 24 | (uint32_t)(clamp01(c[0]) * 255 + 0.5f) << 16 |
                     (uint32_t)(clamp01(c[1]) * 255 + 0.5f) << 8 | (uint32_t)(clamp01(c[2]) * 255 + 0.5f);
        }
    }
}

static bool g_vs_draw;
static bool g_vs_gpu = true;
static uint64_t g_vs_key;
static uint64_t g_src_key; /* de qué programa es el texto de g_vs_src */
static uint16_t g_vs_mask;
static char g_vs_src[48 * 1024];

/* Etapa 5: los vértices de los índices pendientes, solo con los atributos del
 * programa (en el orden de sus bits), cada uno una vez, y los índices que ve
 * la GPU. Falso: hacerlo como antes. */
static float *g_pk;
static size_t g_pk_cap;
static uint32_t *g_tidx;
static size_t g_tidx_cap;

static bool grow(void **p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) {
        return true;
    }
    size_t nc = *cap ? *cap : 4096;
    while (nc < need) {
        nc *= 2;
    }
    void *n = realloc(*p, nc * elem);
    if (!n) {
        return false;
    }
    *p = n;
    *cap = nc;
    return true;
}

static bool pk_build(GpuDraw *d)
{
    unsigned prim = S.prim, n = g_nidx;
    if (prim < 5 || prim > 10 || !n) {
        return false;
    }
    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t v = g_idx[i];
        mn = v < mn ? v : mn;
        mx = v > mx ? v : mx;
    }
    uint32_t range = mx - mn + 1;
    bool indexed = range <= 4u * n + 256u;
    unsigned nv = indexed ? range : n;
    unsigned k = (unsigned)__builtin_popcount(g_vs_mask);
    if (!k) {
        k = 1;
    }
    if (!grow((void **)&g_pk, &g_pk_cap, (size_t)nv * k * 4, sizeof(float))) {
        return false;
    }
    unsigned so = k * 4, j = 0;
    for (unsigned a = 0; a < 16; a++) {
        if (!(g_vs_mask & (1u << a))) {
            continue;
        }
        float *dst = g_pk + j * 4;
        j++;
        uint32_t fmt = S.va_fmt[a];
        unsigned nb = attr_bytes(fmt);
        if (!nb) { /* sin arreglo: su valor actual en todos */
            for (unsigned v = 0; v < nv; v++) {
                memcpy(dst + (size_t)v * so, S.cur[a], 16);
            }
            continue;
        }
        uint32_t base = ((S.va_off[a] & 0x80000000u) ? S.dma_vb : S.dma_va) + (S.va_off[a] & 0x7FFFFFFFu);
        uint32_t stride = fmt >> 8;
        if (indexed) {
            const uint8_t *p0 = mem(base + mn * stride, (range - 1) * stride + nb);
            if (!p0) {
                return false;
            }
            if ((fmt & 0xF) == 2 && ((fmt >> 4) & 0xF) <= 4) { /* floats: lo más común */
                unsigned sz = (fmt >> 4) & 0xF;
                for (unsigned v = 0; v < nv; v++) {
                    float *o = dst + (size_t)v * so;
                    o[0] = o[1] = o[2] = 0;
                    o[3] = 1;
                    memcpy(o, p0 + (size_t)v * stride, sz * 4);
                }
            } else {
                for (unsigned v = 0; v < nv; v++) {
                    attr_conv(fmt, p0 + (size_t)v * stride, dst + (size_t)v * so);
                }
            }
        } else {
            for (unsigned v = 0; v < nv; v++) {
                const uint8_t *p = mem(base + g_idx[v] * stride, nb);
                float *o = dst + (size_t)v * so;
                if (p) {
                    attr_conv(fmt, p, o);
                } else {
                    o[0] = o[1] = o[2] = 0;
                    o[3] = 1;
                }
            }
        }
    }
    d->vs_pk = g_pk;
    d->vs_pk_n = nv;
    d->vs_idx = NULL;
    d->vs_nidx = 0;
    d->nverts = n;
    if (indexed) {
        /* los índices (relativos a mn); quads y tiras de quads a triángulos */
        size_t cnt = 0;
        if (!grow((void **)&g_tidx, &g_tidx_cap, (size_t)n * 3 / 2 + 6, sizeof(uint32_t))) {
            return false;
        }
        if (prim == 8) {
            for (unsigned i = 0; i + 3 < n; i += 4) {
                uint32_t q0 = g_idx[i] - mn, q1 = g_idx[i + 1] - mn, q2 = g_idx[i + 2] - mn, q3 = g_idx[i + 3] - mn;
                uint32_t q[6] = {q0, q1, q2, q0, q2, q3};
                memcpy(g_tidx + cnt, q, sizeof(q));
                cnt += 6;
            }
        } else if (prim == 9) {
            for (unsigned i = 0; i + 3 < n; i += 2) {
                uint32_t q0 = g_idx[i] - mn, q1 = g_idx[i + 1] - mn, q2 = g_idx[i + 2] - mn, q3 = g_idx[i + 3] - mn;
                uint32_t q[6] = {q0, q1, q3, q0, q3, q2};
                memcpy(g_tidx + cnt, q, sizeof(q));
                cnt += 6;
            }
        } else {
            for (unsigned i = 0; i < n; i++) {
                g_tidx[i] = g_idx[i] - mn;
            }
            cnt = n;
            if (prim == 5) {
                cnt -= cnt % 3;
            }
        }
        d->vs_idx = g_tidx;
        d->vs_nidx = (unsigned)cnt;
    }
    return true;
}

static bool gpu_try(const Target *t, const OutVert *ov)
{
    _Static_assert(sizeof(OutVert) == GPU_VERT_FLOATS * sizeof(float), "OutVert para la GPU");
    GpuDraw d;
    memset(&d, 0, sizeof(d));
    d.prim = S.prim;
    d.nverts = S.nin;
    d.verts = (const float *)ov;
    d.query = g_zpass_on;
    if (g_vs_draw) {
        if (g_nidx) {
            g_zone = Z_PACK;
            bool pk = pk_build(&d);
            g_zone = Z_SETUP;
            if (!pk) {
                return false;
            }
        } else {
            d.vs_in = &S.in[0][0][0];
        }
        d.vs_mask = g_vs_mask;
        d.vs_consts = S.consts;
        d.vs_key = g_vs_key;
        d.vs_src = g_src_key == g_vs_key ? g_vs_src : NULL; /* el GPU ya lo tiene compilado */
    }
    d.color_pa = (uint32_t)(t->color - S.ram);
    d.pitch = t->pitch;
    d.fmt = t->bpp == 4 ? t->fmt : 0;
    d.w = (int)(t->pitch / (t->bpp ? t->bpp : 4));
    d.h = t->y1;
    if (t->zeta) {
        d.zeta_pa = (uint32_t)(t->zeta - S.ram);
        d.zpitch = t->zpitch;
        d.zfmt = t->zfmt;
    }
    d.zmax = t->zmax > 0 ? t->zmax : (((S.surf_format >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f);
    d.x0 = t->x0, d.y0 = t->y0, d.x1 = t->x1, d.y1 = t->y1;
    d.blend = S.blend, d.dtest = S.dtest, d.dmask = S.dmask, d.cull = S.cull, d.atest = S.atest;
    d.sfac = S.sfac, d.dfac = S.dfac, d.beq = S.beq, d.bcolor = S.bcolor, d.dfunc = S.dfunc;
    d.cull_face = S.cull_face, d.front_face = S.front_face, d.cmask = S.cmask, d.afunc = S.afunc, d.aref = S.aref;
    d.cctl = S.cctl, d.fcw0 = S.fcw0, d.fcw1 = S.fcw1, d.fog_color = S.fog_color;
    memcpy(d.c_icw, S.c_icw, sizeof(d.c_icw));
    memcpy(d.a_icw, S.a_icw, sizeof(d.a_icw));
    memcpy(d.c_ocw, S.c_ocw, sizeof(d.c_ocw));
    memcpy(d.a_ocw, S.a_ocw, sizeof(d.a_ocw));
    memcpy(d.factor0, S.factor0, sizeof(d.factor0));
    memcpy(d.factor1, S.factor1, sizeof(d.factor1));
    for (int s = 0; s < 4; s++) {
        GpuTex *g = &d.tex[s];
        if (!g_tex_on[s]) {
            continue;
        }
        g->on = true;
        g->mode = g_tex_mode[s];
        if (g->mode == 4) {
            continue;
        }
        const Tex *x = &g_tex[s];
        g->addr = (uint32_t)(x->base - S.ram);
        g->fmt = x->fmt, g->w = x->w, g->h = x->h;
        g->cube = x->cube && g->mode == 3;
        g->linear = x->linear;
        g->bilinear = x->bilinear;
        g->au = x->au, g->av = x->av;
        g->data = x->base;
        g->bytes = x->bytes;
        g->pal = x->pal;
        g->pal_bytes = x->pal ? x->pal_n * 4 : 0;
        g->decode = gpu_decode;
        g->stage = s;
    }
    return gpu_draw(&d);
}

static OutVert *g_xf_out;

/* Los vértices i con i % n == part (cada hilo los suyos). */
static void transform_part(void *arg, int part, int n)
{
    (void)arg;
    unsigned chunk = (S.nin + (unsigned)n - 1) / (unsigned)n, i0 = (unsigned)part * chunk;
    unsigned i1 = i0 + chunk < S.nin ? i0 + chunk : S.nin;
    for (unsigned i = i0; i < i1; i++) {
        transform(S.in[i], &g_xf_out[i]);
    }
}

/* 4c: el vertex program como vertex shader en el GPU. El GLSL se hace una vez
 * por programa (y si no se puede traducir, la CPU lo hace como siempre). */

void sw_set_vs_gpu(bool on) { g_vs_gpu = on; }

/* Lo que ya se sabe de cada vertex program visto, por su clave: si tiene
 * versión GLSL y qué entradas usa. Así, cuando el juego vuelve a poner un
 * programa que ya se vio (pasa cientos de veces por cuadro), no hay que
 * decodificarlo ni escribir su GLSL otra vez. */
typedef struct VsEnt {
    uint64_t key;
    uint16_t mask;
    uint8_t st; /* 1: con GLSL · 2: sin traducir (lo hace la CPU) · 3: programa inválido */
} VsEnt;
#define VS_ENTS 4096
static VsEnt g_vsent[VS_ENTS];

static VsEnt *vs_ent(void)
{
    VsEnt *e = &g_vsent[(S.pkey >> 1) & (VS_ENTS - 1)];
    if (e->key != S.pkey) {
        e->key = S.pkey;
        e->mask = 0;
        if (!prog_parse()) {
            e->st = 3;
        } else if (vsh_glsl(&S.prog, g_vs_src, sizeof(g_vs_src), &e->mask)) {
            e->st = 1;
            g_src_key = S.pkey;
        } else {
            e->st = 2;
            g_src_key = 0;
            S.st.vs_untranslated++;
        }
    }
    return e;
}

static bool gpu_try_vs(const Target *t)
{
    if (!g_vs_gpu) {
        return false;
    }
    VsEnt *e = vs_ent();
    if (e->st != 1) {
        return false;
    }
    int gs = gpu_vs_state(S.pkey);
    if (gs == 2) {
        return false;
    }
    if (gs == 0) { /* el GPU todavía no lo tiene: con su texto, que lo compile */
        if (g_src_key != S.pkey) {
            g_src_key = 0;
            if (!prog_parse() || !vsh_glsl(&S.prog, g_vs_src, sizeof(g_vs_src), &e->mask)) {
                return false;
            }
            g_src_key = S.pkey;
        }
        if (!gpu_vs_prepare(S.pkey, g_vs_src)) {
            return false; /* no compila, o le toca en el cuadro siguiente: la CPU */
        }
    }
    g_vs_key = S.pkey;
    g_vs_mask = e->mask;
    g_vs_draw = true;
    bool ok = gpu_try(t, NULL);
    g_vs_draw = false;
    if (ok) {
        S.st.vs_gpu_draws++;
    }
    return ok;
}

static void draw_(void);

static void draw(void)
{
    ZONE_SET(Z_SETUP);
    draw_();
    ZONE_END();
}

static void draw_(void)
{
    if (g_skip) {
        S.st.skipped_frames_draws++;
        return;
    }
    if (!S.nin && !g_nidx) {
        return;
    }
    S.st.draws++;
    if ((S.xform_mode & 3) != 2) {
        S.st.skipped_ff++;
        return;
    }
    Target t;
    if (!target(&t)) {
        S.st.no_target++;
        return;
    }
    run_program(); /* la clave del programa puesto */
    for (int s = 0; s < 4; s++) {
        g_tex_mode[s] = (S.shader_prog >> (5 * s)) & 0x1F;
        g_tex_on[s] = g_tex_mode[s] && ((S.t_ctl0[s] >> 30) & 1);
        if (g_tex_on[s] && g_tex_mode[s] != 4) {
            g_tex_on[s] = tex_setup(s, &g_tex[s]);
        }
    }
    if (gpu_enabled() && gpu_try_vs(&t)) {
        return; /* los vértices los transformó el GPU */
    }
    if (!prog_parse()) {
        return; /* programa inválido */
    }
    if (g_nidx) {
        idx_materialize(); /* la CPU: todos los vértices armados, como antes */
    }
    if (!S.nin) {
        return;
    }
    OutVert *ov = malloc((size_t)S.nin * sizeof(OutVert));
    if (!ov) {
        return;
    }
    g_xf_out = ov;
    g_zone = Z_XFORM;
    if (S.nin >= 192 && workers_count() > 1) {
        workers_run(transform_part, NULL); /* los vértices repartidos entre los hilos */
    } else {
        transform_part(NULL, 0, 1);
    }
    {
        __typeof__(S.st.last) *L = &S.st.last;
        memset(L, 0, sizeof(*L));
        L->prim = S.prim;
        L->nverts = S.nin;
        L->target_pa = (uint32_t)(t.color - S.ram);
        L->target_pitch = t.pitch;
        L->target_fmt = S.surf_format;
        L->zeta_pa = t.zeta ? (uint32_t)(t.zeta - S.ram) : 0;
        L->tex_on = g_tex_on[0];
        L->tex_mode = g_tex_mode[0];
        L->tex_fmt = (S.t_fmt[0] >> 8) & 0xFF;
        if (g_tex_on[0] && g_tex_mode[0] != 4) {
            L->tex_pa = (uint32_t)(g_tex[0].base - S.ram);
            L->tex_w = g_tex[0].w;
            L->tex_h = g_tex[0].h;
        }
        memcpy(L->pos, ov[0].pos, sizeof(L->pos));
        memcpy(L->d0, ov[0].col[0], sizeof(L->d0));
        memcpy(L->t0, ov[0].tex[0], sizeof(L->t0));
        L->cctl = S.cctl;
        L->fcw0 = S.fcw0;
        L->fcw1 = S.fcw1;
        L->first_px = 0xFFFFFFFFu;
    }
    if (gpu_enabled()) {
        if (gpu_try(&t, ov)) {
            free(ov);
            return;
        }
        /* la GPU no pudo: el software, sobre la RAM al día */
        gpu_flush_surface((uint32_t)(t.color - S.ram));
    }
    g_zone = Z_RASTER;
    uint64_t px0 = S.st.pixels, tr0 = S.st.triangles;
    unsigned n = S.nin;
    switch (S.prim) {
    case 5: /* triángulos */
        for (unsigned i = 0; i + 2 < n; i += 3) {
            tri(&t, &ov[i], &ov[i + 1], &ov[i + 2]);
        }
        break;
    case 6: /* tira */
        for (unsigned i = 0; i + 2 < n; i++) {
            if (i & 1) {
                tri(&t, &ov[i + 1], &ov[i], &ov[i + 2]);
            } else {
                tri(&t, &ov[i], &ov[i + 1], &ov[i + 2]);
            }
        }
        break;
    case 7:  /* abanico */
    case 10: /* polígono */
        for (unsigned i = 1; i + 1 < n; i++) {
            tri(&t, &ov[0], &ov[i], &ov[i + 1]);
        }
        break;
    case 8: /* quads */
        for (unsigned i = 0; i + 3 < n; i += 4) {
            tri(&t, &ov[i], &ov[i + 1], &ov[i + 2]);
            tri(&t, &ov[i], &ov[i + 2], &ov[i + 3]);
        }
        break;
    case 9: /* tira de quads */
        for (unsigned i = 0; i + 3 < n; i += 2) {
            tri(&t, &ov[i], &ov[i + 1], &ov[i + 3]);
            tri(&t, &ov[i], &ov[i + 3], &ov[i + 2]);
        }
        break;
    default: /* puntos y líneas: todavía no */
        break;
    }
    raster_flush(&t);
    if (gpu_enabled()) {
        gpu_invalidate((uint32_t)(t.color - S.ram), t.pitch * (uint32_t)t.y1);
    }
    free(ov);
    S.st.last.pixels = S.st.pixels - px0;
    S.st.last.tris = S.st.triangles - tr0;
    if (g_zpass_on) {
        g_zpass_sw += S.st.last.pixels;
    }
}

static void zclear(uint32_t flags)
{
    if (!(flags & 3) || g_skip) {
        return;
    }
    Target t;
    if (!target(&t) || !t.zeta) {
        return;
    }
    unsigned aa = (S.surf_format >> 12) & 0xF, fx = aa ? 2 : 1, fy = aa == 2 ? 2 : 1;
    int x0 = (int)(S.clear_h & 0xFFFF) * (int)fx, x1 = (int)((S.clear_h >> 16) + 1) * (int)fx;
    int y0 = (int)(S.clear_v & 0xFFFF) * (int)fy, y1 = (int)((S.clear_v >> 16) + 1) * (int)fy;
    if (x1 > t.x1) x1 = t.x1;
    if (y1 > t.y1) y1 = t.y1;
    if (gpu_enabled() && gpu_clear_zeta((uint32_t)(t.zeta - S.ram), t.zpitch, t.y1, x0, y0, x1, y1, S.zs_clear, t.zfmt,
                                        flags)) {
        return;
    }
    uint32_t m = t.zfmt == 1 ? 0xFFFF : ((flags & 1 ? 0xFFFFFF00u : 0) | (flags & 2 ? 0xFFu : 0));
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (t.zfmt == 1) {
                uint16_t v = (uint16_t)S.zs_clear;
                memcpy(t.zeta + (uint32_t)y * t.zpitch + (uint32_t)x * 2, &v, 2);
            } else {
                uint8_t *p = t.zeta + (uint32_t)y * t.zpitch + (uint32_t)x * 4;
                uint32_t old;
                memcpy(&old, p, 4);
                uint32_t v = (old & ~m) | (S.zs_clear & m);
                memcpy(p, &v, 4);
            }
        }
    }
}

/* ---------------------------------------------------------------- métodos */

static void set_attr(unsigned a, unsigned comp, float v, bool emit)
{
    if (a >= 16) {
        return;
    }
    S.cur[a][comp] = v;
    if (emit && a == 0 && S.begun) {
        push_vertex(S.cur);
    }
}

static void sw_method_(uint32_t m, uint32_t v);

void sw_method(uint32_t m, uint32_t v)
{
    ZONE_SET(Z_METH);
    sw_method_(m, v);
    ZONE_END();
}

static void sw_method_(uint32_t m, uint32_t v)
{
    if (!S.ram) {
        return;
    }
    switch (m) {
    case 0x0184: S.dma_a = S.dma(v); return;
    case 0x0188: S.dma_b = S.dma(v); return;
    case 0x0194: S.dma_color = S.dma(v); S.has_color = v != 0; return;
    case 0x0198: S.dma_zeta = S.dma(v); S.has_zeta = v != 0; return;
    case 0x019C: S.dma_va = S.dma(v); return;
    case 0x01A0: S.dma_vb = S.dma(v); return;
    case 0x0200: S.clip_h = v; return;
    case 0x0204: S.clip_v = v; return;
    case 0x0208: S.surf_format = v; return;
    case 0x020C: S.surf_pitch = v; return;
    case 0x0210: S.color_off = v; return;
    case 0x0214: S.zeta_off = v; return;
    case 0x0288: S.fcw0 = v; return;
    case 0x028C: S.fcw1 = v; return;
    case 0x02A8: S.fog_color = v; return;
    case 0x0300: S.atest = v & 1; return;
    case 0x0304: S.blend = v & 1; return;
    case 0x0308: S.cull = v & 1; return;
    case 0x030C: S.dtest = v & 1; return;
    case 0x033C: S.afunc = v; return;
    case 0x0340: S.aref = v; return;
    case 0x0344: S.sfac = v; return;
    case 0x0348: S.dfac = v; return;
    case 0x034C: S.bcolor = v; return;
    case 0x0350: S.beq = v; return;
    case 0x0354: S.dfunc = v; return;
    case 0x0358: S.cmask = v; return;
    case 0x035C: S.dmask = v & 1; return;
    case 0x039C: S.cull_face = v; return;
    case 0x03A0: S.front_face = v; return;
    case 0x1D8C: S.zs_clear = v; return;
    case 0x1D94: zclear(v); return;
    case 0x1D98: S.clear_h = v; return;
    case 0x1D9C: S.clear_v = v; return;
    case 0x1E60: S.cctl = v; return;
    case 0x1E70: S.shader_prog = v; return;
    case 0x1E94: S.xform_mode = v; return;
    case 0x1E9C: S.pload = v; S.pword = 0; return;
    case 0x1EA0: S.pstart = v; S.pdirty = true; return;
    case 0x1EA4: S.cload = v; S.cword = 0; return;
    case 0x17C8: /* CLEAR_REPORT_VALUE (1: el de píxeles que pasaron Z) */
        if (v == 1) {
            g_zpass_sw = 0;
            gpu_query_reset();
        }
        return;
    case 0x17CC: g_zpass_on = v & 1; return; /* SET_ZPASS_PIXEL_COUNT_ENABLE */
    case 0x17FC: /* SET_BEGIN_END */
        if (v) {
            S.prim = v;
            S.nin = 0;
            S.ninl = 0;
            g_nidx = 0;
            S.begun = true;
        } else if (S.begun) {
            if (S.ninl) {
                inline_vertices();
            }
            draw();
            S.begun = false;
            S.nin = 0;
            g_nidx = 0;
        }
        return;
    case 0x1800: /* ARRAY_ELEMENT16 */
        array_vertex(v & 0xFFFF);
        array_vertex(v >> 16);
        return;
    case 0x1808: array_vertex(v); return;
    case 0x1810: { /* DRAW_ARRAYS */
        uint32_t start = v & 0xFFFFFF, n = (v >> 24) + 1;
        for (uint32_t i = 0; i < n; i++) {
            array_vertex(start + i);
        }
        return;
    }
    case 0x1818: /* INLINE_ARRAY */
        if (S.ninl >= S.inl_cap) {
            if (S.inl_cap >= (1u << 22)) {
                return;
            }
            unsigned nc = S.inl_cap ? S.inl_cap * 2 : 4096;
            void *n = realloc(S.inl, nc * 4u);
            if (!n) {
                return;
            }
            S.inl = n;
            S.inl_cap = nc;
        }
        S.inl[S.ninl++] = v;
        return;
    default:
        break;
    }
    if (m >= 0x0260 && m < 0x0280) { S.a_icw[(m - 0x0260) / 4] = v; return; }
    if (m >= 0x0A60 && m < 0x0A80) { S.factor0[(m - 0x0A60) / 4] = v; return; }
    if (m >= 0x0A80 && m < 0x0AA0) { S.factor1[(m - 0x0A80) / 4] = v; return; }
    if (m >= 0x0AA0 && m < 0x0AC0) { S.a_ocw[(m - 0x0AA0) / 4] = v; return; }
    if (m >= 0x0AC0 && m < 0x0AE0) { S.c_icw[(m - 0x0AC0) / 4] = v; return; }
    if (m >= 0x1E40 && m < 0x1E60) { S.c_ocw[(m - 0x1E40) / 4] = v; return; }
    if (m >= 0x0A20 && m < 0x0A30) { S.consts[59 * 4 + (m - 0x0A20) / 4] = f32(v); return; }
    if (m >= 0x0AF0 && m < 0x0B00) { S.consts[58 * 4 + (m - 0x0AF0) / 4] = f32(v); return; }
    if (m >= 0x0B00 && m < 0x0B80) { /* SET_TRANSFORM_PROGRAM */
        if (S.pload < PROG_SLOTS) {
            S.pmem[S.pload * 4 + S.pword] = v;
            S.pdirty = true;
        }
        if (++S.pword == 4) {
            S.pword = 0;
            S.pload++;
        }
        return;
    }
    if (m >= 0x0B80 && m < 0x0C00) { /* SET_TRANSFORM_CONSTANT */
        if (S.cload < 192) {
            S.consts[S.cload * 4 + S.cword] = f32(v);
        }
        if (++S.cword == 4) {
            S.cword = 0;
            S.cload++;
        }
        return;
    }
    if (m >= 0x1720 && m < 0x1760) { S.va_off[(m - 0x1720) / 4] = v; return; }
    if (m >= 0x1760 && m < 0x17A0) { S.va_fmt[(m - 0x1760) / 4] = v; return; }
    if (m >= 0x1B00 && m < 0x1C00) { /* texturas: 0x40 por etapa */
        unsigned s = (m - 0x1B00) / 0x40, r = (m - 0x1B00) & 0x3F;
        switch (r) {
        case 0x00: S.t_off[s] = v; break;
        case 0x04: S.t_fmt[s] = v; break;
        case 0x08: S.t_addr[s] = v; break;
        case 0x0C: S.t_ctl0[s] = v; break;
        case 0x10: S.t_ctl1[s] = v; break;
        case 0x14: S.t_filter[s] = v; break;
        case 0x1C: S.t_rect[s] = v; break;
        case 0x20: S.t_pal[s] = v; break;
        default: break;
        }
        return;
    }
    /* vértices inmediatos: escribir el atributo 0 emite el vértice */
    if (m >= 0x1500 && m < 0x150C) { set_attr(0, (m - 0x1500) / 4, f32(v), m == 0x1508); return; }
    if (m >= 0x1518 && m < 0x1528) {
        if (m == 0x1518) {
            S.cur[0][3] = 1;
        }
        set_attr(0, (m - 0x1518) / 4, f32(v), m == 0x1524);
        return;
    }
    if (m >= 0x1880 && m < 0x1900) { /* DATA2F_M */
        unsigned a = (m - 0x1880) / 8, c = ((m - 0x1880) / 4) & 1;
        if (!c) {
            S.cur[a][2] = 0;
            S.cur[a][3] = 1;
        }
        set_attr(a, c, f32(v), c == 1);
        return;
    }
    if (m >= 0x1900 && m < 0x1940) { /* DATA2S */
        unsigned a = (m - 0x1900) / 4;
        S.cur[a][1] = (float)(int16_t)(v >> 16);
        S.cur[a][2] = 0;
        S.cur[a][3] = 1;
        set_attr(a, 0, (float)(int16_t)(v & 0xFFFF), true);
        return;
    }
    if (m >= 0x1940 && m < 0x1980) { /* DATA4UB */
        unsigned a = (m - 0x1940) / 4;
        S.cur[a][1] = ((v >> 8) & 255) / 255.0f;
        S.cur[a][2] = ((v >> 16) & 255) / 255.0f;
        S.cur[a][3] = (v >> 24) / 255.0f;
        set_attr(a, 0, (v & 255) / 255.0f, true);
        return;
    }
    if (m >= 0x1980 && m < 0x1A00) { /* DATA4S_M */
        unsigned a = (m - 0x1980) / 8, c = ((m - 0x1980) / 4) & 1;
        S.cur[a][c * 2] = (float)(int16_t)(v & 0xFFFF);
        set_attr(a, c * 2 + 1, (float)(int16_t)(v >> 16), c == 1);
        return;
    }
    if (m >= 0x1A00 && m < 0x1B00) { /* DATA4F_M */
        unsigned a = (m - 0x1A00) / 16, c = ((m - 0x1A00) / 4) & 3;
        set_attr(a, c, f32(v), c == 3);
        return;
    }
}
