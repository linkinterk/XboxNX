/*
 * XboxNX — repetir en la PC un cuadro capturado en la Switch (− en el
 * registro: volcado/cuadro_ram.bin y volcado/cuadro.bin) con el dibujo por
 * software, para depurar el dibujo sin la Switch.
 *
 *   replay cuadro_ram.bin cuadro.bin salida.ppm [destino pitch w h] [dibujos]
 *
 * dibujos: parar tras ese número de dibujos (0 = todos). Con LISTA=1 escribe
 * cada dibujo (destino, texturas, modos) a stderr.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_sw.h"
#include "workers.h"

#define RAM (64u << 20)
static uint8_t *g_ram;
static uint32_t g_base; /* la base del objeto DMA del método que se está dando */
static uint32_t dma_rec(uint32_t h) { (void)h; return g_base; }

typedef struct { uint32_t m, v, base; } CapRec;

static uint32_t clear_value, clear_h, clear_v, color_off, color_base, surf_pitch;

static void color_clear(uint32_t flags)
{
    if (!(flags & 0xF0)) {
        return;
    }
    uint32_t mask = (flags & 0x10 ? 0x00FF0000u : 0) | (flags & 0x20 ? 0x0000FF00u : 0) |
                    (flags & 0x40 ? 0x000000FFu : 0) | (flags & 0x80 ? 0xFF000000u : 0);
    uint32_t pitch = surf_pitch & 0xFFFF;
    uint32_t x1 = clear_h & 0xFFFF, x2 = (clear_h >> 16) + 1, y1 = clear_v & 0xFFFF, y2 = (clear_v >> 16) + 1;
    for (uint32_t y = y1; y < y2; y++) {
        for (uint32_t x = x1; x < x2 && x * 4 < pitch; x++) {
            uint32_t a = color_base + color_off + y * pitch + x * 4;
            if (a + 4 > RAM) {
                return;
            }
            uint32_t v;
            memcpy(&v, g_ram + a, 4);
            v = (v & ~mask) | (clear_value & mask);
            memcpy(g_ram + a, &v, 4);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "uso: replay cuadro_ram.bin cuadro.bin salida.ppm [destino pitch w h] [dibujos]\n");
        return 1;
    }
    g_ram = malloc(RAM);
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(g_ram, 1, RAM, f) != RAM) {
        fprintf(stderr, "no se pudo leer %s\n", argv[1]);
        return 1;
    }
    fclose(f);
    f = fopen(argv[2], "rb");
    if (!f) {
        fprintf(stderr, "no se pudo leer %s\n", argv[2]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t n = (size_t)sz / sizeof(CapRec);
    CapRec *rec = malloc((size_t)sz + 1);
    if (fread(rec, sizeof(CapRec), n, f) != n) {
        return 1;
    }
    fclose(f);
    uint32_t dst = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 0x3374000;
    uint32_t pitch = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 2560;
    int w = argc > 6 ? atoi(argv[6]) : 640, h = argc > 7 ? atoi(argv[7]) : 480;
    unsigned stop = argc > 8 ? (unsigned)atoi(argv[8]) : 0;
    bool list = getenv("LISTA") != NULL;
    workers_init();
    sw_init(g_ram, RAM, dma_rec);
    /* lo que suele venir de antes del cuadro: el modo del vertex shader y los
     * objetos DMA (en D3D todos con base 0) */
    g_base = 0;
    sw_method(0x1E94, 2);
    for (uint32_t m = 0x0184; m <= 0x01A0; m += 4) {
        sw_method(m, 1);
    }
    unsigned draws = 0;
    uint32_t tex_fmt[4] = {0}, tex_off[4] = {0}, shader = 0, cctl = 0, prim = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t m = rec[i].m, v = rec[i].v;
        g_base = rec[i].base;
        switch (m) {
        case 0x0194: color_base = rec[i].base; break;
        case 0x1D90: clear_value = v; break;
        case 0x1D98: clear_h = v; break;
        case 0x1D9C: clear_v = v; break;
        case 0x0210: color_off = v; break;
        case 0x020C: surf_pitch = v; break;
        case 0x1D94: color_clear(v); break;
        case 0x1E70: shader = v; break;
        case 0x1E60: cctl = v; break;
        default: break;
        }
        if (m >= 0x1B00 && m < 0x1C00 && ((m - 0x1B00) & 0x3F) == 4) {
            tex_fmt[(m - 0x1B00) / 0x40] = v;
        }
        if (m >= 0x1B00 && m < 0x1C00 && ((m - 0x1B00) & 0x3F) == 0) {
            tex_off[(m - 0x1B00) / 0x40] = v;
        }
        if (m == 0x17FC && v) {
            prim = v;
        }
        if (getenv("SIN_CULL") && m == 0x0308) {
            v = 0;
        }
        if (getenv("CULL_INV") && m == 0x03A0) {
            v ^= 1;
        }
        sw_method(m, v);
        if (m == 0x17FC && !v) {
            draws++;
            if (list) {
                fprintf(stderr, "dibujo %u: prim %u destino %08X modos %X/%X/%X/%X tex", draws, prim,
                        color_base + color_off, shader & 31, (shader >> 5) & 31, (shader >> 10) & 31,
                        (shader >> 15) & 31);
                for (int s = 0; s < 4; s++) {
                    fprintf(stderr, " %02X@%08X", (tex_fmt[s] >> 8) & 0xFF, tex_off[s]);
                }
                const __typeof__(sw_stats()->last) *L = &sw_stats()->last;
                fprintf(stderr, " cctl %X v %u tri %llu px %llu ocultos %u z %u alfa %u pos %.1f %.1f %.1f %.1f\n", cctl, L->nverts,
                        (unsigned long long)L->tris, (unsigned long long)L->pixels, L->culled, L->depth_fail, L->alpha_fail, L->pos[0], L->pos[1],
                        L->pos[2], L->pos[3]);
            }
            if (stop && draws >= stop) {
                break;
            }
        }
    }
    const SwStats *st = sw_stats();
    fprintf(stderr, "%zu métodos, %u dibujos, %llu triángulos, %llu píxeles, sin destino %u, vs con error %u, ff %u\n", n,
            st->draws, (unsigned long long)st->triangles, (unsigned long long)st->pixels, st->no_target, st->vs_errors,
            st->skipped_ff);
    FILE *o = fopen(argv[3], "wb");
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t v;
            memcpy(&v, g_ram + ((dst + (uint32_t)y * pitch + (uint32_t)x * 4) & (RAM - 1)), 4);
            uint8_t px[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
            fwrite(px, 1, 3, o);
        }
    }
    fclose(o);
    return 0;
}
