/*
 * XboxNX — kernel de Xbox, etapa 3a: memoria.
 *
 * Memoria contigua (vista en 0x80000000 + física), memoria virtual del juego
 * (reservas de 64 KB en 0x00010000..0x7FFE0000, páginas al hacer commit),
 * pool del kernel y pilas en el espacio del sistema.
 *
 * Diseño y comportamiento basados en hakuX (native/kernel/mm.c, pool.c y
 * ex.c, GPL-3.0-or-later); implementación propia sobre vm.c.
 */
#include <string.h>

#include "cpu.h"
#include "kernel.h"

#define MEM_COMMIT   0x1000u
#define MEM_RESERVE  0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE  0x8000u
#define MEM_FREE     0x10000u
#define MEM_PRIVATE  0x20000u
#define MEM_TOP_DOWN 0x100000u
#define MEM_IMAGE    0x1000000u
#define PAGE_NOACCESS  0x01u
#define PAGE_READWRITE 0x04u

#define USER_LO  0x00010000u
#define USER_HI  0x7FFE0000u
#define VA_GRAIN 0x10000u

#define POOL_PA    (K_PA + K_SIZE)  /* justo encima del kernel: 0x50000 */
#define POOL_SIZE  0x10000u          /* termina antes del bloque fijo de Halo (0x61000) */
#define POOL_ALIGN 16u
#define POOL_CHUNK 0x10000u
#define POOL_HDR   16u               /* tamaño del bloque antes de cada uno */
#define POOL_PAGES 0x53454741u       /* bloque de páginas propias, fuera del pool */

#define STACK_AREA 0xD0000000u
#define STACK_END  0xE0000000u

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

/* ------------------------------------------------------------- pool */

bool kmm_pool_init(Kernel *k)
{
    if (!vm_claim_phys(k->vm, POOL_PA, POOL_SIZE)) {
        return false;
    }
    memset(k->vm->ram + POOL_PA, 0, POOL_SIZE);
    k->pool_free[0] = (KRange){VM_MIRROR_BASE | POOL_PA, POOL_SIZE};
    k->pool_nfree = 1;
    k->pool_len = POOL_SIZE;
    k->stack_next = STACK_AREA;
    return true;
}

void kmm_pool_free(Kernel *k, uint32_t va, uint32_t size)
{
    size = align_up(size ? size : 1, POOL_ALIGN);
    unsigned i = 0;
    while (i < k->pool_nfree && k->pool_free[i].va < va) {
        i++;
    }
    bool prev = i > 0 && k->pool_free[i - 1].va + k->pool_free[i - 1].len == va;
    bool next = i < k->pool_nfree && va + size == k->pool_free[i].va;
    if (prev && next) {
        k->pool_free[i - 1].len += size + k->pool_free[i].len;
        memmove(&k->pool_free[i], &k->pool_free[i + 1], (k->pool_nfree - i - 1) * sizeof(KRange));
        k->pool_nfree--;
    } else if (prev) {
        k->pool_free[i - 1].len += size;
    } else if (next) {
        k->pool_free[i].va = va;
        k->pool_free[i].len += size;
    } else if (k->pool_nfree < K_POOL_RANGES) {
        memmove(&k->pool_free[i + 1], &k->pool_free[i], (k->pool_nfree - i) * sizeof(KRange));
        k->pool_free[i] = (KRange){va, size};
        k->pool_nfree++;
    }
    k->pool_used -= size < k->pool_used ? size : k->pool_used;
}

uint32_t kmm_pool_alloc(Kernel *k, uint32_t size)
{
    size = align_up(size ? size : 1, POOL_ALIGN);
    for (int tries = 0; tries < 2; tries++) {
        for (unsigned i = 0; i < k->pool_nfree; i++) {
            KRange *r = &k->pool_free[i];
            if (r->len < size) {
                continue;
            }
            uint32_t va = r->va;
            r->va += size;
            r->len -= size;
            if (!r->len) {
                memmove(r, r + 1, (k->pool_nfree - i - 1) * sizeof(*r));
                k->pool_nfree--;
            }
            memset(vm_host(k->vm, va), 0, size); /* el pool es contiguo en física */
            k->pool_used += size;
            return va;
        }
        /* crecer: un trozo de RAM desde arriba */
        uint32_t chunk = align_up(size > POOL_CHUNK ? size : POOL_CHUNK, POOL_CHUNK);
        uint32_t pa = vm_alloc_phys_high(k->vm, chunk);
        if (pa == VM_NO_PHYS) {
            return 0;
        }
        k->pool_len += chunk;
        k->pool_used += chunk; /* kmm_pool_free lo descuenta */
        kmm_pool_free(k, VM_MIRROR_BASE | pa, chunk);
    }
    return 0;
}

