/*
 * XboxNX — volcado del estado para depurar: la RAM del Xbox, su tabla de
 * páginas, los hilos y las llamadas al kernel (texto) y el default.xbe.
 */
#ifndef XBOXNX_DUMP_H
#define XBOXNX_DUMP_H

#include <stdbool.h>
#include <stddef.h>

#include "disc.h"
#include "runner.h"

/* Escribe todo en dir (lo crea). Devuelve false si algo falló; msg dice qué. */
bool dump_state(const Runner *r, const XDisc *disc, const char *dir, char *msg, size_t n);

#endif
