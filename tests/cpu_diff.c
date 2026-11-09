/*
 * XboxNX — conductor de pruebas del intérprete para tests/cpu_diff.py.
 *
 * Lee casos por stdin, uno por línea:
 *   <código hex> <8 registros> <eflags> <memoria hex 256 bytes> <pila hex 32 bytes en ESP0-16>
 * Ejecuta UNA instrucción en 0x1000 con la memoria en 0x100000 y escribe:
 *   <stop> <eip> <8 registros> <eflags> <memoria hex>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/cpu.h"

#define CODE_VA 0x1000u
#define MEM_VA  0x100000u
#define MEM_LEN 256u
#define STACK_VA 0x200000u

static int hexval(char ch)
{
    return ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10;
}

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        out[n++] = (uint8_t)(hexval(s[0]) << 4 | hexval(s[1]));
        s += 2;
    }
    return n;
}

int main(void)
{
    static Vm vm;
    if (!vm_init(&vm) || !vm_alloc_at(&vm, CODE_VA, 0x1000) || !vm_alloc_at(&vm, MEM_VA, 0x1000) ||
        !vm_alloc_at(&vm, STACK_VA - 0x1000, 0x2000)) {
        return 1;
    }
    static char line[8192], code_hex[256], mem_hex[1024], stk_hex[128];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned r[9];
        if (sscanf(line, "%255s %x %x %x %x %x %x %x %x %x %1023s %127s", code_hex, &r[0], &r[1], &r[2], &r[3],
                   &r[4], &r[5], &r[6], &r[7], &r[8], mem_hex, stk_hex) != 12) {
            continue;
        }
        uint8_t code[64] = {0}, mem[MEM_LEN];
        size_t n = unhex(code_hex, code, sizeof(code));
        unhex(mem_hex, mem, MEM_LEN);
        uint8_t fill[0x1000];
        memset(fill, 0x90, sizeof(fill));
        vm_write(&vm, CODE_VA, fill, sizeof(fill));
        vm_write(&vm, CODE_VA, code, n);
        vm_write(&vm, MEM_VA, mem, MEM_LEN);
        static uint8_t zero[0x2000];
        vm_write(&vm, STACK_VA - 0x1000, zero, sizeof(zero));
        uint8_t stk_in[32];
        unhex(stk_hex, stk_in, sizeof(stk_in));
        vm_write(&vm, STACK_VA - 16, stk_in, sizeof(stk_in));
        Cpu c;
        cpu_init(&c, &vm);
        for (int i = 0; i < 8; i++) {
            c.r[i] = r[i];
        }
        c.flags = r[8];
        c.eip = CODE_VA;
        CpuStop st = cpu_step(&c);
        vm_read(&vm, MEM_VA, mem, MEM_LEN);
        printf("%d %x", (int)st, c.eip);
        for (int i = 0; i < 8; i++) {
            printf(" %x", c.r[i]);
        }
        printf(" %x ", c.flags);
        for (unsigned i = 0; i < MEM_LEN; i++) {
            printf("%02x", mem[i]);
        }
        /* la pila cerca de ESP inicial */
        uint8_t stk[32];
        vm_read(&vm, STACK_VA - 16, stk, sizeof(stk));
        printf(" ");
        for (unsigned i = 0; i < sizeof(stk); i++) {
            printf("%02x", stk[i]);
        }
        printf("\n");
        fflush(stdout);
    }
    vm_free(&vm);
    return 0;
}
