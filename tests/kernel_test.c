/*
 * XboxNX — pruebas en PC del kernel de la etapa 3a (memoria y objetos).
 */
#include <stdio.h>
#include <string.h>

#include "../source/cpu.h"
#include "../source/kernel.h"

static int g_fail;
#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                            \
        }                                                        \
    } while (0)

static Vm vm;
static Kernel k;

#define SCRATCH 0x00F00000u /* parámetros de salida */

static uint64_t call(const char *name, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    for (unsigned o = 1; o < K_ORDINAL_MAX; o++) {
        const KExport *e = kernel_export(o);
        if (e && !strcmp(e->name, name)) {
            if (!k.impl[o]) {
                printf("FALLO: %s no está implementada\n", name);
                g_fail++;
                return 0xDEAD;
            }
            KCall c = {&k, &vm, NULL, {a0, a1, a2, a3, a4}};
            return k.impl[o](&c);
        }
    }
    printf("FALLO: %s no existe\n", name);
    g_fail++;
    return 0xDEAD;
}

static uint32_t rd(uint32_t va) { return vm_rd32(&vm, va); }
static void wr(uint32_t va, uint32_t v) { vm_wr32(&vm, va, v); }

int main(void)
{
    CHECK(vm_init(&vm));
    CHECK(kernel_init(&k, &vm));
    CHECK(vm_alloc_at(&vm, SCRATCH, 0x1000));
    printf("funciones reales: %u\n", kernel_real_count(&k));
    CHECK(kernel_real_count(&k) >= 40);

    /* ---- contigua ---- */
    uint32_t free0 = vm_free_pages(&vm);
    uint32_t a = (uint32_t)call("MmAllocateContiguousMemory", 0x1000, 0, 0, 0, 0);
    CHECK(a >= 0x80000000u && a < 0x84000000u && !(a & 0xFFF));
    CHECK(vm_free_pages(&vm) == free0 - 1);
    wr(a, 0x12345678);
    CHECK(rd(a) == 0x12345678);
    uint32_t b = (uint32_t)call("MmAllocateContiguousMemory", 0x2001, 0, 0, 0, 0);
    CHECK(b && b != a);
    CHECK(call("MmQueryAllocationSize", b, 0, 0, 0, 0) == 0x3000);
    CHECK(call("MmGetPhysicalAddress", b, 0, 0, 0, 0) == (b & 0x7FFFFFFF));
    call("MmFreeContiguousMemory", a, 0, 0, 0, 0);
    uint32_t a2 = (uint32_t)call("MmAllocateContiguousMemory", 0x1000, 0, 0, 0, 0);
    CHECK(a2 == a); /* la más baja libre otra vez */
    /* Ex con límites y alineación: el bloque fijo de Halo desde 0x61000 */
    uint32_t c = (uint32_t)call("MmAllocateContiguousMemoryEx", 0x3000, 0x61000, 0x6FFFF, 0x2000, 4);
    CHECK(c && (c & 0x7FFFFFFF) >= 0x62000 && !((c & 0x7FFFFFFF) & 0x1FFF));
    CHECK((c & 0x7FFFFFFF) + 0x3000 <= 0x70000);
    /* un pedido imposible devuelve 0 */
    CHECK(call("MmAllocateContiguousMemoryEx", 0x20000, 0x61000, 0x6FFFF, 0, 4) == 0);

    /* ---- virtual ---- */
    wr(SCRATCH, 0);
    wr(SCRATCH + 4, 0x5000);
    CHECK(call("NtAllocateVirtualMemory", SCRATCH, 0, SCRATCH + 4, 0x3000, 4) == ST_SUCCESS);
    uint32_t v = rd(SCRATCH);
    CHECK(v >= 0x10000 && v < 0x7FFE0000 && !(v & 0xFFFF));
    CHECK(rd(SCRATCH + 4) == 0x5000);
    CHECK(vm_phys(&vm, v) != VM_NO_PHYS && vm_phys(&vm, v + 0x4000) != VM_NO_PHYS);
    /* no puede chocar con la reserva que ya existe */
    wr(SCRATCH + 8, v);
    wr(SCRATCH + 12, 0x1000);
    CHECK(call("NtAllocateVirtualMemory", SCRATCH + 8, 0, SCRATCH + 12, 0x2000, 4) == ST_CONFLICTING_ADDRESSES);
    /* NtQueryVirtualMemory */
    CHECK(call("NtQueryVirtualMemory", v + 0x1234, SCRATCH + 0x100, 0, 0, 0) == ST_SUCCESS);
    CHECK(rd(SCRATCH + 0x100) == v + 0x1000 && rd(SCRATCH + 0x104) == v && rd(SCRATCH + 0x10C) == 0x4000 &&
          rd(SCRATCH + 0x110) == 0x1000);
    /* liberar todo */
    uint32_t before = vm_free_pages(&vm);
    wr(SCRATCH + 4, 0);
    CHECK(call("NtFreeVirtualMemory", SCRATCH, SCRATCH + 4, 0x8000, 0, 0) == ST_SUCCESS);
    CHECK(vm_free_pages(&vm) == before + 5);
    CHECK(vm_phys(&vm, v) == VM_NO_PHYS);
    /* reservar en una dirección, luego commit de una parte y decommit */
    wr(SCRATCH, 0x00400000);
    wr(SCRATCH + 4, 0x100000);
    CHECK(call("NtAllocateVirtualMemory", SCRATCH, 0, SCRATCH + 4, 0x2000, 4) == ST_SUCCESS);
    CHECK(rd(SCRATCH) == 0x00400000 && vm_phys(&vm, 0x00400000) == VM_NO_PHYS);
    wr(SCRATCH, 0x00402000);
    wr(SCRATCH + 4, 0x1800);
    CHECK(call("NtAllocateVirtualMemory", SCRATCH, 0, SCRATCH + 4, 0x1000, 4) == ST_SUCCESS);
    CHECK(rd(SCRATCH) == 0x00402000 && rd(SCRATCH + 4) == 0x2000);
    CHECK(vm_phys(&vm, 0x00402000) != VM_NO_PHYS && vm_phys(&vm, 0x00403FFF) != VM_NO_PHYS);
    CHECK(vm_phys(&vm, 0x00404000) == VM_NO_PHYS);
    wr(SCRATCH, 0x00402000);
    wr(SCRATCH + 4, 0x1000);
    CHECK(call("NtFreeVirtualMemory", SCRATCH, SCRATCH + 4, 0x4000, 0, 0) == ST_SUCCESS);
    CHECK(vm_phys(&vm, 0x00402000) == VM_NO_PHYS && vm_phys(&vm, 0x00403000) != VM_NO_PHYS);
    /* commit fuera de reservas */
    wr(SCRATCH, 0x07000000);
    wr(SCRATCH + 4, 0x1000);
    CHECK(call("NtAllocateVirtualMemory", SCRATCH, 0, SCRATCH + 4, 0x1000, 4) == ST_NOT_COMMITTED);

    /* ---- pool ---- */
    uint32_t p1 = (uint32_t)call("ExAllocatePoolWithTag", 100, 0x41424344, 0, 0, 0);
    CHECK(p1 >= 0x80000000u && !(p1 & 0xF));
    CHECK(call("ExQueryPoolBlockSize", p1, 0, 0, 0, 0) == 100);
    uint32_t p2 = (uint32_t)call("ExAllocatePool", 0x3000, 0, 0, 0, 0); /* páginas propias */
    CHECK(p2 && call("ExQueryPoolBlockSize", p2, 0, 0, 0, 0) == 0x3000);
    wr(p2 + 0x2FFC, 7);
    CHECK(rd(p2 + 0x2FFC) == 7);
    uint32_t fp = vm_free_pages(&vm);
    call("ExFreePool", p2, 0, 0, 0, 0);
    CHECK(vm_free_pages(&vm) == fp + 4);
    call("ExFreePool", p1, 0, 0, 0, 0);
    uint32_t p3 = (uint32_t)call("ExAllocatePool", 100, 0, 0, 0, 0);
    CHECK(p3 == p1); /* el hueco se reutiliza */
    /* crecer el pool más allá de su primer trozo */
    uint32_t last = 0;
    for (int i = 0; i < 200; i++) {
        last = (uint32_t)call("ExAllocatePool", 1000, 0, 0, 0, 0);
    }
    CHECK(last != 0);

    /* ---- objetos ---- */
    CHECK(call("NtCreateEvent", SCRATCH + 0x20, 0, 1, 0, 0) == ST_SUCCESS);
    uint32_t he = rd(SCRATCH + 0x20);
    CHECK(he && !(he & 3));
    CHECK(call("NtSetEvent", he, SCRATCH + 0x24, 0, 0, 0) == ST_SUCCESS && rd(SCRATCH + 0x24) == 0);
    CHECK(call("NtSetEvent", he, SCRATCH + 0x24, 0, 0, 0) == ST_SUCCESS && rd(SCRATCH + 0x24) == 1);
    /* referencia por handle con el tipo correcto y con otro */
    uint32_t evtype = kernel_data_va(&k, "ExEventObjectType");
    uint32_t semtype = kernel_data_va(&k, "ExSemaphoreObjectType");
    CHECK(call("ObReferenceObjectByHandle", he, evtype, SCRATCH + 0x28, 0, 0) == ST_SUCCESS);
    uint32_t eo = rd(SCRATCH + 0x28);
    CHECK(eo && *vm_host(&vm, eo) == 1 /* sincronización */ && rd(eo + 4) == 1);
    CHECK(rd(eo - 0x10) == 2); /* PointerCount: creación + referencia */
    CHECK(call("ObReferenceObjectByHandle", he, semtype, SCRATCH + 0x28, 0, 0) == ST_OBJECT_TYPE_MISMATCH);
    CHECK(call("NtClose", he, 0, 0, 0, 0) == ST_SUCCESS);
    CHECK(call("NtClose", he, 0, 0, 0, 0) == ST_INVALID_HANDLE);
    CHECK(call("NtClose", 0, 0, 0, 0, 0) == ST_INVALID_HANDLE);
    /* semáforo */
    CHECK(call("NtCreateSemaphore", SCRATCH + 0x30, 0, 1, 3, 0) == ST_SUCCESS);
    uint32_t hs = rd(SCRATCH + 0x30);
    CHECK(call("NtReleaseSemaphore", hs, 2, SCRATCH + 0x34, 0, 0) == ST_SUCCESS && rd(SCRATCH + 0x34) == 1);
    CHECK(call("NtReleaseSemaphore", hs, 1, 0, 0, 0) == 0xC0000047u);
    CHECK(call("NtCreateSemaphore", SCRATCH + 0x30, 0, 5, 3, 0) == ST_INVALID_PARAMETER);
    /* mutex */
    CHECK(call("NtCreateMutant", SCRATCH + 0x38, 0, 1, 0, 0) == ST_SUCCESS);
    uint32_t hm = rd(SCRATCH + 0x38);
    CHECK(call("NtReleaseMutant", hm, SCRATCH + 0x3C, 0, 0, 0) == ST_SUCCESS && rd(SCRATCH + 0x3C) == 0);
    CHECK(call("NtReleaseMutant", hm, 0, 0, 0, 0) == 0xC0000046u);
    /* duplicar y cerrar el origen */
    CHECK(call("NtDuplicateObject", hm, SCRATCH + 0x40, 1, 0, 0) == ST_SUCCESS);
    CHECK(rd(SCRATCH + 0x40) != hm && call("NtClose", hm, 0, 0, 0, 0) == ST_INVALID_HANDLE);

    /* ---- estadísticas ---- */
    wr(SCRATCH + 0x200, 0x24);
    CHECK(call("MmQueryStatistics", SCRATCH + 0x200, 0, 0, 0, 0) == ST_SUCCESS);
    CHECK(rd(SCRATCH + 0x204) == 16384 && rd(SCRATCH + 0x208) == vm_free_pages(&vm));
    wr(SCRATCH + 0x200, 0);
    CHECK(call("MmQueryStatistics", SCRATCH + 0x200, 0, 0, 0, 0) == ST_INVALID_PARAMETER);

    /* ---- pila del sistema ---- */
    uint32_t st = kmm_stack_alloc(&k, 0x3000);
    CHECK(st >= 0xD0000000u && vm_phys(&vm, st) != VM_NO_PHYS && vm_phys(&vm, st - 1) == VM_NO_PHYS);

    vm_free(&vm);
    printf(g_fail ? "%d fallos\n" : "todo bien\n", g_fail);
    return g_fail ? 1 : 0;
}
