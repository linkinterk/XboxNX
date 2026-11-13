/*
 * XboxNX — en qué se va el tiempo real: cada parte (intérprete, kernel,
 * audio, GPU, pantalla...) suma su tiempo exclusivo (si una parte entra en
 * otra, la de afuera se pausa). Para saber qué optimizar.
 */
#ifndef XBOXNX_PROF_H
#define XBOXNX_PROF_H

#include <stdint.h>

typedef enum {
    PROF_CPU,     /* el intérprete x86 */
    PROF_KERNEL,  /* llamadas al kernel y planificador */
    PROF_APU,     /* el chip de audio (DSP) */
    PROF_GPU,     /* el push buffer y el dibujo por software */
    PROF_HW,      /* el resto del hardware (USB, AC97, vblank) */
    PROF_UI,      /* dibujar la pantalla de la Switch */
    PROF_N
} ProfPart;

void prof_push(ProfPart p);
void prof_pop(void);
/* El reparto desde la última llamada, en milésimas del total; y lo reinicia. */
void prof_take(unsigned permille[PROF_N]);
/* la parte de ahora (desde otro hilo, sin candado); -1: ninguna */
int prof_now(void);
const char *prof_name(ProfPart p);
/* La parte que corre ahora (para el informe de un cierre). */
const char *prof_current(void);
/* Un reloj en ns (para medir). */
uint64_t prof_now_ns(void);
/* Profundidad de la pila de partes; volver a una (tras un cierre atrapado). */
int prof_depth(void);
void prof_reset(int depth);

/* Etapa 5: dentro del GPU, en qué paso está (para el muestreo de sampler.c).
 * ZONE_SET guarda la anterior; ZONE_END la vuelve a poner. */
enum {
    Z_NONE, Z_PB, Z_METH, Z_SETUP, Z_PACK, Z_GL_STATE, Z_GL_VERT, Z_GL_CALL, Z_TEX, Z_QUERY, Z_XFORM, Z_RASTER,
    Z_READ, Z_CLEAR, Z_FLIP, Z_GL_TARGET, Z_GL_TEXS, Z_GL_UNIF, Z_GL_PROG, Z_N
};
extern volatile int g_zone;
const char *zone_name(int z);
#define ZONE_SET(z) int zone_prev_ = g_zone; g_zone = (z)
#define ZONE_END() (g_zone = zone_prev_)

#endif
