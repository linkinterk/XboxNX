/*
 * XboxNX — prueba en PC de la etapa 2a (memoria, kernel y cargador).
 * Uso: load_test prueba.iso [--bad-sha]
 */
#include <stdio.h>
#include <string.h>

#include "../source/loader.h"
#include "../source/sha1.h"

static int g_fail;
#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                            \
        }                                                        \
    } while (0)

static Game g;
static Vm vm;
static Kernel k;
static LoadReport r;

static void test_sha1(void)
{
    uint8_t d[20];
    Sha1 s;
    sha1_init(&s);
    sha1_update(&s, "abc", 3);
    sha1_final(&s, d);
    static const uint8_t want[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                     0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    CHECK(memcmp(d, want, 20) == 0);
    /* 1 millón de 'a' en trozos */
    sha1_init(&s);
    char a[1000];
    memset(a, 'a', sizeof(a));
    for (int i = 0; i < 1000; i++) {
        sha1_update(&s, a, sizeof(a));
    }
    sha1_final(&s, d);
    static const uint8_t want2[20] = {0x34, 0xaa, 0x97, 0x3c, 0xd4, 0xc4, 0xda, 0xa4, 0xf6, 0x1e,
                                      0xeb, 0x2b, 0xdb, 0xad, 0x27, 0x31, 0x65, 0x34, 0x01, 0x6f};
    CHECK(memcmp(d, want2, 20) == 0);
}

int main(int argc, char **argv)
{
    bool bad = argc > 2 && !strcmp(argv[2], "--bad-sha");
    test_sha1();
    CHECK(game_open(&g, argv[1]));
    CHECK(vm_init(&vm));
    CHECK(kernel_init(&k, &vm));
    /* el kernel no se puede reservar dos veces */
    CHECK(!vm_claim_phys(&vm, K_PA, 0x1000));
    /* trampolín de un ordinal: int 0x3F, ordinal, hlt; visible en el espejo */
    uint32_t t1 = kernel_resolve(&k, 1);
    uint8_t tb[5];
    CHECK(t1 && vm_read(&vm, t1, tb, 5));
    CHECK(tb[0] == 0xCD && tb[1] == 0x3F && tb[2] == 1 && tb[3] == 0 && tb[4] == 0xF4);
    CHECK(vm_rd32(&vm, K_VA) == 0x00905A4Du);
    CHECK(kernel_export(156) && !strcmp(kernel_export(156)->name, "KeTickCount") &&
          kernel_export(156)->kind == KX_DATA);

    CHECK(loader_load(&vm, &k, &g, &r));
    if (!r.ok) {
        printf("error: %s\n", r.error);
    }
    CHECK(r.sections_total == 3 && r.sections_loaded == 2 && r.sections_later == 1);
    CHECK(r.sec_state[2] == SEC_LATER);
    if (bad) {
        CHECK(r.sha_ok == 1 && r.sha_bad == 1 && r.sec_state[0] == SEC_LOADED_BAD);
    } else {
        CHECK(r.sha_ok == 2 && r.sha_bad == 0);
    }
    CHECK(r.nimports == 4);
    CHECK(r.imports[0].ordinal == 1 && r.imports[3].ordinal == 366);
    CHECK(r.n_impl + r.n_data + r.n_todo == 4);
    /* la tabla quedó apuntando al kernel */
    CHECK(vm_rd32(&vm, 0x12000) == kernel_resolve(&k, 1));
    CHECK(vm_rd32(&vm, 0x1200C) == kernel_resolve(&k, 366));
    /* las primeras instrucciones: jmp $ */
    CHECK(r.entry_bytes[0] == 0xEB && r.entry_bytes[1] == 0xFE);
    /* el refcount de la sección cargada */
    CHECK(vm_rd32(&vm, 0x10000 + 0x400 + 0x18) == 1);
    CHECK(vm_rd32(&vm, 0x10000 + 0x400 + 2 * 56 + 0x18) == 0);
    /* la sección no cargada no tiene memoria */
    CHECK(vm_phys(&vm, 0x13000) == VM_NO_PHYS);
    /* lo de la imagen también se ve en el espejo de 0x80000000 */
    uint32_t pa = vm_phys(&vm, 0x11000);
    CHECK(pa != VM_NO_PHYS && vm_rd32(&vm, 0x80000000u + pa) == vm_rd32(&vm, 0x11000));
    printf("cargadas %u/%u, sha %u ok %u mal, imports %u (impl %u, datos %u, faltan %u), %u KB\n",
           r.sections_loaded, r.sections_total, r.sha_ok, r.sha_bad, r.nimports, r.n_impl, r.n_data, r.n_todo,
           r.ram_used_kb);
    vm_free(&vm);
    game_close(&g);
    printf(g_fail ? "%d fallos\n" : "todo bien\n", g_fail);
    return g_fail ? 1 : 0;
}
