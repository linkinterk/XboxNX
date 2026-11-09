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


#define K_MAX_REGIONS 1024
#define K_MAX_CONTIG  2048
#define K_POOL_RANGES 1024
#define K_MAX_HANDLES 4096

#define ST_SUCCESS                0x00000000u
#define ST_INVALID_HANDLE         0xC0000008u
#define ST_INVALID_PARAMETER      0xC000000Du
#define ST_NO_MEMORY              0xC0000017u
#define ST_CONFLICTING_ADDRESSES  0xC0000018u
#define ST_OBJECT_TYPE_MISMATCH   0xC0000024u
#define ST_INSUFFICIENT_RESOURCES 0xC000009Au
#define ST_MEMORY_NOT_ALLOCATED   0xC00000A0u
#define ST_FREE_VM_NOT_AT_BASE    0xC000009Fu
#define ST_UNABLE_TO_FREE_VM      0xC000001Au
#define ST_NOT_COMMITTED          0xC000002Du

/* Tipos de objeto (su "Type" es el dato exportado del kernel con ese nombre). */
typedef enum {
    OB_THREAD, OB_EVENT, OB_SEMAPHORE, OB_MUTANT, OB_TIMER, OB_FILE, OB_DEVICE, OB_SYMLINK, OB_DIRECTORY,
    OB_TYPES
} ObType;

struct Kernel;
struct Cpu;

/* Una llamada al kernel en curso: argumentos ya leídos (stdcall: de la pila;
 * fastcall: ECX, EDX y luego la pila). */
typedef struct KCall {
    struct Kernel *k;
    Vm *vm;
    struct Cpu *cpu;
    uint32_t a[12];
} KCall;
typedef uint64_t (*KFn)(KCall *c);

typedef struct KRegion {
    uint32_t base, size, protect;
    bool image;
} KRegion;

typedef struct KContig {
    uint32_t va, size, protect;
} KContig;

typedef struct KRange {
    uint32_t va, len;
} KRange;

typedef struct KHandle {
    uint32_t object;
    uint8_t type;
} KHandle;

typedef struct Kernel {
    uint32_t tramp_va[K_ORDINAL_MAX];
    uint32_t data_va[K_ORDINAL_MAX];
    Vm *vm;
    KFn impl[K_ORDINAL_MAX];
    uint32_t type_va[OB_TYPES];
    uint32_t current_thread;     /* ETHREAD del hilo que corre */
    /* memoria (kmm.c) */
    KRegion regions[K_MAX_REGIONS];
    unsigned nregions;
    KContig contig[K_MAX_CONTIG];
    unsigned ncontig;
    uint32_t gpu_instance_end;
    uint32_t stack_next;         /* siguiente pila en el espacio del sistema */
    /* pool (kmm.c) */
    KRange pool_free[K_POOL_RANGES];
    unsigned pool_nfree;
    uint32_t pool_len, pool_used;
    /* objetos (kob.c) */
    KHandle handles[K_MAX_HANDLES];
    unsigned nobjects;
} Kernel;

bool kernel_init(Kernel *k, Vm *vm);
/* Registra la implementación de una exportación por nombre. */
void kernel_impl(Kernel *k, const char *name, KFn fn);
/* Cuántas funciones tienen implementación real aquí. */
unsigned kernel_real_count(const Kernel *k);

/* kmm.c: memoria */
void kmm_register(Kernel *k);
bool kmm_pool_init(Kernel *k);
uint32_t kmm_pool_alloc(Kernel *k, uint32_t size);
void kmm_pool_free(Kernel *k, uint32_t va, uint32_t size);
/* La imagen del XBE como una reserva más (NtAllocateVirtualMemory la respeta). */
void kmm_add_image(Kernel *k, uint32_t base, uint32_t size);
/* Una pila en el espacio del sistema (0xD0000000+): devuelve su base (lo más
 * bajo) o 0. */
uint32_t kmm_stack_alloc(Kernel *k, uint32_t size);

/* kob.c: objetos y handles */
void kob_register(Kernel *k);
uint32_t kob_create(Kernel *k, ObType type, uint32_t body_size);
uint32_t kob_handle(Kernel *k, uint32_t object, ObType type);
uint32_t kob_lookup(Kernel *k, uint32_t handle, ObType *type);
bool kob_close(Kernel *k, uint32_t handle);
unsigned kob_handles_open(const Kernel *k);
const KExport *kernel_export(unsigned ordinal);
/* Dirección del trampolín o del dato; 0 si el ordinal no existe. */
uint32_t kernel_resolve(const Kernel *k, unsigned ordinal);
/* Cuántas exportaciones hay de cada tipo. */
void kernel_counts(unsigned *data, unsigned *impl, unsigned *todo);
/* Dirección de un dato exportado por nombre (0 si no existe). */
uint32_t kernel_data_va(const Kernel *k, const char *name);

#endif
