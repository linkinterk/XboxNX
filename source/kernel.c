/*
 * XboxNX — área del kernel de Xbox.
 *
 * Distribución tomada de hakuX (native/kernel/kernel.c, GPL-3.0-or-later).
 */
#include "kernel.h"

#include <string.h>

#define K_STUBS     0x0F00u  /* stub de parada */
#define K_TRAMPS    0x1000u  /* 8 bytes por ordinal */
#define K_DATA      0x2000u  /* 0x40 bytes por dato exportado */
#define K_DATA_SLOT 0x40u
#define K_DATA_BIG  0xA000u  /* datos más grandes que una casilla */

static const KExport k_exports[] = {
#define KX(o, n, kind) {o, n, kind},
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

bool kernel_init(Kernel *k, Vm *vm)
{
    memset(k, 0, sizeof(*k));
    index_exports();
    if (!vm_claim_phys(vm, K_PA, K_SIZE)) {
        return false;
    }
    /* El área es contigua en física, así que se escribe directo. */
    uint8_t *km = vm->ram + K_PA;
    memset(km, 0, K_SIZE);
    memset(km + K_STUBS, 0xCC, K_DATA - K_STUBS);
    km[K_STUBS] = 0xF4; /* hlt: salir del JIT */

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
    return true;
}

uint32_t kernel_resolve(const Kernel *k, unsigned ordinal)
{
    const KExport *e = kernel_export(ordinal);
    if (!e) {
        return 0;
    }
    return e->kind == KX_DATA ? k->data_va[ordinal] : k->tramp_va[ordinal];
}
