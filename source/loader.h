/*
 * XboxNX — carga el XBE en la memoria del Xbox (etapa 2a).
 *
 * Como nc_xbe_load de hakuX (native/core/xbe_load.c, GPL-3.0-or-later):
 * cabeceras en la base, secciones "preload" en sus direcciones, la tabla de
 * thunks del kernel resuelta a los trampolines y datos del kernel. Además
 * verifica cada sección contra el SHA-1 que trae su cabecera.
 */
#ifndef XBOXNX_LOADER_H
#define XBOXNX_LOADER_H

#include "game.h"
#include "kernel.h"
#include "vm.h"

typedef enum {
    SEC_LOADED_OK,    /* cargada y su SHA-1 coincide */
    SEC_LOADED_BAD,   /* cargada pero el SHA-1 no coincide */
    SEC_LATER,        /* no es "preload": la carga el juego (XeLoadSection) */
    SEC_FAILED,
} SecState;

typedef struct LoadImport {
    unsigned ordinal;
    const char *name;
    KxKind kind;
    uint32_t target;
} LoadImport;

typedef struct LoadReport {
    bool ok;
    char error[128];
    uint32_t base, span, entry, tls_dir, stack_commit;
    uint32_t backed_kb;          /* KB de la imagen con memoria */
    uint32_t ram_used_kb;        /* KB de RAM del Xbox ocupados (kernel + juego) */
    unsigned sections_total, sections_loaded, sections_later, sha_ok, sha_bad;
    uint8_t sec_state[XBE_MAX_SECTIONS];
    unsigned nimports, n_impl, n_data, n_todo;
    LoadImport imports[K_ORDINAL_MAX];
    uint8_t entry_bytes[16];     /* las primeras instrucciones x86 del juego */
    unsigned ms;                 /* cuánto tardó */
} LoadReport;

bool loader_load(Vm *vm, Kernel *k, const Game *g, LoadReport *r);

#endif