/* ExAllocatePool: un bloque de una página o más toma páginas propias y las
 * devuelve al liberarse (como MiAllocatePoolPages). */
static uint32_t ex_alloc(Kernel *k, uint32_t size)
{
    if (size + POOL_HDR > VM_PAGE) {
        uint32_t len = align_up(size + POOL_HDR, VM_PAGE);
        uint32_t pa = vm_alloc_phys_high(k->vm, len);
        if (pa != VM_NO_PHYS) {
            uint32_t p = VM_MIRROR_BASE | pa;
            vm_wr32(k->vm, p, size);
            vm_wr32(k->vm, p + 4, POOL_PAGES);
            return p + POOL_HDR;
        }
    }
    uint32_t p = kmm_pool_alloc(k, size + POOL_HDR);
    if (!p) {
        return 0;
    }
    vm_wr32(k->vm, p, size);
    return p + POOL_HDR;
}

static uint64_t ExAllocatePool(KCall *c) { return ex_alloc(c->k, c->a[0]); }
static uint64_t ExAllocatePoolWithTag(KCall *c) { return ex_alloc(c->k, c->a[0]); }

static uint64_t ExFreePool(KCall *c)
{
    if (!c->a[0]) {
        return 0;
    }
    uint32_t hdr = c->a[0] - POOL_HDR;
    uint32_t size = vm_rd32(c->vm, hdr) + POOL_HDR;
    if (vm_rd32(c->vm, hdr + 4) == POOL_PAGES) {
        vm_wr32(c->vm, hdr + 4, 0);
        vm_free_phys(c->vm, hdr & ~VM_MIRROR_BASE, align_up(size, VM_PAGE));
    } else {
        kmm_pool_free(c->k, hdr, size);
    }
    return 0;
}

static uint64_t ExQueryPoolBlockSize(KCall *c) { return vm_rd32(c->vm, c->a[0] - POOL_HDR); }

/* ------------------------------------------------------- contigua */

static KContig *contig_at(Kernel *k, uint32_t va)
{
    for (unsigned i = 0; i < k->ncontig; i++) {
        if (k->contig[i].va == va) {
            return &k->contig[i];
        }
    }
    return NULL;
}

static KContig *contig_holding(Kernel *k, uint32_t va)
{
    for (unsigned i = 0; i < k->ncontig; i++) {
        if (va - k->contig[i].va < k->contig[i].size) {
            return &k->contig[i];
        }
    }
    return NULL;
}

static uint32_t contig_alloc(Kernel *k, uint32_t size, uint32_t lo, uint32_t hi, uint32_t align, uint32_t prot)
{
    if (!size || k->ncontig >= K_MAX_CONTIG) {
        return 0;
    }
    size = align_up(size, VM_PAGE);
    uint32_t pa = vm_alloc_phys(k->vm, size, lo, hi, align < VM_PAGE ? VM_PAGE : align);
    if (pa == VM_NO_PHYS) {
        return 0;
    }
    uint32_t va = VM_MIRROR_BASE | pa;
    k->contig[k->ncontig++] = (KContig){va, size, prot ? prot : PAGE_READWRITE};
    return va;
}

static void contig_free(Kernel *k, uint32_t va)
{
    KContig *b = contig_at(k, va);
    if (b) {
        vm_free_phys(k->vm, va & ~VM_MIRROR_BASE, b->size);
        *b = k->contig[--k->ncontig];
    }
}

/* MmAllocateContiguousMemory(NumberOfBytes) */
static uint64_t MmAllocateContiguousMemory(KCall *c)
{
    return contig_alloc(c->k, c->a[0], 0, 0, VM_PAGE, PAGE_READWRITE);
}

