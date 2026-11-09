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
    unsigned no_target;    /* sin superficie donde dibujar */
} SwStats;

void sw_init(uint8_t *ram, uint32_t ram_size, SwDmaFn dma);
/* Un método de Kelvin con su valor. */
void sw_method(uint32_t m, uint32_t v);
const SwStats *sw_stats(void);

#endif
