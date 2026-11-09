/*
 * XboxNX — lo que el APU de hakuX pide del núcleo (native/core), sobre el
 * de XboxNX: el registro, la línea de interrupción, la pausa y el reintento
 * de un acceso (que aquí no hacen falta: los cuadros corren en el mismo hilo
 * que el juego, entre sus instrucciones).
 */
#ifndef XBOXNX_APU_SHIM_H
#define XBOXNX_APU_SHIM_H

#include <stdbool.h>
#include <stdint.h>

void nc_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void nc_irq_set(int line, bool level);
static inline bool nc_paused(void) { return false; }
static inline bool nc_mmio_retry(void) { return false; }

#endif