/* MmAllocateContiguousMemoryEx(NumberOfBytes, Lowest, Highest, Alignment, Protect):
 * los límites son de páginas (la más alta se toma completa). */
static uint64_t MmAllocateContiguousMemoryEx(KCall *c)
{
    uint32_t hi = c->a[2] >= VM_RAM_SIZE - 1 ? 0 : (c->a[2] | (VM_PAGE - 1)) + 1;
    return contig_alloc(c->k, c->a[0], c->a[1], hi, c->a[3] ? c->a[3] : VM_PAGE, c->a[4]);
}

static uint64_t MmFreeContiguousMemory(KCall *c)
{
    contig_free(c->k, c->a[0]);
    return 0;
}

/* MmAllocateSystemMemory(NumberOfBytes, Protect) */
static uint64_t MmAllocateSystemMemory(KCall *c)
{
    return contig_alloc(c->k, c->a[0], 0, 0, VM_PAGE, c->a[1]);
}

/* MmFreeSystemMemory(BaseAddress, NumberOfBytes) -> páginas liberadas */
static uint64_t MmFreeSystemMemory(KCall *c)
{
    KContig *b = contig_at(c->k, c->a[0]);
    uint32_t pages = b ? b->size / VM_PAGE : 0;
    contig_free(c->k, c->a[0]);
    return pages;
}

/* ------------------------------------------------------- virtual */

static KRegion *region_at(Kernel *k, uint32_t va)
{
    for (unsigned i = 0; i < k->nregions; i++) {
        if (va - k->regions[i].base < k->regions[i].size) {
            return &k->regions[i];
        }
    }
    return NULL;
}

static bool range_free(Kernel *k, uint32_t base, uint32_t size)
{
    for (unsigned i = 0; i < k->nregions; i++) {
        const KRegion *r = &k->regions[i];
        if (base < r->base + r->size && r->base < base + size) {
            return false;
        }
    }
    return true;
}

static uint32_t find_free(Kernel *k, uint32_t size, bool top_down)
{
    uint32_t span = align_up(size, VA_GRAIN);
    if (top_down) {
        for (uint32_t b = (USER_HI - span) & ~(VA_GRAIN - 1); b >= USER_LO; b -= VA_GRAIN) {
            if (range_free(k, b, span)) {
                return b;
            }
        }
        return 0;
    }
    for (uint32_t b = USER_LO; b + span <= USER_HI; b += VA_GRAIN) {
        if (range_free(k, b, span)) {
            return b;
        }
    }
    return 0;
}

static bool add_region(Kernel *k, uint32_t base, uint32_t size, uint32_t prot, bool image)
{
    if (k->nregions >= K_MAX_REGIONS) {
        return false;
    }
    k->regions[k->nregions++] = (KRegion){base, size, prot, image};
    return true;
}

void kmm_add_image(Kernel *k, uint32_t base, uint32_t size)
{
    add_region(k, base & ~(VA_GRAIN - 1), align_up(base + size, VA_GRAIN) - (base & ~(VA_GRAIN - 1)),
               PAGE_READWRITE, true);
}

/* NtAllocateVirtualMemory(*BaseAddress, ZeroBits, *RegionSize, AllocationType, Protect) */
static uint64_t NtAllocateVirtualMemory(KCall *c)
{
    Kernel *k = c->k;
    uint32_t base = vm_rd32(c->vm, c->a[0]);
    uint32_t size = vm_rd32(c->vm, c->a[2]);
    uint32_t type = c->a[3];
    if (!size || !(type & (MEM_COMMIT | MEM_RESERVE))) {
        return ST_INVALID_PARAMETER;
    }
    if ((type & MEM_RESERVE) || !base) { /* un commit sin dirección también reserva */
        if (base) {
            uint32_t end = align_up(base + size, VM_PAGE);
            base &= ~(VA_GRAIN - 1);
            size = end - base;
            if (base < USER_LO || end > USER_HI || !range_free(k, base, size)) {
                return ST_CONFLICTING_ADDRESSES;
            }
        } else {
            size = align_up(size, VM_PAGE);
            base = find_free(k, size, (type & MEM_TOP_DOWN) != 0);
            if (!base) {
                return ST_NO_MEMORY;
            }
        }
        if (!add_region(k, base, size, c->a[4], false)) {
            return ST_NO_MEMORY;
        }
    } else {
        uint32_t end = align_up(base + size, VM_PAGE);
        base &= ~(VM_PAGE - 1);
        size = end - base;
        KRegion *r = region_at(k, base);
        if (!r || end > r->base + r->size) {
            return ST_NOT_COMMITTED;
        }
    }
    if ((type & MEM_COMMIT) && !vm_alloc_at(c->vm, base, size)) {
        return ST_NO_MEMORY;
    }
    vm_wr32(c->vm, c->a[0], base);
    vm_wr32(c->vm, c->a[2], size);
    return ST_SUCCESS;
}

