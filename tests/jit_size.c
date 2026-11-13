/*
 * XboxNX — cuánto ocupa el código del JIT con código real de Halo 2
 * (páginas de una captura de la RAM, tools/replay). Solo ARM64.
 * Uso: jit_size ram.bin [muestras]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/cpu.h"

size_t cpu_test_jit(Cpu *c, uint32_t eip, unsigned *ninsn, size_t *code_bytes);
uint32_t cpu_test_block_end(Cpu *c, uint32_t eip);

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        return 1;
    }
    static uint8_t ram[64u << 20];
    size_t n = fread(ram, 1, sizeof(ram), f);
    fclose(f);
    int want = argc > 2 ? atoi(argv[2]) : 4000;
    static Vm vm;
    if (!vm_init(&vm)) {
        return 1;
    }
    Cpu c;
    cpu_init(&c, &vm);
    c.use_blocks = true;
    c.jit_hot = 1;
    unsigned long long tot = 0, code = 0, insns = 0, blocks = 0;
    int got = 0;
    /* funciones: tras un ret (C3) o relleno (CC) que empiezan con push ebp / sub esp / push reg */
    for (size_t a = 0x10000; a + 16 < n && got < want; a++) {
        if (!((ram[a] == 0xC3 || ram[a] == 0xCC) && (ram[a + 1] == 0x55 || ram[a + 1] == 0x83 || ram[a + 1] == 0x56 ||
                                                      ram[a + 1] == 0x53 || ram[a + 1] == 0x8B))) {
            continue;
        }
        if (ram[a + 1] == 0x55 && ram[a + 2] != 0x8B) {
            continue;
        }
        uint32_t page = (uint32_t)(a + 1) & ~0xFFFu;
        if (!vm_alloc_at(&vm, page, 0x1000)) {
            /* ya está */
        }
        vm_write(&vm, page, ram + page, 0x1000);
        /* sigue la función: unos cuantos bloques seguidos */
        uint32_t eip = (uint32_t)a + 1;
        for (int k = 0; k < 6; k++) {
            unsigned ni = 0;
            size_t cb = 0;
            size_t t = cpu_test_jit(&c, eip, &ni, &cb);
            if (!t) {
                break;
            }
            tot += t;
            code += cb;
            insns += ni;
            blocks++;
            /* el siguiente: tras la última instrucción del bloque */
            const uint8_t *dummy = NULL;
            (void)dummy;
            eip = cpu_test_block_end(&c, eip);
            if (!eip || (eip & ~0xFFFu) != page) {
                break;
            }
        }
        got++;
    }
    printf("bloques %llu, instrucciones %llu (%.1f por bloque), código %llu bytes (%.0f por bloque, %.1f por instrucción), con copias %.0f por bloque\n",
           blocks, insns, blocks ? (double)insns / blocks : 0.0, code, blocks ? (double)code / blocks : 0.0,
           insns ? (double)code / insns : 0.0, blocks ? (double)tot / blocks : 0.0);
    return 0;
}
