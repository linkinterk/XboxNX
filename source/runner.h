/*
 * XboxNX — arranque del juego en el intérprete (etapa 2c).
 *
 * Prepara el primer hilo como el kernel del Xbox (KPCR en fs:, ETHREAD,
 * pila con su área FX, como hakuX en native/kernel/ps.c), salta al punto de
 * entrada del XBE y registra cada llamada al kernel. Como el kernel nativo
 * es la etapa 3, cada llamada se "simula": devuelve 0 (éxito) y saca sus
 * argumentos de la pila. Sirve para ver qué pide Halo 2 al arrancar.
 */
#ifndef XBOXNX_RUNNER_H
#define XBOXNX_RUNNER_H

#include "cpu.h"
#include "kernel.h"
#include "loader.h"

#define RUN_MAX_CALLS 200

typedef struct KCallRec {
    unsigned ordinal;
    const char *name;
    KxKind kind;
    uint32_t caller;     /* dirección de retorno */
    uint32_t args[4];
    uint64_t icount;     /* instrucciones ejecutadas hasta la llamada */
} KCallRec;

typedef enum {
    RUN_ACTIVE,
    RUN_THREAD_END,   /* el hilo terminó (PsTerminateSystemThread o volvió) */
    RUN_CPU_STOP,     /* el intérprete paró (instrucción que falta, fallo...) */
    RUN_CALL_LIMIT,   /* demasiadas llamadas registradas */
    RUN_SYSTEM_STOP,  /* el juego pidió reiniciar o un error fatal */
} RunState;

typedef struct Runner {
    Cpu cpu;
    Kernel *k;
    RunState state;
    char end[128];
    uint32_t entry, stack_base, stack_limit;
    KCallRec calls[RUN_MAX_CALLS];
    unsigned ncalls;
    unsigned distinct;
    uint64_t ns;             /* tiempo de CPU del intérprete */
} Runner;

/* Prepara el hilo de arranque; false si no hay memoria para la pila. */
bool runner_boot(Runner *r, Vm *vm, Kernel *k, const LoadReport *load);
/* Ejecuta hasta max instrucciones; devuelve el estado. */
RunState runner_slice(Runner *r, uint64_t max);

#endif