/* NtFreeVirtualMemory(*BaseAddress, *RegionSize, FreeType) */
static uint64_t NtFreeVirtualMemory(KCall *c)
{
    Kernel *k = c->k;
    uint32_t base = vm_rd32(c->vm, c->a[0]);
    uint32_t size = vm_rd32(c->vm, c->a[1]);
    KRegion *r = region_at(k, base);
    if (!r || r->image) {
        return ST_MEMORY_NOT_ALLOCATED;
    }
    if (c->a[2] & MEM_RELEASE) {
        uint32_t r_end = r->base + r->size;
        uint32_t end = size ? align_up(base + size, VM_PAGE) : r_end;
        base &= ~(VM_PAGE - 1);
        if (!size && base != r->base) {
            return ST_FREE_VM_NOT_AT_BASE;
        }
        if (end > r_end) {
            return ST_UNABLE_TO_FREE_VM;
        }
        if (base > r->base && end < r_end && !add_region(k, end, r_end - end, r->protect, false)) {
            return ST_NO_MEMORY;
        }
        r = region_at(k, base); /* add_region puede no moverla, pero por si acaso */
        size = end - base;
        vm_unmap(c->vm, base, size);
        if (base > r->base) {
            r->size = base - r->base;
        } else if (end < r_end) {
            r->base = end;
            r->size = r_end - end;
        } else {
            *r = k->regions[--k->nregions];
        }
    } else if (c->a[2] & MEM_DECOMMIT) {
        uint32_t end = size ? align_up(base + size, VM_PAGE) : r->base + r->size;
        base &= ~(VM_PAGE - 1);
        size = end - base;
        vm_unmap(c->vm, base, size);
    } else {
        return ST_INVALID_PARAMETER;
    }
    vm_wr32(c->vm, c->a[0], base);
    vm_wr32(c->vm, c->a[1], size);
    return ST_SUCCESS;
}

/* NtQueryVirtualMemory(BaseAddress, MEMORY_BASIC_INFORMATION*) */
static uint64_t NtQueryVirtualMemory(KCall *c)
{
    Kernel *k = c->k;
    uint32_t va = c->a[0] & ~(VM_PAGE - 1), out = c->a[1];
    KRegion *r = region_at(k, va);
    uint32_t mbi[7] = {0};
    mbi[0] = va;
    if (!r) {
        /* libre hasta la siguiente reserva */
        uint32_t next = USER_HI;
        for (unsigned i = 0; i < k->nregions; i++) {
            if (k->regions[i].base > va && k->regions[i].base < next) {
                next = k->regions[i].base;
            }
        }
        mbi[3] = next > va ? next - va : VM_PAGE;
        mbi[4] = MEM_FREE;
        mbi[5] = PAGE_NOACCESS;
    } else {
        bool committed = vm_phys(c->vm, va) != VM_NO_PHYS;
        uint32_t end = va;
        while (end < r->base + r->size && (vm_phys(c->vm, end) != VM_NO_PHYS) == committed) {
            end += VM_PAGE;
        }
        mbi[1] = r->base;
        mbi[2] = r->protect;
        mbi[3] = end - va;
        mbi[4] = committed ? MEM_COMMIT : MEM_RESERVE;
        mbi[5] = committed ? r->protect : 0;
        mbi[6] = r->image ? MEM_IMAGE : MEM_PRIVATE;
    }
    for (int i = 0; i < 7; i++) {
        vm_wr32(c->vm, out + 4 * (uint32_t)i, mbi[i]);
    }
    return ST_SUCCESS;
}

