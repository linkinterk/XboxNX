/*
 * XboxNX — intérprete x86 de 32 bits (etapa 2b, primera parte).
 *
 * Ejecuta el código del juego instrucción por instrucción sobre la memoria
 * del Xbox (vm.c). Es lento a propósito: sirve para validar el arranque y
 * después para comparar contra el traductor FEX. Se detiene en cada llamada
 * al kernel (int 0x3F de los trampolines) y en lo que todavía no sabe hacer.
 */
#ifndef XBOXNX_CPU_H
#define XBOXNX_CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "vm.h"

enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };

#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_TF 0x0100u
#define F_IF 0x0200u
#define F_DF 0x0400u
#define F_OF 0x0800u
#define F_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

typedef enum {
    CPU_RUNNING = 0,
    CPU_KCALL,    /* int 0x3F: llamada al kernel (kcall_ordinal) */
    CPU_HLT,
    CPU_BREAK,    /* int3 */
    CPU_UNIMPL,   /* instrucción que el intérprete aún no tiene */
    CPU_FAULT,    /* acceso a memoria sin mapear (fault_addr) */
    CPU_DIVIDE,   /* división por cero o desbordada */
    CPU_LIMIT,    /* se acabó el presupuesto de instrucciones */
} CpuStop;

#define CPU_TRACE 16

typedef union X128 {
    float f[4];
    uint32_t u[4];
    uint64_t q[2];
} X128;

/* Estado de coma flotante: x87 (en double), MMX y SSE. */
typedef struct CpuFp {
    double st[8];        /* registros físicos del x87 */
    uint8_t top;         /* ST(0) = st[top] */
    uint8_t empty;       /* bit i: st[i] vacío */
    bool mmx;            /* en modo MMX (hasta emms) */
    uint64_t mm[8];
    X128 xmm[8];
} CpuFp;

typedef struct Cpu {
    uint32_t r[8];
    uint32_t eip;
    uint32_t flags;
    uint32_t fs_base;
    uint16_t fpu_cw, fpu_sw;
    uint32_t mxcsr;
    CpuFp fp;
    uint64_t icount;
    Vm *vm;

    CpuStop stop;
    uint32_t stop_eip;        /* la instrucción donde paró */
    uint8_t stop_bytes[8];    /* sus bytes */
    uint32_t fault_addr;
    bool fault_write;
    unsigned kcall_ordinal;
    char why[64];             /* detalle (qué instrucción falta) */

    uint32_t trace[CPU_TRACE]; /* las últimas EIP */
    unsigned trace_pos;
} Cpu;

void cpu_init(Cpu *c, Vm *vm);
/* Ejecuta hasta max instrucciones o hasta parar; devuelve el motivo. */
CpuStop cpu_run(Cpu *c, uint64_t max);
/* Un paso; para pruebas. */
CpuStop cpu_step(Cpu *c);
const char *cpu_stop_text(CpuStop s);

#endif
