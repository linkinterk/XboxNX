/*
 * XboxNX — área del kernel de Xbox.
 *
 * Distribución tomada de hakuX (native/kernel/kernel.c, GPL-3.0-or-later).
 */
#include "kernel.h"
#include "hw.h"

#include <string.h>

#define K_STUBS     0x0F00u  /* stub de parada */
#define K_TRAMPS    0x1000u  /* 8 bytes por ordinal */
#define K_DATA      0x2000u  /* 0x40 bytes por dato exportado */
#define K_DATA_SLOT 0x40u
#define K_DATA_BIG  0xA000u  /* datos más grandes que una casilla */

static const KExport k_exports[] = {
#define KX(o, n, kind, conv, bytes) {o, n, kind, conv, bytes},
#include "kexports.h"
#undef KX
};

static const KExport *k_by_ordinal[K_ORDINAL_MAX];
static bool k_index_ready;

static void index_exports(void)
{
    if (k_index_ready) {
        return;
    }
    for (size_t i = 0; i < sizeof(k_exports) / sizeof(k_exports[0]); i++) {
        if (k_exports[i].ordinal < K_ORDINAL_MAX) {
            k_by_ordinal[k_exports[i].ordinal] = &k_exports[i];
        }
    }
    k_index_ready = true;
}

const KExport *kernel_export(unsigned ordinal)
{
    index_exports();
    return ordinal < K_ORDINAL_MAX ? k_by_ordinal[ordinal] : NULL;
}