/* NtProtectVirtualMemory(*BaseAddress, *RegionSize, NewProtect, *OldProtect):
 * la memoria aquí es plana; se guarda para las consultas. */
static uint64_t NtProtectVirtualMemory(KCall *c)
{
    uint32_t base = vm_rd32(c->vm, c->a[0]);
    KRegion *r = region_at(c->k, base);
    if (c->a[3]) {
        vm_wr32(c->vm, c->a[3], r ? r->protect : PAGE_READWRITE);
    }
    if (r) {
        r->protect = c->a[2];
    }
    return ST_SUCCESS;
}

/* ------------------------------------------------------- consultas */

static uint64_t MmQueryAllocationSize(KCall *c)
{
    KContig *b = contig_at(c->k, c->a[0]);
    if (b) {
        return b->size;
    }
    KRegion *r = region_at(c->k, c->a[0]);
    return r ? r->size : 0;
}

static uint64_t MmGetPhysicalAddress(KCall *c)
{
    uint32_t pa = vm_phys(c->vm, c->a[0]);
    return pa == VM_NO_PHYS ? 0 : pa;
}

static uint64_t MmIsAddressValid(KCall *c) { return vm_phys(c->vm, c->a[0]) != VM_NO_PHYS; }

static uint64_t MmQueryAddressProtect(KCall *c)
{
    if (vm_phys(c->vm, c->a[0]) == VM_NO_PHYS) {
        return 0;
    }
    KContig *b = contig_holding(c->k, c->a[0]);
    KRegion *r = b ? NULL : region_at(c->k, c->a[0]);
    uint32_t p = b ? b->protect : r ? r->protect : 0;
    return p ? p : PAGE_READWRITE;
}

static uint64_t MmSetAddressProtect(KCall *c)
{
    KContig *b = contig_holding(c->k, c->a[0]);
    if (b) {
        b->protect = c->a[2];
    }
    return 0;
}

static uint64_t MmNothing(KCall *c)
{
    (void)c;
    return 0;
}

/* MmClaimGpuInstanceMemory(NumberOfBytes, *NumberOfPaddingBytes): la memoria
 * de instancia de la GPU arriba de la RAM, bajo 64 KB de relleno. */
static uint64_t MmClaimGpuInstanceMemory(KCall *c)
{
    Kernel *k = c->k;
    const uint32_t pad = 0x10000;
    if (c->a[1]) {
        vm_wr32(c->vm, c->a[1], pad);
    }
    if (!k->gpu_instance_end) {
        k->gpu_instance_end = VM_RAM_SIZE - pad;
        uint32_t size = align_up(c->a[0], VM_PAGE);
        vm_claim_phys(c->vm, k->gpu_instance_end - size, size + pad);
    }
    return VM_MIRROR_BASE | k->gpu_instance_end;
}

/* MmMapIoSpace(PhysicalAddress, NumberOfBytes, Protect) */
static uint64_t MmMapIoSpace(KCall *c)
{
    return c->a[0] < VM_RAM_SIZE ? (VM_MIRROR_BASE | c->a[0]) : c->a[0];
}

/* ------------------------------------------------------- pilas */

uint32_t kmm_stack_alloc(Kernel *k, uint32_t size)
{
    size = align_up(size, VM_PAGE);
    uint32_t base = k->stack_next + VM_PAGE; /* una página de guarda sin mapear debajo */
    if ((uint64_t)base + size > STACK_END || !vm_alloc_at(k->vm, base, size)) {
        return 0;
    }
    k->stack_next = base + size;
    return base;
}

/* MmCreateKernelStack(NumberOfBytes, DebuggerThread) -> tope de la pila */
static uint64_t MmCreateKernelStack(KCall *c)
{
    uint32_t size = align_up(c->a[0], VM_PAGE);
    uint32_t base = kmm_stack_alloc(c->k, size);
    return base ? base + size : 0;
}

