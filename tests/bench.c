/*
 * XboxNX — medida de velocidad del intérprete en la PC: un bucle con
 * memoria, aritmética, pila y llamadas (16 instrucciones por vuelta).
 * BENCH_MMX=1: un bucle MMX como el del video Bink (12 por vuelta).
 * Uso: bench [millones de instrucciones]
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../source/cpu.h"

int main(int argc, char **argv)
{
    static Vm vm;
    if (!vm_init(&vm) || !vm_alloc_at(&vm, 0x1000, 0x1000) || !vm_alloc_at(&vm, 0x100000, 0x1000) ||
        !vm_alloc_at(&vm, 0x1FF000, 0x2000)) {
        return 1;
    }
    static const uint8_t code[] = {
        0x8B, 0x06, 0x01, 0xD8, 0x89, 0x07, 0x8D, 0x76, 0x04, 0x81, 0xE6, 0xFC, 0x0F, 0x10, 0x00, 0x50,
        0xE8, 0x0A, 0x00, 0x00, 0x00, 0x5A, 0x39, 0xD0, 0x75, 0x00, 0x43, 0x49, 0x75, 0xE2, 0xF4,
        0x6B, 0xC0, 0x03, 0x83, 0xF0, 0x55, 0xC3,
    };
    static const uint8_t mmx[] = {
        0x0F, 0x6F, 0x0E, 0x0F, 0x6F, 0x56, 0x08, 0x0F, 0xED, 0xCA, 0x0F, 0xD9, 0xCB, 0x0F, 0x61, 0xC8,
        0x0F, 0x72, 0xD2, 0x03, 0x0F, 0xE5, 0xCA, 0x0F, 0x7F, 0x0F, 0x0F, 0x67, 0xCA, 0x0F, 0xEF, 0xDB,
        0x49, 0x75, 0xDD, 0xF4,
    };
    bool use_mmx = getenv("BENCH_MMX") != NULL;
    if (use_mmx) {
        vm_write(&vm, 0x1000, mmx, sizeof(mmx));
    } else {
        vm_write(&vm, 0x1000, code, sizeof(code));
    }
    double mi = argc > 1 ? atof(argv[1]) : 200;
    Cpu c;
    cpu_init(&c, &vm);
    c.use_blocks = getenv("SIN_BLOQUES") == NULL;
    c.eip = 0x1000;
    c.r[R_ECX] = (uint32_t)(mi * 1e6 / (use_mmx ? 12 : 16));
    c.r[R_ESI] = c.r[R_EDI] = 0x100000;
    c.r[R_ESP] = 0x200000;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    CpuStop st = cpu_run(&c, (uint64_t)(mi * 2e6));
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    printf("%s: %.1f M instrucciones en %.2f s = %.1f MIPS (ebx %u)\n", cpu_stop_text(st), c.icount / 1e6, s,
           c.icount / 1e6 / s, c.r[R_EBX]);
    return 0;
}
