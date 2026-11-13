/*
 * XboxNX — dibujo del NV2A por software (etapa 4b).
 *
 * Recibe los métodos de Kelvin (la clase 3D, 0x97) del lector del push
 * buffer y dibuja en la memoria del Xbox como lo haría el GPU: vértices
 * (inmediatos, de arreglos o en línea), el vertex shader del juego (con el
 * emulador nv2a_vsh_cpu, dominio público), triángulos con su profundidad,
 * texturas (lineales y swizzle, DXT), los register combiners y la mezcla.
 * Lento comparado con el GPU de la Switch, pero exacto y sencillo: sirve
 * para ver los juegos ya y como referencia para el dibujo por GPU.
 */
#ifndef XBOXNX_NV2A_SW_H
#define XBOXNX_NV2A_SW_H

#include <stdbool.h>
#include <stdint.h>

/* La base física de un objeto DMA por su handle (0 si no existe). */
typedef uint32_t (*SwDmaFn)(uint32_t handle);

typedef struct SwStats {
    uint64_t triangles;    /* dibujados */
    uint64_t pixels;       /* escritos */
    unsigned draws;        /* dibujos hechos */
    unsigned skipped_ff;   /* dibujos con función fija (todavía no) */
    unsigned vs_errors;    /* programas que no se pudieron leer */
    unsigned vs_gpu_draws; /* 4c: dibujos con el vertex shader en el GPU */
    unsigned vs_untranslated; /* programas que no se pudieron pasar a GLSL */
    unsigned no_target;    /* sin superficie donde dibujar */
    unsigned skipped_frames_draws; /* dibujos no hechos por saltar cuadros */
    unsigned no_target_why; /* 1 formato, 2 pitch, 3 swizzle, 4 sin DMA, 5 recorte, 6 memoria */
    uint32_t surf_format, surf_pitch, clip_h, clip_v; /* los de ese momento */
    /* el último dibujo, para entender qué hace el juego */
    struct {
        uint32_t prim, nverts, target_pa, target_pitch, target_fmt, zeta_pa;
        uint32_t tex_pa, tex_fmt, tex_w, tex_h, tex_mode;
        bool tex_on;
        float pos[4], d0[4], t0[4];   /* el primer vértice tras el shader */
        uint32_t first_px;           /* el primer píxel escrito (ARGB) */
        uint64_t pixels, tris;
        uint32_t cctl, fcw0, fcw1;
        unsigned culled, depth_fail, alpha_fail;
    } last;
} SwStats;

void sw_init(uint8_t *ram, uint32_t ram_size, SwDmaFn dma);
/* Un método de Kelvin con su valor. */
void sw_method(uint32_t m, uint32_t v);
const SwStats *sw_stats(void);
/* El estado de Kelvin, para la captura de un cuadro (tools/replay.c). */
#include <stddef.h>
size_t sw_state_size(void);
void sw_state_get(void *out);
void sw_state_set(const void *in);
/* Saltar los dibujos (los cuadros que no se van a ver). */
void sw_set_skip(bool skip);
/* 4c: el vertex program en el GPU (por defecto sí). */
void sw_set_vs_gpu(bool on);

#endif
