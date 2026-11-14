/*
 * XboxNX — el APU del MCPX (0xFE800000, IRQ 5). Ver apu/apu.c.
 */
#ifndef XBOXNX_APU_H
#define XBOXNX_APU_H

#include <stdbool.h>
#include <stdint.h>

struct Kernel;

typedef struct ApuStats {
    uint64_t frames, ep_frames, writes;
    uint64_t ns_busy;   /* v0.18.68: tiempo haciendo cuadros (VP + DSP) */
    uint64_t skipped;   /* cuadros que no se hicieron por ir más de 100 ms atrasado */
    uint64_t lag_ns;    /* lo atrasado que iba la última vez (tiempo del juego) */
    unsigned logs;
    bool gp_running, ep_running;
} ApuStats;

/* La salida: cada cuadro del EP, n muestras estéreo de 16 bits a 48 kHz. */
typedef void (*ApuOutFn)(void *ctx, const int16_t *stereo, unsigned n);

/* Los cuadros en su propio hilo (true en la Switch) o en el del intérprete. */
void apu_set_threaded(bool on);
/* al salir de la app: termina el hilo de los cuadros */
void apu_exit(void);
bool apu_init(struct Kernel *k);
uint32_t apu_read(struct Kernel *k, uint32_t off, int size);
void apu_write(struct Kernel *k, uint32_t off, int size, uint32_t v);
/* Corre los cuadros que tocan hasta el tiempo virtual del kernel. */
void apu_tick(struct Kernel *k);
bool apu_next_frame(uint64_t *ns);
bool apu_irq(void);
const ApuStats *apu_stats(void);
void apu_set_output(ApuOutFn fn, void *ctx);

#endif
