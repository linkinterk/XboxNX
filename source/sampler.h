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

#endif
