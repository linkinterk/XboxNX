/*
 * XboxNX — etapa 3h: el sonido del Xbox por la Switch (audout).
 */
#ifndef XBOXNX_SOUND_H
#define XBOXNX_SOUND_H

#include <stdbool.h>
#include <stdint.h>

/* Conecta la salida del APU y empieza audout (una vez, al abrir la app). */
bool sound_init(void);
void sound_exit(void);
/* Encender / apagar (A en el registro, con el juego corriendo). */
void sound_set_on(bool on);
bool sound_on(void);
/* Una línea para la pantalla. */
const char *sound_info(void);
#ifndef __SWITCH__
/* PC: sacar lo que hay en el anillo (pruebas). */
unsigned sound_take(int16_t *out, unsigned n);
#endif

#endif