void kernel_counts(unsigned *data, unsigned *impl, unsigned *todo)
{
    *data = *impl = *todo = 0;
    for (size_t i = 0; i < sizeof(k_exports) / sizeof(k_exports[0]); i++) {
        switch (k_exports[i].kind) {
        case KX_DATA: (*data)++; break;
        case KX_IMPL: (*impl)++; break;
        default: (*todo)++; break;
        }
    }
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void kernel_impl(Kernel *k, const char *name, KFn fn)
{
    for (size_t i = 0; i < sizeof(k_exports) / sizeof(k_exports[0]); i++) {
        if (k_exports[i].kind != KX_DATA && strcmp(k_exports[i].name, name) == 0) {
            k->impl[k_exports[i].ordinal] = fn;
            return;
        }
    }
}

unsigned kernel_real_count(const Kernel *k)
{
    unsigned n = 0;
    for (unsigned i = 0; i < K_ORDINAL_MAX; i++) {
        n += k->impl[i] != NULL;
    }
    return n;
}

bool kernel_init(Kernel *k, Vm *vm)
{
    memset(k, 0, sizeof(*k));
    k->vm = vm;
    k->ns_per_instr = K_NS_PER_INSTR;
    index_exports();
    if (!vm_claim_phys(vm, K_PA, K_SIZE)) {
        return false;
    }
    /* El área es contigua en física, así que se escribe directo. */
    uint8_t *km = vm->ram + K_PA;
    memset(km, 0, K_SIZE);
    memset(km + K_STUBS, 0xCC, K_DATA - K_STUBS);
    km[K_STUBS] = 0xF4; /* hlt: salir del JIT */
    /* stub de vuelta de la rutina de un hilo: int 0x3F con el pseudo-ordinal 0x3FF */
    km[K_STUBS + 8] = 0xCD;
    km[K_STUBS + 9] = K_CALL_VECTOR;
    km[K_STUBS + 10] = (uint8_t)K_ORD_THREAD_RETURN;
    km[K_STUBS + 11] = (uint8_t)(K_ORD_THREAD_RETURN >> 8);
    km[K_STUBS + 16] = 0xCD;
    km[K_STUBS + 17] = K_CALL_VECTOR;
    km[K_STUBS + 18] = (uint8_t)K_ORD_DPC_RETURN;
    km[K_STUBS + 19] = (uint8_t)(K_ORD_DPC_RETURN >> 8);
    km[K_STUBS + 24] = 0xCD; /* un ISR volvió */
    km[K_STUBS + 25] = K_CALL_VECTOR;
    km[K_STUBS + 26] = (uint8_t)K_ORD_ISR_RETURN;
    km[K_STUBS + 27] = (uint8_t)(K_ORD_ISR_RETURN >> 8);
    km[K_STUBS + 32] = 0xCD; /* un APC de usuario volvió */
    km[K_STUBS + 33] = K_CALL_VECTOR;
    km[K_STUBS + 34] = (uint8_t)K_ORD_APC_RETURN;
    km[K_STUBS + 35] = (uint8_t)(K_ORD_APC_RETURN >> 8);

    for (size_t i = 0; i < sizeof(k_exports) / sizeof(k_exports[0]); i++) {
        const KExport *e = &k_exports[i];
        if (e->ordinal >= K_ORDINAL_MAX) {
            continue;
        }
        if (e->kind == KX_DATA) {
            k->data_va[e->ordinal] = K_VA + K_DATA + e->ordinal * K_DATA_SLOT;
            if (strcmp(e->name, "XboxAlternateSignatureKeys") == 0) {
                k->data_va[e->ordinal] = K_VA + K_DATA_BIG;
            }
        } else {
            uint32_t off = K_TRAMPS + e->ordinal * 8u;
            uint8_t *t = km + off;
            t[0] = 0xCD; /* int 0x3F */
            t[1] = K_CALL_VECTOR;
            t[2] = (uint8_t)e->ordinal;
            t[3] = (uint8_t)(e->ordinal >> 8);
            t[4] = 0xF4; /* nunca llega: el kernel vuelve al que llamó */
            k->tramp_va[e->ordinal] = K_VA + off;
        }
    }

    /* Cabecera PE con directorio de exportaciones, como la imagen real. */
    wr32(km + 0x00, 0x00905A4Du); /* "MZ" */
    wr32(km + 0x3C, 0x80);
    wr32(km + 0x80, 0x00004550u); /* "PE\0\0" */
    wr32(km + 0x84, 0x0000014Cu); /* i386 */
    wr32(km + 0x94, 0x010200E0u);
    wr32(km + 0x98, 0x0000010Bu); /* PE32 */
    wr32(km + 0x98 + 28, K_VA);
    wr32(km + 0x98 + 92, 16);
    wr32(km + 0x98 + 96, 0x200);  /* directorio de exportaciones */
    wr32(km + 0x98 + 100, 0x28);
    wr32(km + 0x200 + 0x10, 1);                 /* ordinal base */
    wr32(km + 0x200 + 0x14, K_ORDINAL_MAX - 1); /* funciones */
    wr32(km + 0x200 + 0x1C, 0x300);             /* AddressOfFunctions */
    for (unsigned o = 1; o < K_ORDINAL_MAX; o++) {
        uint32_t va = k->tramp_va[o] ? k->tramp_va[o] : k->data_va[o];
        wr32(km + 0x300 + 4 * (o - 1), va ? va - K_VA : 0);
    }
    /* Datos exportados que se leen al arrancar (los mismos valores que hakuX). */
    uint32_t va;
    if ((va = kernel_data_va(k, "XboxKrnlVersion")) != 0) {
        uint8_t *p = vm_host(vm, va);
        const uint16_t v[4] = {1, 0, 5838, 1};
        for (int i = 0; i < 4; i++) {
            p[i * 2] = (uint8_t)v[i];
            p[i * 2 + 1] = (uint8_t)(v[i] >> 8);
        }
    }
    if ((va = kernel_data_va(k, "KeTimeIncrement")) != 0) {
        wr32(vm_host(vm, va), 10000); /* 1 ms en unidades de 100 ns */
    }
    if ((va = kernel_data_va(k, "HalDiskCachePartitionCount")) != 0) {
        wr32(vm_host(vm, va), 3);
    }
    if ((va = kernel_data_va(k, "IdexChannelObject")) != 0) {
        /* cola de dispositivo vacía en +0x24 (KDEVICE_QUEUE) */
        uint32_t q = va + 0x24;
        uint8_t *p = vm_host(vm, q);
        p[0] = 0x14;
        p[2] = 0x0C;
        wr32(p + 4, q + 4);
        wr32(p + 8, q + 4);
    }
    if ((va = kernel_data_va(k, "KdDebuggerNotPresent")) != 0) {
        *vm_host(vm, va) = 1;
    }

    static const char *const type_names[OB_TYPES] = {
        "PsThreadObjectType", "ExEventObjectType",   "ExSemaphoreObjectType",
        "ExMutantObjectType", "ExTimerObjectType",   "IoFileObjectType",
        "IoDeviceObjectType", "ObSymbolicLinkObjectType", "ObDirectoryObjectType",
    };
    for (unsigned t = 0; t < OB_TYPES; t++) {
        k->type_va[t] = kernel_data_va(k, type_names[t]);
    }
    if (!kmm_pool_init(k)) {
        return false;
    }
    kmm_register(k);
    kob_register(k);
    kps_register(k);
    kio_register(k);
    ksys_register(k);
    if (!hw_init(k)) {
        return false;
    }
    k->cur = -1;
    return true;
}

bool kernel_title_loaded(Kernel *k, const char *path)
{
    /* XeImageFileName: ANSI_STRING { Length, MaximumLength, Buffer }. Sin
     * ella, el arranque de XAPI no sabe de dónde vino el juego y pide
     * relanzarse (LaunchDataPage + HalReturnToFirmware(2)). */
    uint32_t len = (uint32_t)strlen(path) + 1;
    uint32_t buf = kmm_pool_alloc(k, len);
    uint32_t s = kernel_data_va(k, "XeImageFileName");
    if (!buf || !s) {
        return false;
    }
    vm_write(k->vm, buf, path, len);
    uint8_t hdr[4] = {(uint8_t)(len - 1), (uint8_t)((len - 1) >> 8), (uint8_t)len, (uint8_t)(len >> 8)};
    vm_write(k->vm, s, hdr, 4);
    vm_wr32(k->vm, s + 4, buf);
    return true;
}

uint32_t kernel_data_va(const Kernel *k, const char *name)
{
    for (size_t i = 0; i < sizeof(k_exports) / sizeof(k_exports[0]); i++) {
        if (k_exports[i].kind == KX_DATA && strcmp(k_exports[i].name, name) == 0) {
            return k->data_va[k_exports[i].ordinal];
        }
    }
    return 0;
}

unsigned kernel_pop_bytes(const KExport *e)
{
    switch (e->conv) {
    case KC_STD: return e->arg_bytes;
    case KC_FAST: return e->arg_bytes > 8 ? e->arg_bytes - 8 : 0;
    default: return 0;
    }
}

uint32_t kernel_resolve(const Kernel *k, unsigned ordinal)
{
    const KExport *e = kernel_export(ordinal);
    if (!e) {
        return 0;
    }
    return e->kind == KX_DATA ? k->data_va[ordinal] : k->tramp_va[ordinal];
}
