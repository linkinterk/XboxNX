/*
 * XboxNX — qué instrucciones del juego deja el JIT en C, pesadas por el
 * tiempo de su bloque en perfil.txt (de la Switch). Solo ARM64 (qemu).
 * Uso: slow_scan ram.bin perfil.txt [más perfil.txt...]
 * Escribe: peso (suma del % de los bloques), veces, y la forma (bytes de
 * prefijos + opcode + modrm/reg).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/cpu.h"

unsigned cpu_test_slow(Cpu *c, uint32_t eip, uint32_t *starts, unsigned *ninsn);

typedef struct {
    char key[32];
    double w;
    unsigned n;
    uint32_t ex; /* un ejemplo: dónde está */
} Ent;

static Ent g_e[4096];
static unsigned g_ne;
static double g_tot, g_slow_w;
static unsigned long g_insns, g_slow;

static void add(const char *key, double w, uint32_t at)
{
    for (unsigned i = 0; i < g_ne; i++) {
        if (!strcmp(g_e[i].key, key)) {
            g_e[i].w += w;
            g_e[i].n++;
            return;
        }
    }
    if (g_ne < 4096) {
        snprintf(g_e[g_ne].key, sizeof(g_e[g_ne].key), "%s", key);
        g_e[g_ne].w = w;
        g_e[g_ne].n = 1;
        g_e[g_ne].ex = at;
        g_ne++;
    }
}

static int cmp(const void *a, const void *b)
{
    double d = ((const Ent *)b)->w - ((const Ent *)a)->w;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static int hexval(char ch) { return ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10; }

static uint8_t g_ram[64u << 20];

int main(int argc, char **argv)
{
    if (argc < 3) {
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        return 1;
    }
    size_t rn = fread(g_ram, 1, sizeof(g_ram), f);
    fclose(f);
    static Vm vm;
    if (!vm_init(&vm)) {
        return 1;
    }
    /* todo el código del juego (como en la RAM capturada) */
    for (uint32_t a = 0x10000; a + 0x1000 <= rn && a < 0x01000000u; a += 0x1000) {
        vm_alloc_at(&vm, a, 0x1000);
        vm_write(&vm, a, g_ram + a, 0x1000);
    }
    Cpu c;
    cpu_init(&c, &vm);
    c.use_blocks = true;
    c.jit_hot = 1;
    for (int ai = 2; ai < argc; ai++) {
        f = fopen(argv[ai], "r");
        if (!f) {
            continue;
        }
        static char line[4096];
        while (fgets(line, sizeof(line), f)) {
            unsigned eip;
            double pct;
            char hex[1024];
            if (sscanf(line, "%8x %lf%% %1023s", &eip, &pct, hex) != 3 || eip < 0x10000 || eip >= 0x01000000u) {
                continue;
            }
            /* los bytes del perfil (lo que corría en la Switch) encima */
            uint8_t by[512];
            size_t n = 0;
            for (const char *s = hex; s[0] && s[1] && n < sizeof(by); s += 2) {
                by[n++] = (uint8_t)(hexval(s[0]) << 4 | hexval(s[1]));
            }
            vm_write(&vm, eip, by, n);
            uint32_t st[64];
            unsigned ni = 0, k = cpu_test_slow(&c, eip, st, &ni);
            g_tot += pct;
            g_insns += ni;
            g_slow += k;
            if (k) {
                g_slow_w += pct;
            }
            for (unsigned j = 0; j < k; j++) {
                uint8_t b[16];
                vm_read(&vm, st[j], b, sizeof(b));
                char key[32];
                int o = 0, p = 0;
                while (p < 4 && (b[p] == 0x66 || b[p] == 0xF2 || b[p] == 0xF3 || b[p] == 0x64 || b[p] == 0xF0 ||
                                 b[p] == 0x2E || b[p] == 0x3E || b[p] == 0x26 || b[p] == 0x36)) {
                    o += snprintf(key + o, sizeof(key) - o, "%02X ", b[p++]);
                }
                unsigned op = b[p++];
                o += snprintf(key + o, sizeof(key) - o, "%02X", op);
                if (op == 0x0F) {
                    unsigned op2 = b[p++];
                    o += snprintf(key + o, sizeof(key) - o, "%02X", op2);
                    unsigned m = b[p];
                    snprintf(key + o, sizeof(key) - o, m >= 0xC0 ? " r,r" : " m");
                } else if (op >= 0xD8 && op <= 0xDF) {
                    unsigned m = b[p];
                    if (m >= 0xC0) {
                        snprintf(key + o, sizeof(key) - o, " %02X", m);
                    } else {
                        snprintf(key + o, sizeof(key) - o, " /%u m", (m >> 3) & 7);
                    }
                } else {
                    unsigned m = b[p];
                    snprintf(key + o, sizeof(key) - o, " /%u%s", (m >> 3) & 7, m >= 0xC0 ? " r" : " m");
                }
                add(key, pct, st[j]);
            }
        }
        fclose(f);
    }
    qsort(g_e, g_ne, sizeof(Ent), cmp);
    printf("bloques %.1f%% del tiempo, instrucciones %lu, lentas %lu (%.1f%%); bloques con alguna lenta %.1f%%\n", g_tot,
           g_insns, g_slow, g_insns ? 100.0 * g_slow / g_insns : 0.0, g_slow_w);
    int top = getenv("TOP") ? atoi(getenv("TOP")) : 40;
    for (unsigned i = 0; i < g_ne && (int)i < top; i++) {
        uint8_t by[12];
        vm_read(&vm, g_e[i].ex, by, sizeof(by));
        printf("%7.2f  %4u  %-14s  %08X:", g_e[i].w, g_e[i].n, g_e[i].key, g_e[i].ex);
        for (int k = 0; k < 12; k++) {
            printf(" %02X", by[k]);
        }
        printf("\n");
    }
    return 0;
}
