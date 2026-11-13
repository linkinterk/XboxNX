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
#include "xpr.h"

struct Kernel;

#define HW_IRQ_USB0 1
#define HW_IRQ_NV2A 3
#define HW_IRQ_APU  5
#define HW_IRQ_ACI  6

#define HW_RECENT 4

/* Lo que el juego dibujó en un cuadro (entre dos flips). */
typedef struct HwDraws {
    unsigned draws;      /* SET_BEGIN_END con primitiva */
    unsigned vertices;
    unsigned program;    /* dibujos con vertex shader (los otros: función fija) */
    unsigned textured;   /* dibujos con alguna textura */
    unsigned immediate;  /* vértices sueltos (SET_VERTEX4F) */
    unsigned prims;      /* bits: 1 puntos, 2 líneas... 5 triángulos, 6 tira, 7 abanico, 8 quads */
    uint8_t formats[8];  /* formatos de textura vistos */
    unsigned nformats;
} HwDraws;

typedef struct HwStats {
    uint32_t last_addr;     /* el último acceso a un registro */
    uint32_t last_value;
    bool last_write;
    uint64_t accesses;
    uint64_t vblanks;
    uint64_t pb_words;      /* palabras del push buffer recorridas */
    unsigned pb_stall;      /* 0 corre, 1 FLIP_STALL, 2 trampa, 3 FIFO cerrada, 4 error */
    unsigned traps, flips, sem_releases;
    unsigned frames;        /* cambios de PCRTC_START: cuadros mostrados */
    unsigned clears;        /* CLEAR_SURFACE hechos */
    unsigned reports, zpass_tests, reports_noinst; /* GET_REPORT recibidos, pruebas de visibilidad (CLEAR_REPORT_VALUE), informes sin DMA */
    unsigned av_modes;      /* llamadas a AvSetDisplayMode */
    HwDraws last;           /* el último cuadro */
    uint64_t total_draws;
    /* los registros distintos tocados hace poco (el más reciente primero) */
    struct {
        uint32_t addr, value, count;
        bool write;
    } recent[HW_RECENT];
} HwStats;

/* El modo de video que pidió el juego (AvSetDisplayMode). */
void hw_set_display(struct Kernel *k, uint32_t format, uint32_t pitch, uint32_t fb);
/* La imagen que el Xbox muestra, en RGBA (px con malloc, reutilizado). */
bool hw_screen(const struct Kernel *k, XImage *out);
/* La dirección física que se muestra (PCRTC_START). */
uint32_t hw_scanout(void);
/* La dirección que hw_screen mostró y el destino del último cuadro terminado. */
uint32_t hw_shown(void);
/* Capturar el próximo cuadro entero (la RAM al empezar y cada método de
 * Kelvin) en dir/cuadro_ram.bin y dir/cuadro.bin. Estado: 0 nada, 1 esperando,
 * 2 capturando, 3 guardado, -1 error. */
void hw_capture_arm(const char *dir);
/* Dibujar solo 1 de cada n cuadros (el juego corre más rápido; 1 = todos). */
void hw_set_frameskip(unsigned n);
unsigned hw_frameskip(void);
int hw_capture_state(void);
uint32_t hw_last_target(void);

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
void hw_irq_debug(char *b, size_t n);

#endif
