/*
 * XboxNX — memoria del Xbox.
 */
#include "vm.h"

#include <stdlib.h>
#include <string.h>

#define VPAGES (1u << 20)                 /* 4 GB / 4 KB */
#define PPAGES (VM_RAM_SIZE / VM_PAGE)    /* 16384 */

static void *alloc_aligned(size_t align, size_t size)
{
    void *p = NULL;
#if defined(__SWITCH__)
    p = aligned_alloc(align, size);
#else
    if (posix_memalign(&p, align, size) != 0) {
        p = NULL;
    }
#endif
    return p;
}

bool vm_init(Vm *vm)
{
    memset(vm, 0, sizeof(*vm));
    vm->ram = alloc_aligned(VM_PAGE, VM_RAM_SIZE);
    vm->pt = calloc(VPAGES, sizeof(uint32_t));
    vm->used = calloc(PPAGES, 1);
    vm->code = calloc(PPAGES, 1);
    vm->gen = calloc(PPAGES, sizeof(uint32_t));
    if (!vm->ram || !vm->pt || !vm->used || !vm->code || !vm->gen) {
        vm_free(vm);
        return false;
    }
    memset(vm->ram, 0, VM_RAM_SIZE);
    /* El espejo de toda la RAM física en 0x80000000. */
    for (uint32_t p = 0; p < PPAGES; p++) {
        vm->pt[(VM_MIRROR_BASE / VM_PAGE) + p] = p + 1;
    }
    /* La página 0 física queda reservada (nadie debería recibirla). */
    vm->used[0] = 1;
    vm->pages_used = 1;
    vm->next_free = PPAGES - 1;
    return true;
}

void vm_free(Vm *vm)
{
    free(vm->ram);
    free(vm->pt);
    free(vm->used);
    free(vm->code);
    free(vm->gen);
    memset(vm, 0, sizeof(*vm));
}

bool vm_claim_phys(Vm *vm, uint32_t pa, uint32_t len)
{
    if (pa % VM_PAGE || (uint64_t)pa + len > VM_RAM_SIZE) {
        return false;
    }
    uint32_t first = pa / VM_PAGE, n = (len + VM_PAGE - 1) / VM_PAGE;
    for (uint32_t i = 0; i < n; i++) {
        if (vm->used[first + i]) {
            return false;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        vm->used[first + i] = 1;
    }
    vm->pages_used += n;
    return true;
}

/* La memoria virtual toma las páginas libres más altas (como hakuX): los
 * bloques contiguos y los que los juegos ponen en direcciones fijas abajo
 * (Halo, desde 0x61000) quedan libres. */
static uint32_t take_page(Vm *vm)
{
    for (uint32_t i = 0; i < PPAGES; i++) {
        uint32_t p = (vm->next_free + PPAGES - i) % PPAGES;
        if (!vm->used[p]) {
            vm->used[p] = 1;
            vm->pages_used++;
            vm->next_free = p;
            memset(vm->ram + (size_t)p * VM_PAGE, 0, VM_PAGE);
            vm_touch(vm, p);
            return p;
        }
    }
    return VM_NO_PHYS;
}

static void mark(Vm *vm, uint32_t first, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        vm->used[first + i] = 1;
    }
    vm->pages_used += n;
    memset(vm->ram + (size_t)first * VM_PAGE, 0, (size_t)n * VM_PAGE);
    for (uint32_t i = 0; i < n; i++) {
        vm_touch(vm, first + i);
    }
}

uint32_t vm_alloc_phys(Vm *vm, uint32_t size, uint32_t lo, uint32_t hi, uint32_t align)
{
    uint32_t n = (size + VM_PAGE - 1) / VM_PAGE;
    if (!n) {
        n = 1;
    }
    uint32_t step = align > VM_PAGE ? align / VM_PAGE : 1;
    uint32_t last = (hi && hi < VM_RAM_SIZE ? hi : VM_RAM_SIZE) / VM_PAGE;
    uint32_t first = (lo + VM_PAGE - 1) / VM_PAGE;
    first = (first + step - 1) / step * step;
    for (uint32_t p = first; p + n <= last; p += step) {
        uint32_t k = 0;
        while (k < n && !vm->used[p + k]) {
            k++;
        }
        if (k == n) {
            mark(vm, p, n);
            return p * VM_PAGE;
        }
        /* saltar más allá de la página ocupada, al siguiente múltiplo */
        p = ((p + k) / step) * step;
    }
    return VM_NO_PHYS;
}

