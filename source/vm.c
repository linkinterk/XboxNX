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
    if (!vm->ram || !vm->pt || !vm->used) {
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
    vm->next_free = 1;
    return true;
}

void vm_free(Vm *vm)
{
    free(vm->ram);
    free(vm->pt);
    free(vm->used);
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

static uint32_t take_page(Vm *vm)
{
    for (uint32_t i = 0; i < PPAGES; i++) {
        uint32_t p = (vm->next_free + i) % PPAGES;
        if (!vm->used[p]) {
            vm->used[p] = 1;
            vm->pages_used++;
            vm->next_free = p + 1;
            memset(vm->ram + (size_t)p * VM_PAGE, 0, VM_PAGE);
            return p;
        }
    }
    return VM_NO_PHYS;
}

bool vm_alloc_at(Vm *vm, uint32_t va, uint32_t len)
{
    if (!len) {
        return true;
    }
    uint64_t end = ((uint64_t)va + len + VM_PAGE - 1) & ~(uint64_t)(VM_PAGE - 1);
    if (end > VM_MIRROR_BASE) {
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
