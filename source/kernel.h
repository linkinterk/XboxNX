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

#include "cpu.h"
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
#define K_STUB_DPC_RETURN  (K_VA + 0x0F10u)  /* un DPC volvió: int 0x3F, 0x3FE */
#define K_ORD_DPC_RETURN    0x3FEu
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

#define ST_TIMEOUT                0x00000102u
#define ST_INVALID_DEVICE_STATE   0xC0000184u

#define K_MAX_THREADS 64
#define K_MAX_WAIT    64
#define K_MAX_TIMERS  128
#define K_MAX_DPCS    64

/* Tiempo virtual: 1 ns por instrucción (el Pentium III de 733 MHz del Xbox
 * hace ~1 por ciclo). Cuando todos esperan, salta al siguiente plazo. */
#define K_NS_PER_INSTR 1u

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
    bool block;      /* la llamada dejó al hilo esperando: EAX llega al despertar */
    bool resched;    /* replanificar al volver (ceder, hilo nuevo, terminar) */
    bool ended;      /* el hilo actual terminó */
    char note[64];   /* detalle para el registro (el archivo que abre...) */
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

typedef enum { KT_FREE, KT_READY, KT_RUNNING, KT_WAITING, KT_DEAD } KtState;

typedef struct KCtx {
    uint32_t r[8], eip, flags;
    uint16_t fpu_cw, fpu_sw;
    uint32_t mxcsr;
    CpuFp fp;        /* x87, MMX y SSE */
} KCtx;

typedef struct KThread {
    KtState state;
    uint32_t id;
    uint32_t ethread, stack_base, stack_limit, tib, exception_list;
    uint32_t start;            /* rutina de inicio (para mostrar) */
    KCtx ctx;
    int priority;
    unsigned suspend;
    uint32_t exit_status;
    uint64_t instructions;     /* cuántas ejecutó */
    /* espera */
    uint32_t wait_obj[K_MAX_WAIT];
    unsigned nwait;
    bool wait_all;
    bool has_deadline;
    uint64_t deadline_ns;
    uint32_t wait_cs;          /* sección crítica que tomará al despertar */
    const char *wait_what;     /* qué llamada espera */
} KThread;

typedef struct KTimerRec {
    uint32_t timer, dpc;
    uint64_t due_ns, period_ns;
} KTimerRec;

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
    /* hilos y tiempo (kps.c) */
    KThread threads[K_MAX_THREADS];
    int cur;                     /* hilo que corre (índice), -1 ninguno */
    unsigned next_id;
    uint64_t now_ns;             /* tiempo virtual desde el arranque */
    unsigned quantum_count;
    KTimerRec timers[K_MAX_TIMERS];
    unsigned ntimers;
    uint32_t dpc_queue[K_MAX_DPCS];
    unsigned ndpc;
    bool in_dpc;
    KCtx dpc_saved;              /* contexto interrumpido por el DPC */
    uint32_t dpc_stack_top;
    unsigned dpcs_run, switches;
    /* archivos (kio.c) */
    const struct XDisc *disc;    /* el disco del juego (D:) */
    char hdd_root[192];          /* carpeta del disco duro virtual */
    unsigned files_open, reads, writes;
    uint64_t bytes_read;
    /* sistema (ksys.c) */
    uint32_t xbe_base, xbe_sector, xbe_size;  /* el XBE en memoria y en el disco */
    uint64_t boot_filetime;      /* hora real al arrancar (100 ns desde 1601) */
    int64_t time_offset;         /* lo que NtSetSystemTime movió el reloj */
    uint32_t av_saved;
} Kernel;

bool kernel_init(Kernel *k, Vm *vm);
/* Registra la implementación de una exportación por nombre. */
void kernel_impl(Kernel *k, const char *name, KFn fn);
/* Cuántas funciones tienen implementación real aquí. */
unsigned kernel_real_count(const Kernel *k);

/* Lo que el kernel deja listo al cargar el juego: XeImageFileName (la ruta
 * del XBE, como hakuX: \Device\CdRom0\default.xbe). */
bool kernel_title_loaded(Kernel *k, const char *image_path);

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

/* kps.c: hilos, esperas y tiempo */
/* kio.c: archivos, discos, enlaces simbólicos y dispositivos (etapa 3c) */
void kio_register(Kernel *k);
/* El disco del juego y la carpeta del disco duro virtual (se crea si falta). */
void kio_set_media(Kernel *k, const struct XDisc *disc, const char *hdd_root);
/* Se cerró el último handle de un objeto archivo. */
void kio_close(Kernel *k, uint32_t object);
/* El evento del objeto archivo (para esperar sobre su handle). */
#define K_FO_EVENT 0x38u

/* ksys.c: Rtl, configuración (EEPROM), cerrojos de lectura/escritura,
 * secciones del XBE, HAL, AV y criptografía Xc (etapa 3d) */
void ksys_register(Kernel *k);
/* Hora del sistema del Xbox (FILETIME). */
uint64_t kps_system_time(const Kernel *k);
/* Espera un objeto (para las funciones de otros módulos que bloquean). */
uint64_t kps_wait_one(KCall *c, uint32_t object, const char *what);

/* Un objeto quedó señalado: despierta a quien lo espera y, si es de más
 * prioridad que el hilo actual, pide replanificar. */
void kps_signaled(KCall *c);
void kps_register(Kernel *k);
/* Crea un hilo listo para correr en eip con la pila preparada por el que
 * llama (kps_thread_push). Devuelve el índice o -1. */
int kps_thread_create(Kernel *k, uint32_t stack_size, uint32_t tls_size, uint32_t ext_size);
/* Pone en la pila del hilo los dwords v[0..n) (v[0] queda arriba) y su eip. */
void kps_thread_start(Kernel *k, int t, uint32_t eip, const uint32_t *v, unsigned n);
/* Guarda/carga el contexto del CPU en/desde el hilo actual. */
void kps_save(Kernel *k, struct Cpu *c);
void kps_load(Kernel *k, struct Cpu *c);
/* Avanza el tiempo, dispara temporizadores, despierta esperas y elige el
 * siguiente hilo (o un DPC). Devuelve false si no hay nada que correr. */
bool kps_schedule(Kernel *k, struct Cpu *c, bool preempt);
/* El hilo actual terminó con este estado. */
void kps_exit(Kernel *k, uint32_t status);
/* Vuelve de un DPC (stub 0x3FE). */
void kps_dpc_return(Kernel *k, struct Cpu *c);
unsigned kps_alive(const Kernel *k);
/* Siguiente plazo (de espera o temporizador); false si no hay. */
bool kps_next_deadline(const Kernel *k, uint64_t *ns);
void kps_update_tick(Kernel *k);

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
