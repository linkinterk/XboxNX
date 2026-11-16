/*
 * XboxNX — cuánto código genera el JIT para los bloques de perfil.txt,
 * pesado por su tiempo (la medida para mejorar el JIT con un perfil plano).
 * Solo ARM64 (qemu). Uso: PAGINAS=paginas.bin hot_jit ram.bin perfil.txt...
 * HOT_EIP=xxxx: además escribe el código ARM de ese bloque en JIT_DUMP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/cpu.h"

size_t cpu_test_jit(Cpu *c, uint32_t eip, unsigned *ninsn, size_t *code_bytes);
const void *cpu_test_jit_code(void);

static uint8_t g_ram[64u << 20];

typedef struct {
    uint32_t eip;
    double w;
} Hot;

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
    static uint32_t pt[1u << 20];
    const char *pg = getenv("PAGINAS");
    FILE *pf = pg ? fopen(pg, "rb") : NULL;
    if (!pf || fread(pt, 4, 1u << 20, pf) != (1u << 20)) {
        return 1;
    }
    fclose(pf);
    for (uint32_t v = 0x10; v < 0x80000; v++) {
        if (pt[v] && (size_t)pt[v] * 0x1000 <= rn) {
            vm_alloc_at(&vm, v << 12, 0x1000);
            vm_write(&vm, v << 12, g_ram + (size_t)(pt[v] - 1) * 0x1000, 0x1000);
        }
    }
    static Hot hot[8192];
    unsigned nh = 0;
    for (int ai = 2; ai < argc; ai++) {
        f = fopen(argv[ai], "r");
        if (!f) {
            continue;
        }
        static char line[4096];
        while (fgets(line, sizeof(line), f)) {
            unsigned eip;
            double pct;
            if (sscanf(line, "%8x %lf%%", &eip, &pct) != 2 || eip < 0x10000) {
                continue;
            }
            unsigned i = 0;
            while (i < nh && hot[i].eip != eip) {
                i++;
            }
            if (i == nh && nh < 8192) {
                hot[nh++] = (Hot){eip, 0};
            }
            if (i < nh) {
                hot[i].w += pct;
            }
        }
        fclose(f);
    }
    uint32_t want = getenv("HOT_EIP") ? (uint32_t)strtoul(getenv("HOT_EIP"), 0, 16) : 0;
    char *dump = getenv("JIT_DUMP");
    double wi = 0, wa = 0, tw = 0;
    int verbose = getenv("HOT_V") != NULL;
    for (unsigned i = 0; i < nh; i++) {
        Cpu c;
        cpu_init(&c, &vm);
        c.use_blocks = true;
        c.jit_hot = 1;
        if (dump && hot[i].eip != want) {
            unsetenv("JIT_DUMP");
        }
        unsigned ni = 0;
        size_t cb = 0;
        if (!cpu_test_jit(&c, hot[i].eip, &ni, &cb) || !ni) {
            if (dump) {
                setenv("JIT_DUMP", dump, 1);
            }
            continue;
        }
        if (dump) {
            setenv("JIT_DUMP", dump, 1);
        }
        double arm = cb / 4.0;
        wi += hot[i].w * ni;
        wa += hot[i].w * arm;
        tw += hot[i].w;
        if (getenv("HOT_DIR")) { /* el código de cada bloque, para tools: camino rápido */
            char fn[512];
            snprintf(fn, sizeof(fn), "%s/%08X_%.3f_%u.bin", getenv("HOT_DIR"), hot[i].eip, hot[i].w, ni);
            FILE *o = fopen(fn, "wb");
            if (o) {
                fwrite(cpu_test_jit_code(), 1, cb, o);
                fclose(o);
            }
        }
        if (verbose) {
            printf("%08X %6.2f%%  x86 %3u  ARM %5.0f  %.2f\n", hot[i].eip, hot[i].w, ni, arm, arm / ni);
        }
    }
    printf("bloques %u (%.1f%% del tiempo) · ARM por x86 (pesado por tiempo) %.3f\n", nh, tw, wa / wi);
    return 0;
}
