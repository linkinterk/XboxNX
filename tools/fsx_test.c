/* v0.18.76: los fragment shaders a la medida contra el común.
 * Combinaciones de register combiners al azar, cada una dibujada con los dos
 * y comparada píxel a píxel (en la PC con mesa).
 *   uso: fsx_test [casos] [semilla] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gpu.h"

#define RAM (16u << 20)
#define W 64
static uint8_t *ram;
static const uint32_t pa = 0x100000;
static uint32_t rs = 1;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}
static float rf(void) { return (float)(rnd() % 1001) / 1000.0f; }

static void vert(float *v, float x, float y)
{
    memset(v, 0, GPU_VERT_FLOATS * 4);
    v[0] = x, v[1] = y, v[2] = rf() * 0.5f, v[3] = 1;
    for (int i = 4; i < 28; i++) {
        v[i] = rf() * 1.2f - 0.1f; /* colores y coordenadas, a veces fuera de 0..1 */
    }
    v[28] = rf() * 2.0f - 0.5f;
}

static uint8_t tex_bytes[256];
static void decode(const void *ctx, int face, uint32_t *out)
{
    (void)ctx;
    (void)face;
    for (int i = 0; i < 64; i++) {
        /* v0.18.85: una de cada 5 con alfa 0 (ALPHAKILLEN) */
        out[i] = 0x01000000u * (uint32_t)(i % 5 == 0 ? 0 : i * 4) | (uint32_t)(i * 37 % 256) << 16 | (uint32_t)(i * 91 % 256) << 8 |
                 (uint32_t)(255 - i * 3);
    }
}

