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
    uint8_t seen[K_ORDINAL_MAX];
    uint64_t ns;             /* tiempo de CPU del intérprete */
    uint64_t quantum_left;   /* instrucciones hasta cambiar de hilo */
    /* HalReturnToFirmware: lo que el juego dejó en LaunchDataPage */
    bool has_launch;
    uint32_t launch_type, launch_title;
    char launch_path[96];
} Runner;

/* Registro visible: las primeras RUN_LOG_HEAD y las últimas RUN_LOG_TAIL. */
unsigned runner_log_len(const Runner *r);
/* Entrada j del registro visible; *number = número de llamada (desde 0). */
const KCallRec *runner_log_at(const Runner *r, unsigned j, unsigned *number);

/* Prepara el hilo de arranque; false si no hay memoria para la pila. */
bool runner_boot(Runner *r, Vm *vm, Kernel *k, const LoadReport *load);
/* Ejecuta hasta max instrucciones; devuelve el estado. */
RunState runner_slice(Runner *r, uint64_t max);

#endif