/* MmDeleteKernelStack(StackBase, StackLimit) */
static uint64_t MmDeleteKernelStack(KCall *c)
{
    if (c->a[0] > c->a[1]) {
        vm_unmap(c->vm, c->a[1], c->a[0] - c->a[1]);
    }
    return 0;
}

/* MmQueryStatistics(MM_STATISTICS*): { Length 0x24, TotalPhysicalPages,
 *   AvailablePages, VirtualMemoryBytesCommitted, VirtualMemoryBytesReserved,
 *   CachePagesCommitted, PoolPagesCommitted, StackPagesCommitted, ImagePagesCommitted } */
static uint64_t MmQueryStatistics(KCall *c)
{
    Kernel *k = c->k;
    if (vm_rd32(c->vm, c->a[0]) != 0x24) {
        return ST_INVALID_PARAMETER;
    }
    uint32_t committed = 0, reserved = 0, image = 0;
    for (unsigned i = 0; i < k->nregions; i++) {
        const KRegion *r = &k->regions[i];
        reserved += r->size;
        for (uint32_t p = r->base; p < r->base + r->size; p += VM_PAGE) {
            if (vm_phys(c->vm, p) != VM_NO_PHYS) {
                if (r->image) {
                    image++;
                } else {
                    committed += VM_PAGE;
                }
            }
        }
    }
    uint32_t s[9] = {0x24, VM_RAM_SIZE / VM_PAGE, vm_free_pages(c->vm), committed, reserved, 0,
                     k->pool_len / VM_PAGE, (k->stack_next - STACK_AREA) / VM_PAGE, image};
    for (int i = 0; i < 9; i++) {
        vm_wr32(c->vm, c->a[0] + 4 * (uint32_t)i, s[i]);
    }
    return ST_SUCCESS;
}

void kmm_register(Kernel *k)
{
    kernel_impl(k, "ExAllocatePool", ExAllocatePool);
    kernel_impl(k, "ExAllocatePoolWithTag", ExAllocatePoolWithTag);
    kernel_impl(k, "ExFreePool", ExFreePool);
    kernel_impl(k, "ExQueryPoolBlockSize", ExQueryPoolBlockSize);
    kernel_impl(k, "MmAllocateContiguousMemory", MmAllocateContiguousMemory);
    kernel_impl(k, "MmAllocateContiguousMemoryEx", MmAllocateContiguousMemoryEx);
    kernel_impl(k, "MmFreeContiguousMemory", MmFreeContiguousMemory);
    kernel_impl(k, "MmAllocateSystemMemory", MmAllocateSystemMemory);
    kernel_impl(k, "MmFreeSystemMemory", MmFreeSystemMemory);
    kernel_impl(k, "NtAllocateVirtualMemory", NtAllocateVirtualMemory);
    kernel_impl(k, "NtFreeVirtualMemory", NtFreeVirtualMemory);
    kernel_impl(k, "NtQueryVirtualMemory", NtQueryVirtualMemory);
    kernel_impl(k, "NtProtectVirtualMemory", NtProtectVirtualMemory);
    kernel_impl(k, "MmQueryAllocationSize", MmQueryAllocationSize);
    kernel_impl(k, "MmGetPhysicalAddress", MmGetPhysicalAddress);
    kernel_impl(k, "MmIsAddressValid", MmIsAddressValid);
    kernel_impl(k, "MmQueryAddressProtect", MmQueryAddressProtect);
    kernel_impl(k, "MmSetAddressProtect", MmSetAddressProtect);
    kernel_impl(k, "MmPersistContiguousMemory", MmNothing);
    kernel_impl(k, "MmLockUnlockBufferPages", MmNothing);
    kernel_impl(k, "MmLockUnlockPhysicalPage", MmNothing);
    kernel_impl(k, "MmUnmapIoSpace", MmNothing);
    kernel_impl(k, "MmClaimGpuInstanceMemory", MmClaimGpuInstanceMemory);
    kernel_impl(k, "MmMapIoSpace", MmMapIoSpace);
    kernel_impl(k, "MmCreateKernelStack", MmCreateKernelStack);
    kernel_impl(k, "MmDeleteKernelStack", MmDeleteKernelStack);
    kernel_impl(k, "MmQueryStatistics", MmQueryStatistics);
}