static uint8_t sel(void)
{
    static const uint8_t regs[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
    return (uint8_t)(regs[rnd() % sizeof(regs)] | (rnd() & 1u) << 4 | (rnd() & 7u) << 5);
}
static uint32_t icw(void) { return (uint32_t)sel() << 24 | (uint32_t)sel() << 16 | (uint32_t)sel() << 8 | sel(); }
static uint32_t ocw(void)
{
    static const uint8_t dst[] = {0, 0, 4, 5, 8, 9, 12, 13};
    uint32_t v = dst[rnd() % 8] | (uint32_t)dst[rnd() % 8] << 4 | (uint32_t)dst[rnd() % 8] << 8;
    v |= (rnd() & 0xFu) << 12;   /* dot cd, dot ab, mux, op (bit 15) */
    v |= (rnd() & 0x3u) << 16;   /* el resto de op */
    v |= (rnd() & 0x3u) << 18;   /* azul a alfa */
    return v;
}

int main(int argc, char **argv)
{
    int cases = argc > 1 ? atoi(argv[1]) : 300;
    rs = argc > 2 ? (uint32_t)atoi(argv[2]) : 12345u;
    ram = calloc(1, RAM);
    if (!gpu_init(NULL)) {
        fprintf(stderr, "sin GPU\n");
        return 1;
    }
    gpu_set_ram(ram, RAM);
    gpu_enable(true);
    gpu_set_link_budget(~0ull);
    for (int i = 0; i < 256; i++) {
        tex_bytes[i] = (uint8_t)(i * 7);
    }
    int bad = 0;
    uint32_t *a = malloc(W * W * 4), *b = malloc(W * W * 4);
    float v[6 * GPU_VERT_FLOATS];
    for (int c = 0; c < cases; c++) {
        GpuDraw d;
        memset(&d, 0, sizeof(d));
        d.color_pa = pa, d.pitch = W * 4, d.fmt = 8, d.w = W, d.h = W;
        d.x0 = 0, d.y0 = 0, d.x1 = W, d.y1 = W;
        d.cmask = 0x01010101u;
        d.zmax = 1.0f;
        d.psize = 1.0f;
        d.prim = 5, d.nverts = 6, d.verts = v;
        vert(v, 0, 0);
        vert(v + GPU_VERT_FLOATS, W, 0);
        vert(v + 2 * GPU_VERT_FLOATS, 0, W);
        memcpy(v + 3 * GPU_VERT_FLOATS, v + GPU_VERT_FLOATS, GPU_VERT_FLOATS * 4);
        vert(v + 4 * GPU_VERT_FLOATS, W, W);
        memcpy(v + 5 * GPU_VERT_FLOATS, v + 2 * GPU_VERT_FLOATS, GPU_VERT_FLOATS * 4);
        unsigned n = rnd() % 10 == 0 ? 0 : 1 + rnd() % 8;
        d.cctl = n | (rnd() & 1u) << 8 | (rnd() & 1u) << 12;
        for (int s = 0; s < 8; s++) {
            d.c_icw[s] = icw(), d.a_icw[s] = icw();
            d.c_ocw[s] = ocw(), d.a_ocw[s] = ocw() & ~0x3000u;
            d.factor0[s] = rnd(), d.factor1[s] = rnd();
        }
        d.fcw0 = icw() & 0x3F3F3F3Fu;
        d.fcw1 = (uint32_t)sel() << 24 | (uint32_t)sel() << 16 | (uint32_t)(sel() & 0x3F) << 8 | (rnd() & 0x60u);
        d.fc0 = rnd(), d.fc1 = rnd();
        d.fog_color = rnd();
        d.spec = rnd() & 1u, d.spec_a1 = rnd() & 1u;
        d.fog = rnd() % 3 == 0;
        static const uint32_t fm[] = {0x2601, 0x800, 0x801, 0x802, 0x803, 0x804};
        d.fog_mode = fm[rnd() % 6];
        d.fog_p[0] = rf() * 2, d.fog_p[1] = rf() * 2 - 1;
        d.atest = rnd() % 4 == 0;
        d.afunc = 0x200 + rnd() % 8;
        d.aref = rnd() & 0xFF;
        static const unsigned modes[] = {0, 4, 4, 1, 2, 12};
        for (int i = 0; i < 4; i++) {
            GpuTex *t = &d.tex[i];
            unsigned m = modes[rnd() % 6];
            if (!m) {
                continue;
            }
            t->on = true;
            t->mode = m;
            if (m != 4) {
                t->addr = 0x200000 + 0x1000u * (uint32_t)i, t->fmt = 6, t->w = 8, t->h = 8;
                t->au = t->av = 1;
                t->data = tex_bytes, t->bytes = sizeof(tex_bytes);
                t->decode = decode;
                t->akill = rnd() % 3 == 0;
            }
        }
        for (int pass = 0; pass < 2; pass++) {
            gpu_fsx_set(pass ? 2 : 0);
            gpu_clear_color(pa, W * 4, W, 0, 0, W, W, 0x80402010u, 0xF);
            gpu_draw(&d);
            gpu_flush_all();
            memcpy(pass ? b : a, ram + pa, W * W * 4);
        }
        int diff = 0, maxd = 0;
        for (int i = 0; i < W * W; i++) {
            int m = 0;
            for (int k = 0; k < 32; k += 8) {
                int e = abs((int)((a[i] >> k) & 0xFF) - (int)((b[i] >> k) & 0xFF));
                m = e > m ? e : m;
            }
            maxd = m > maxd ? m : maxd;
            diff += m > 2;
        }
        if (diff > W * W / 50) { /* hasta 2%: un umbral (r0.a >= 0.5) que cae justo en el borde */
            bad++;
            fprintf(stderr, "caso %d: %d píxeles distintos (máx %d) · cctl %08X fcw %08X %08X tex %u %u %u %u\n", c,
                    diff, maxd, d.cctl, d.fcw0, d.fcw1, d.tex[0].mode * d.tex[0].on, d.tex[1].mode * d.tex[1].on,
                    d.tex[2].mode * d.tex[2].on, d.tex[3].mode * d.tex[3].on);
        }
    }
    const GpuStats *g = gpu_stats();
    printf("%d casos, %d distintos · a la medida: %u compilados, %u sin compilar, %llu dibujos\n", cases, bad,
           g->fsx_built, g->fsx_bad, (unsigned long long)g->fsx_draws);
    return bad != 0;
}
