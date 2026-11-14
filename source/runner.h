/*
 * XboxNX — ejecución del juego en el intérprete.
 *
 * Crea el hilo de arranque como el kernel del Xbox (KPCR en fs:, ETHREAD,
 * pila con su área FX), salta al punto de entrada del XBE y reparte la CPU
 * entre los hilos del juego (etapa 3b). Cada llamada al kernel se registra;
 * las que aún no tienen implementación se "simulan" con un 0.
 */
#ifndef XBOXNX_RUNNER_H
#define XBOXNX_RUNNER_H

#include "cpu.h"
#include "kernel.h"
#include "loader.h"

#define RUN_LOG_HEAD 150   /* las primeras llamadas se guardan siempre */
#define RUN_LOG_TAIL 250   /* y las últimas, en anillo */

typedef struct KCallRec {
    unsigned ordinal;
    const char *name;
    KxKind kind;
    uint32_t caller;     /* dirección de retorno */
    uint32_t args[4];
    uint64_t icount;     /* instrucciones ejecutadas hasta la llamada */
    bool real;           /* la contestó una implementación real (no simulada) */
    uint32_t ret;        /* lo que devolvió (EAX) */
    uint32_t thread;     /* id del hilo que llamó */
    char note[64];       /* detalle (el archivo que abrió...) */
} KCallRec;

typedef enum {
    RUN_ACTIVE,
    RUN_THREAD_END,   /* el hilo terminó (PsTerminateSystemThread o volvió) */
    RUN_CPU_STOP,     /* el intérprete paró (instrucción que falta, fallo...) */
    RUN_CALL_LIMIT,   /* (ya no se usa) */
    RUN_SYSTEM_STOP,  /* el juego pidió reiniciar o un error fatal */
    RUN_DEADLOCK,     /* todos los hilos esperan algo que nunca llegará */
} RunState;

typedef struct Runner {
    Cpu cpu;
    Kernel *k;
    RunState state;
    char end[128];
    uint32_t entry, stack_base, stack_limit;
    KCallRec head[RUN_LOG_HEAD];
    KCallRec tail[RUN_LOG_TAIL];
    unsigned ncalls;         /* total de llamadas */
    unsigned distinct;
    unsigned simulated;      /* llamadas contestadas con un 0 simulado */
    uint64_t idle_skips;     /* bucles de espera saltados */
    uint8_t seen[K_ORDINAL_MAX];
    uint32_t counts[K_ORDINAL_MAX]; /* llamadas de cada función del kernel */
    uint64_t kns[K_ORDINAL_MAX];    /* tiempo real en cada una (con el cambio de hilo que provoque) */
    uint64_t kns_total;
    uint64_t kfast;          /* etapa 5: contestadas sin salir de cpu_run (la hora) */
    uint32_t kf_who[8];      /* v0.18.62: quién pide la hora (dirección de vuelta), los más */
    uint64_t kf_n[8];
    uint64_t ns;             /* tiempo de CPU del intérprete */
    uint64_t quantum_left;   /* instrucciones hasta cambiar de hilo */
    /* HalReturnToFirmware: lo que el juego dejó en LaunchDataPage */
    bool has_launch;
    uint32_t launch_type, launch_title;
    char launch_path[96];
} Runner;

/* Registro de E/S de archivos (para el volcado): las últimas RUN_IO_LOG. */
#define RUN_IO_LOG 4096
typedef struct RunIoRec {
    uint32_t num;          /* número de llamada */
    uint16_t ordinal;
    uint16_t thread;
    uint32_t caller;
    uint32_t a[8];
    uint32_t ret;
    uint32_t iosb[2];      /* el IO_STATUS_BLOCK al volver (si hay) */
    uint32_t buf[4];       /* los primeros 16 bytes del búfer de información (Set/QueryInformationFile) */
    char note[48];
} RunIoRec;
/* La entrada j (0 = la más vieja) del registro de E/S, o NULL. */
const RunIoRec *runner_io_at(unsigned j);
unsigned runner_io_len(void);

/* Registro visible: las primeras RUN_LOG_HEAD y las últimas RUN_LOG_TAIL. */
unsigned runner_log_len(const Runner *r);
/* Entrada j del registro visible; *number = número de llamada (desde 0). */
const KCallRec *runner_log_at(const Runner *r, unsigned j, unsigned *number);

/* Prepara el hilo de arranque; false si no hay memoria para la pila. */
bool runner_boot(Runner *r, Vm *vm, Kernel *k, const LoadReport *load);
/* Ejecuta hasta max instrucciones; devuelve el estado. */
RunState runner_slice(Runner *r, uint64_t max);

#endif
