/*
 * XboxNX — cierres del propio emulador (un acceso inválido en el código de
 * la Switch o en el traducido por el JIT).
 *
 * El manejador de excepciones de libnx escribe un informe en
 * sdmc:/switch/XboxNX/cierre.txt (dónde falló: pc, lr, la parte que corría,
 * la instrucción x86 del juego) y, si el hilo tenía una guardia puesta,
 * vuelve a ella con longjmp: la app no se cierra y la pantalla muestra el
 * fallo. Solo en la Switch; en la PC las funciones no hacen nada.
 */
#ifndef XBOXNX_CRASH_H
#define XBOXNX_CRASH_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CrashInfo {
    bool valid;
    uint32_t desc;           /* tipo de excepción de Horizon */
    uint64_t pc, lr, sp, far;
    uint64_t base, text_size; /* el código de XboxNX.nro (pc - base = dirección en el .elf) */
    uint64_t jit_rx, jit_size;
    uint32_t guest_eip;
    bool guarded;            /* el hilo tenía guardia: se recuperó */
    const char *part;        /* qué parte corría (prof) */
    uint64_t trace[10];      /* direcciones de vuelta halladas en la pila */
    int ntrace;
    uint64_t gpr[29];
} CrashInfo;

extern CrashInfo g_crash;

/* Si este hilo falla, volver con longjmp(*jb, 1). quiet: sin informe en la SD
 * (la prueba de excepciones). Una guardia por hilo. */
void crash_guard_push(jmp_buf *jb, bool quiet);
void crash_guard_pop(void);
/* Dónde leer la instrucción x86 que corría (para el informe). */
void crash_set_eip(const uint32_t *eip);
/* Una línea para la pantalla con lo que pasó en g_crash. */
void crash_summary(char *out, size_t n);
/* Las primeras líneas del informe de un cierre anterior; false si no hay. */
bool crash_last(char *out, size_t n);

#endif
