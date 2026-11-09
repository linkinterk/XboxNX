/*
 * XboxNX — área del kernel de Xbox (etapa 2a).
 *
 * Igual que hakuX (native/kernel/kernel.c, GPL-3.0-or-later): el kernel vive
 * en 0x80010000 (física 0x10000). Cada función exportada tiene un trampolín
 * de 8 bytes "int 0x3F; ordinal; hlt" que entra al kernel nativo (etapa 3), y
 * cada dato exportado una casilla de 0x40 bytes. Una cabecera PE con
 * directorio de exportaciones deja que el juego las busque por ordinal.
 */
#ifndef XBOXNX_KERNEL_H
#define XBOXNX_KERNEL_H

#include <stdbool.h>
#include <stdint.h>

#include "vm.h"

#define K_VA          0x80010000u
#define K_PA          0x00010000u
#define K_SIZE        0x00040000u
#define K_CALL_VECTOR 0x3Fu
#define K_ORDINAL_MAX 512u

typedef enum { KX_DATA, KX_IMPL, KX_TODO } KxKind;
typedef enum { KC_STD, KC_FAST, KC_CDECL, KC_DATA } KcConv;

typedef struct KExport {
    unsigned ordinal;
    const char *name;
    KxKind kind;
    KcConv conv;
    unsigned arg_bytes;   /* bytes de argumentos (fastcall: incluye ECX/EDX) */
} KExport;

/* Dónde está cada cosa del kernel (área en K_VA). */
#define K_STUB_HLT     (K_VA + 0x0F00u)  /* hlt */
#define K_STUB_RETURN  (K_VA + 0x0F08u)  /* el hilo volvió de su rutina: int 0x3F, 0x3FF */
#define K_ORD_THREAD_RETURN 0x3FFu
#define K_KPCR_VA      (K_VA + 0x10000u)
#define K_ETHREAD_VA   (K_VA + 0x14000u)

/* Bytes que la llamada saca de la pila al volver (stdcall/fastcall). */
unsigned kernel_pop_bytes(const KExport *e);


typedef struct Kernel {
    uint32_t tramp_va[K_ORDINAL_MAX];
    uint32_t data_va[K_ORDINAL_MAX];
} Kernel;

bool kernel_init(Kernel *k, Vm *vm);
const KExport *kernel_export(unsigned ordinal);
/* Dirección del trampolín o del dato; 0 si el ordinal no existe. */
uint32_t kernel_resolve(const Kernel *k, unsigned ordinal);
/* Cuántas exportaciones hay de cada tipo. */
void kernel_counts(unsigned *data, unsigned *impl, unsigned *todo);
/* Dirección de un dato exportado por nombre (0 si no existe). */
uint32_t kernel_data_va(const Kernel *k, const char *name);

#endif
