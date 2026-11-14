/*
 * XboxNX — etapa 5: muestreo de dónde se va el tiempo del hilo principal
 * (bloques del JIT calientes), a sdmc:/switch/XboxNX/perfil.txt.
 */
#ifndef XBOXNX_SAMPLER_H
#define XBOXNX_SAMPLER_H

#include <stdint.h>

#include "vm.h"

/* cur: Cpu.jit_cur (el bloque que corre ahora, 0 fuera del JIT). */
void sampler_start(const volatile uint32_t *cur, Vm *vm, const char *version);
/* al salir de la app (antes de liberar la RAM del Xbox) */
void sampler_stop(void);

/* Caja negra: cada segundo el hilo del muestreo pide una línea con el estado
 * (fn) y guarda las últimas en caja_negra.txt. Si la app se cierra sin aviso
 * (sin cierre.txt), ahí queda lo que pasaba justo antes. */
void sampler_set_status(void (*fn)(char *out, unsigned n));
/* La salida fue normal: lo anota al final de la caja negra. */
void sampler_mark_clean(void);

#endif
