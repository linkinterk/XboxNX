/*
 * XboxNX — los relojes de la Switch (CPU, memoria, GPU) mientras corre la app.
 */
#ifndef XBOXNX_CLOCKS_H
#define XBOXNX_CLOCKS_H

#include <stdbool.h>

/* Sube CPU a 1785 MHz, memoria a 1600 MHz y GPU a lo de los juegos. */
void clocks_raise(void);
/* Los deja como estaban al abrir la app. */
void clocks_restore(void);
/* "CPU 1785 MHz · memoria 1600 MHz · GPU 460 MHz" (para la pantalla) */
const char *clocks_info(void);

#endif
