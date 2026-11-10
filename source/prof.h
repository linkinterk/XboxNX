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
const char *prof_name(ProfPart p);
/* La parte que corre ahora (para el informe de un cierre). */
const char *prof_current(void);
/* Profundidad de la pila de partes; volver a una (tras un cierre atrapado). */
int prof_depth(void);
void prof_reset(int depth);

#endif
