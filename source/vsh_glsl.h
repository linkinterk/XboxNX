/*
 * XboxNX — etapa 4c: un vertex program del NV2A a un vertex shader GLSL.
 */
#ifndef XBOXNX_VSH_GLSL_H
#define XBOXNX_VSH_GLSL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vsh/nv2a_vsh_disassembler.h"
#include "vsh/nv2a_vsh_emulator_execution_state.h"

/* El GLSL del programa en buf (in_mask: las entradas v0..v15 que usa).
 * false: no se puede (escribe en c[], demasiado largo...). */
bool vsh_glsl(const Nv2aVshProgram *p, char *buf, size_t n, uint16_t *in_mask);

#endif
