/*
 * XboxNX — memoria del Xbox (etapa 2a).
 *
 * 64 MB de RAM física y una tabla de páginas para los 4 GB de direcciones
 * de 32 bits del Xbox. Como en el Xbox, toda la RAM física se ve también en
 * 0x80000000 + dirección física (ahí vive el kernel).
 *
 * Inspirado en native/core/vm.c de hakuX (GPL-3.0-or-later); aquí sin mmap
 * para que funcione igual en la Switch y en la PC.
 */
#ifndef XBOXNX_VM_H
#define XBOXNX_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VM_PAGE        4096u
#define VM_RAM_SIZE    (64u * 1024 * 1024)
#define VM_MIRROR_BASE 0x80000000u
#define VM_NO_PHYS     0xFFFFFFFFu

typedef struct Vm {
    uint8_t *ram;          /* VM_RAM_SIZE bytes, alineados a página */
    uint32_t *pt;          /* página virtual -> página física + 1 (0: sin mapear) */
    uint8_t *used;         /* páginas físicas ocupadas */
    uint32_t pages_used;
    uint32_t next_free;    /* pista para buscar páginas libres */
} Vm;

bool vm_init(Vm *vm);
void vm_free(Vm *vm);

/* Reserva páginas físicas concretas (el kernel en 0x10000). */
bool vm_claim_phys(Vm *vm, uint32_t pa, uint32_t len);
/* Mapea [va, va+len) a páginas físicas nuevas, en cero. Las ya mapeadas se
 * quedan como están. */
bool vm_alloc_at(Vm *vm, uint32_t va, uint32_t len);

uint32_t vm_phys(const Vm *vm, uint32_t va);
/* Puntero a la memoria de va, válido hasta el final de su página; NULL si no
 * está mapeada. */
uint8_t *vm_host(const Vm *vm, uint32_t va);

bool vm_read(const Vm *vm, uint32_t va, void *buf, size_t len);
bool vm_write(Vm *vm, uint32_t va, const void *buf, size_t len);
uint32_t vm_rd32(const Vm *vm, uint32_t va);
bool vm_wr32(Vm *vm, uint32_t va, uint32_t v);

#endif
