/*
 * XboxNX — pruebas de hardware para saber si la etapa 2 (CPU x86 sobre JIT)
 * es posible en esta Switch.
 */
#ifndef XBOXNX_PROBE_H
#define XBOXNX_PROBE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ProbeResult {
    /* Modo de arranque */
    bool applet_mode;          /* true: abierto desde el Album (poca RAM) */
    /* Memoria */
    uint64_t mem_total;
    uint64_t mem_used;
    uint64_t biggest_alloc;    /* el bloque más grande que se pudo pedir */
    /* JIT */
    bool jit_ok;
    uint32_t jit_rc;
    int jit_value;             /* 42 si el código generado corrió */
    /* Espacio de direcciones */
    bool va4g_ok;              /* se encontró una ventana de 4 GiB libre */
} ProbeResult;

void probe_run(ProbeResult *r);

/* Prueba de excepciones: provoca un acceso inválido y lo atrapa. Si la
 * Switch no lo permite, la app se cierra (eso también es un resultado). */
bool probe_exception(uint32_t *desc, uint64_t *far);

#endif
