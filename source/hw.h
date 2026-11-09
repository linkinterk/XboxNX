/*
 * XboxNX — hardware del Xbox simulado (etapa 3e).
 *
 * Las ventanas de registros (MMIO) de los chips: el GPU NV2A en 0xFD000000,
 * el audio (APU 0xFE800000, AC97 0xFEC00000), USB (0xFED00000/0xFED08000),
 * la red (0xFEF00000) y la flash (0xFF000000); los puertos de E/S (el reloj
 * ACPI, el GPIO del campo de video); las líneas de interrupción.
 *
 * El NV2A: identificación, memoria, su temporizador (PTIMER con alarma), el
 * vblank a 60 Hz con su interrupción (línea 3), la FIFO siempre libre y el
 * lector del push buffer, que por ahora recorre los comandos sin dibujar
 * (eso es la etapa 4) atendiendo lo que D3D espera: objetos, semáforos,
 * informes, los contadores de flip (FLIP_STALL) y las trampas de software.
 *
 * Comportamiento según hakuX (native/dev/nv2a.c, ports.c, ohci.c y
 * hw/xbox/hle/pb, GPL-3.0-or-later); implementación propia.
 */
#ifndef XBOXNX_HW_H
#define XBOXNX_HW_H

#include <stdbool.h>
#include <stdint.h>

struct Kernel;

#define HW_IRQ_USB0 1
#define HW_IRQ_NV2A 3

typedef struct HwStats {
    uint32_t last_addr;     /* el último acceso a un registro */
    uint32_t last_value;
    bool last_write;
    uint64_t accesses;
    uint64_t vblanks;
    uint64_t pb_words;      /* palabras del push buffer recorridas */
    unsigned pb_stall;      /* 0 corre, 1 FLIP_STALL, 2 trampa, 3 FIFO cerrada, 4 error */
    unsigned traps, flips, sem_releases;
} HwStats;

/* Prepara el hardware para una ejecución nueva. */
bool hw_init(struct Kernel *k);
/* Acceso de la CPU a una dirección sin memoria: true si es de un chip. */
bool hw_mmio(void *kernel, uint32_t addr, int size, bool write, uint32_t *value);
uint32_t hw_port_in(void *kernel, uint16_t port, int size);
void hw_port_out(void *kernel, uint16_t port, int size, uint32_t value);
/* Avanza el hardware hasta el tiempo virtual del kernel (vblank, alarma). */
void hw_tick(struct Kernel *k);
/* El próximo evento del hardware (para saltar el tiempo cuando todos esperan). */
bool hw_next_event(const struct Kernel *k, uint64_t *ns);
/* Líneas de interrupción activas (bit = nivel del bus). */
uint32_t hw_irq_lines(const struct Kernel *k);
const HwStats *hw_stats(void);

#endif
