/*
 * XboxNX — el AC97 del MCPX (0xFEC00000, IRQ 6). Ver aci.c.
 */
#ifndef XBOXNX_ACI_H
#define XBOXNX_ACI_H

#include <stdbool.h>
#include <stdint.h>

struct Kernel;

void aci_init(struct Kernel *k);
uint32_t aci_read(struct Kernel *k, uint32_t off, int size);
void aci_write(struct Kernel *k, uint32_t off, int size, uint32_t v);
/* El PCM que se fue consumiendo hasta el tiempo virtual del kernel. */
void aci_tick(struct Kernel *k);
bool aci_irq(void);
uint64_t aci_pcm_bytes(void);

#endif
