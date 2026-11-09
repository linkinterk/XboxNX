/*
 * XboxNX — carga el XBE en la memoria del Xbox.
 */
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha1.h"

#define XBE_SECTIONS     0x120
#define XBE_TLS_DIR      0x12C
#define XBE_STACK_COMMIT 0x130
#define SEC_HDR_SIZE     0x38
#define SEC_REFS         0x18
#define SEC_DIGEST       0x24

static void fail(LoadReport *r, const char *msg, const char *what)
{
    snprintf(r->error, sizeof(r->error), "%s%s%s", msg, what ? " " : "", what ? what : "");
    r->ok = false;
}

/* El SHA-1 de una sección: el de su tamaño (32 bits) seguido de sus bytes. */
static bool digest_matches(const uint8_t *data, uint32_t len, const uint8_t want[20])
{
    uint8_t got[20], lenb[4] = {(uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
    Sha1 s;
    sha1_init(&s);
    sha1_update(&s, lenb, 4);
    sha1_update(&s, data, len);
    sha1_final(&s, got);
    return memcmp(got, want, 20) == 0;
}

bool loader_load(Vm *vm, Kernel *k, const Game *g, LoadReport *r)
{
    memset(r, 0, sizeof(*r));
    const XbeInfo *x = &g->xbe;
    if (g->xbe_rc != XBE_OK) {
        fail(r, "El XBE no es válido", NULL);
        return false;
    }
    uint32_t base = x->base, hdr_size = x->sizeof_headers;
    if (hdr_size < 0x180 || hdr_size > g->xbe_file.size || base % VM_PAGE) {
        fail(r, "Cabecera del XBE inválida", NULL);
        return false;
    }
    r->base = base;
    r->entry = x->entry;
    r->span = (x->sizeof_image > hdr_size ? x->sizeof_image : hdr_size);
    r->span = (r->span + VM_PAGE - 1) & ~(VM_PAGE - 1);
    if ((uint64_t)base + r->span > VM_MIRROR_BASE) {
        fail(r, "La imagen no cabe en la memoria del juego", NULL);
        return false;
    }

    /* cabeceras en la base */
    uint8_t *hdr = malloc(hdr_size);
    if (!hdr || !vm_alloc_at(vm, base, hdr_size) || !xdisc_read(&g->disc, &g->xbe_file, 0, hdr, hdr_size) ||
        !vm_write(vm, base, hdr, hdr_size)) {
        free(hdr);
        fail(r, "No se pudieron cargar las cabeceras", NULL);
        return false;
    }
    r->tls_dir = vm_rd32(vm, base + XBE_TLS_DIR);
    r->stack_commit = vm_rd32(vm, base + XBE_STACK_COMMIT);
    uint32_t sec_hdrs = vm_rd32(vm, base + XBE_SECTIONS);

    /* secciones */
    r->sections_total = x->nsections;
    for (unsigned i = 0; i < x->nsections; i++) {
        const XbeSection *s = &x->sections[i];
        if (!(s->flags & XBE_SEC_PRELOAD)) {
            r->sec_state[i] = SEC_LATER;
            r->sections_later++;
            continue;
        }
        if (s->va < base || (uint64_t)s->va + s->vsize > (uint64_t)base + r->span) {
            r->sec_state[i] = SEC_FAILED;
            free(hdr);
            fail(r, "Sección fuera de la imagen:", s->name);
            return false;
        }
        if (!vm_alloc_at(vm, s->va, s->vsize)) {
            r->sec_state[i] = SEC_FAILED;
            free(hdr);
            fail(r, "Sin memoria para la sección", s->name);
            return false;
        }
        uint8_t *data = s->raw_size ? malloc(s->raw_size) : NULL;
        if (s->raw_size && (!data || !xdisc_read(&g->disc, &g->xbe_file, s->raw_off, data, s->raw_size))) {
            free(data);
            free(hdr);
            r->sec_state[i] = SEC_FAILED;
            fail(r, "No se pudo leer la sección", s->name);
            return false;
        }
        uint32_t n = s->raw_size < s->vsize ? s->raw_size : s->vsize;
        if (n) {
            vm_write(vm, s->va, data, n);
        }
        uint8_t want[20];
        bool good = vm_read(vm, sec_hdrs + i * SEC_HDR_SIZE + SEC_DIGEST, want, 20) &&
                    digest_matches(data ? data : (const uint8_t *)"", s->raw_size, want);
        free(data);
        r->sec_state[i] = good ? SEC_LOADED_OK : SEC_LOADED_BAD;
        if (good) {
            r->sha_ok++;
        } else {
            r->sha_bad++;
        }
        /* cargada una vez, como cuenta XeLoadSection */
        vm_wr32(vm, sec_hdrs + i * SEC_HDR_SIZE + SEC_REFS, 1);
        r->sections_loaded++;
    }
    free(hdr);

    uint32_t backed = 0;
    for (uint32_t p = base; p < base + r->span; p += VM_PAGE) {
        backed += vm_phys(vm, p) != VM_NO_PHYS ? VM_PAGE : 0;
    }
    r->backed_kb = backed >> 10;

    /* tabla de thunks del kernel: 0x80000000 | ordinal, termina en 0 */
    for (uint32_t t = x->kernel_thunk; r->nimports < K_ORDINAL_MAX; t += 4) {
        if (vm_phys(vm, t) == VM_NO_PHYS) {
            fail(r, "La tabla del kernel no está en memoria", NULL);
            return false;
        }
        uint32_t v = vm_rd32(vm, t);
        if (!v) {
            break;
        }
        if (!(v & 0x80000000u)) {
            fail(r, "Un thunk del kernel no es un ordinal", NULL);
            return false;
        }
        unsigned ord = v & 0x7FFFFFFFu;
        const KExport *e = kernel_export(ord);
        uint32_t target = kernel_resolve(k, ord);
        if (!e || !target) {
            char b[32];
            snprintf(b, sizeof(b), "%u", ord);
            fail(r, "Ordinal del kernel desconocido:", b);
            return false;
        }
        vm_wr32(vm, t, target);
        LoadImport *im = &r->imports[r->nimports++];
        im->ordinal = ord;
        im->name = e->name;
        im->kind = e->kind;
        im->target = target;
        if (e->kind == KX_DATA) {
            r->n_data++;
        } else if (e->kind == KX_IMPL) {
            r->n_impl++;
        } else {
            r->n_todo++;
        }
    }

    vm_read(vm, x->entry, r->entry_bytes, sizeof(r->entry_bytes));
    r->ram_used_kb = vm->pages_used * (VM_PAGE / 1024);
    r->ok = true;
    return true;
}
