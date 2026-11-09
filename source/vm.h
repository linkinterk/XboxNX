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
#include <string.h>

#define VM_PAGE        4096u
#define VM_RAM_SIZE    (64u * 1024 * 1024)
#define VM_MIRROR_BASE 0x80000000u
#define VM_NO_PHYS     0xFFFFFFFFu
#define VM_MIRROR_END  (VM_MIRROR_BASE + VM_RAM_SIZE)

typedef struct Vm {
    uint8_t *ram;          /* VM_RAM_SIZE bytes, alineados a página */
    uint32_t *pt;          /* página virtual -> página física + 1 (0: sin mapear) */
    uint8_t *used;         /* páginas físicas ocupadas */
    uint32_t pages_used;
    uint32_t next_free;    /* pista para buscar páginas libres */
    /* El código traducido (cpu.c, bloques): una página física con código
     * marcado en code[]; escribirla sube su gen[] y los bloques de ella se
     * vuelven a traducir. */
    uint8_t *code;
    uint32_t *gen;
    /* Qué bytes de cada página con código son instrucciones decodificadas
     * (512 bytes, un bit por byte; solo las páginas que lo necesitan): una
     * escritura en datos de la misma página no obliga a traducir otra vez. */
    uint8_t **cbits;
    uint64_t rewrites; /* veces que se escribió encima de código traducido */
} Vm;

/* Alguien escribió en la página física p (toda). */
static inline void vm_touch(Vm *vm, uint32_t p)
{
    if (vm->code && vm->code[p]) {
        vm->code[p] = 0;
        vm->gen[p]++;
        vm->rewrites++;
        if (vm->cbits && vm->cbits[p]) {
            memset(vm->cbits[p], 0, 512);
        }
    }
}

/* Se escribieron len bytes en off de la página p: solo cuenta si tocan código. */
static inline void vm_touch_range(Vm *vm, uint32_t p, uint32_t off, uint32_t len)
{
    if (!vm->code || !vm->code[p]) {
        return;
    }
    const uint8_t *bm = vm->cbits ? vm->cbits[p] : NULL;
    if (bm) {
        uint32_t end = off + len > 4096 ? 4096 : off + len;
        uint32_t i = off;
        for (; i < end; i++) {
            if (bm[i >> 3] & (1u << (i & 7))) {
                break;
            }
        }
        if (i == end) {
            return; /* solo datos */
        }
    }
    vm_touch(vm, p);
}

/* Los bytes [off, end) de la página p son código decodificado. */
void vm_mark_code(Vm *vm, uint32_t p, uint32_t off, uint32_t end);

bool vm_init(Vm *vm);
void vm_free(Vm *vm);

/* Reserva páginas físicas concretas (el kernel en 0x10000). */
bool vm_claim_phys(Vm *vm, uint32_t pa, uint32_t len);
/* Mapea [va, va+len) a páginas físicas nuevas, en cero. Las ya mapeadas se
 * quedan como están. */
bool vm_alloc_at(Vm *vm, uint32_t va, uint32_t len);

/* Páginas físicas contiguas libres dentro de [lo, hi) (hi 0: hasta el final),
 * alineadas, la más baja que entre; quedan ocupadas y en cero. VM_NO_PHYS si no hay. */
uint32_t vm_alloc_phys(Vm *vm, uint32_t size, uint32_t lo, uint32_t hi, uint32_t align);
/* Igual pero desde arriba (para lo que no debe estorbar a los bloques fijos). */
uint32_t vm_alloc_phys_high(Vm *vm, uint32_t size);
void vm_free_phys(Vm *vm, uint32_t pa, uint32_t size);
/* Quita la memoria de [va, va+len) fuera del espejo: libera sus páginas físicas. */
void vm_unmap(Vm *vm, uint32_t va, uint32_t len);
uint32_t vm_free_pages(const Vm *vm);

uint32_t vm_phys(const Vm *vm, uint32_t va);
/* Puntero a la memoria de va, válido hasta el final de su página; NULL si no
 * está mapeada. */
uint8_t *vm_host(const Vm *vm, uint32_t va);

bool vm_read(const Vm *vm, uint32_t va, void *buf, size_t len);
bool vm_write(Vm *vm, uint32_t va, const void *buf, size_t len);
uint32_t vm_rd32(const Vm *vm, uint32_t va);
bool vm_wr32(Vm *vm, uint32_t va, uint32_t v);

#endif