uint32_t vm_alloc_phys_high(Vm *vm, uint32_t size)
{
    int64_t n = (size + VM_PAGE - 1) / VM_PAGE;
    if (!n || n >= (int64_t)PPAGES) {
        return VM_NO_PHYS;
    }
    int64_t p = (int64_t)PPAGES - n;
    while (p >= 1) {
        int64_t busy = -1;
        for (int64_t k = n - 1; k >= 0; k--) {
            if (vm->used[p + k]) {
                busy = p + k;
                break;
            }
        }
        if (busy < 0) {
            mark(vm, (uint32_t)p, (uint32_t)n);
            return (uint32_t)p * VM_PAGE;
        }
        p = busy - n; /* el bloque termina justo debajo de la ocupada */
    }
    return VM_NO_PHYS;
}

void vm_free_phys(Vm *vm, uint32_t pa, uint32_t size)
{
    uint32_t first = pa / VM_PAGE, n = (size + VM_PAGE - 1) / VM_PAGE;
    for (uint32_t i = 0; i < n && first + i < PPAGES; i++) {
        if (vm->used[first + i] && first + i != 0) {
            vm->used[first + i] = 0;
            vm->pages_used--;
            vm_touch(vm, first + i);
        }
    }
}

void vm_unmap(Vm *vm, uint32_t va, uint32_t len)
{
    uint64_t end = ((uint64_t)va + len + VM_PAGE - 1) & ~(uint64_t)(VM_PAGE - 1);
    for (uint64_t a = va & ~(VM_PAGE - 1); a < end; a += VM_PAGE) {
        if (a >= VM_MIRROR_BASE && a < VM_MIRROR_END) {
            continue;
        }
        uint32_t *e = &vm->pt[a / VM_PAGE];
        if (*e) {
            vm_free_phys(vm, (*e - 1) * VM_PAGE, VM_PAGE);
            *e = 0;
        }
    }
}

uint32_t vm_free_pages(const Vm *vm)
{
    return PPAGES - vm->pages_used;
}

bool vm_alloc_at(Vm *vm, uint32_t va, uint32_t len)
{
    if (!len) {
        return true;
    }
    uint64_t end = ((uint64_t)va + len + VM_PAGE - 1) & ~(uint64_t)(VM_PAGE - 1);
    if (end > VM_MIRROR_BASE && va < VM_MIRROR_END) {
        return false; /* el espejo no se reasigna */
    }
    for (uint64_t a = va & ~(VM_PAGE - 1); a < end; a += VM_PAGE) {
        uint32_t *e = &vm->pt[a / VM_PAGE];
        if (*e) {
            continue;
        }
        uint32_t p = take_page(vm);
        if (p == VM_NO_PHYS) {
            return false;
        }
        *e = p + 1;
    }
    return true;
}

uint32_t vm_phys(const Vm *vm, uint32_t va)
{
    uint32_t e = vm->pt[va / VM_PAGE];
    return e ? (e - 1) * VM_PAGE + (va % VM_PAGE) : VM_NO_PHYS;
}

uint8_t *vm_host(const Vm *vm, uint32_t va)
{
    uint32_t pa = vm_phys(vm, va);
    return pa == VM_NO_PHYS ? NULL : vm->ram + pa;
}

bool vm_read(const Vm *vm, uint32_t va, void *buf, size_t len)
{
    uint8_t *out = buf;
    while (len) {
        uint8_t *h = vm_host(vm, va);
        if (!h) {
            return false;
        }
        size_t n = VM_PAGE - (va % VM_PAGE);
        if (n > len) {
            n = len;
        }
        memcpy(out, h, n);
        out += n;
        va += (uint32_t)n;
        len -= n;
    }
    return true;
}

bool vm_write(Vm *vm, uint32_t va, const void *buf, size_t len)
{
    const uint8_t *in = buf;
    while (len) {
        uint8_t *h = vm_host(vm, va);
        if (!h) {
            return false;
        }
        size_t n = VM_PAGE - (va % VM_PAGE);
        if (n > len) {
            n = len;
        }
        memcpy(h, in, n);
        vm_touch(vm, (uint32_t)((h - vm->ram) / VM_PAGE));
        in += n;
        va += (uint32_t)n;
        len -= n;
    }
    return true;
}

uint32_t vm_rd32(const Vm *vm, uint32_t va)
{
    uint8_t b[4] = {0};
    vm_read(vm, va, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

bool vm_wr32(Vm *vm, uint32_t va, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return vm_write(vm, va, b, 4);
}
