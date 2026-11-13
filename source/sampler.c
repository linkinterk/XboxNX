/*
 * XboxNX — etapa 5: muestreo del tiempo del hilo principal.
 *
 * Un hilo aparte (núcleo 2) mira cada ~250 µs qué bloque del JIT corre
 * (Cpu.jit_cur, que el código traducido escribe al entrar a cada bloque) o,
 * fuera del JIT, en qué parte está (prof.h). Cada ~30 s escribe en
 * sdmc:/switch/XboxNX/perfil.txt los bloques donde más tiempo se va, con los
 * bytes de su código x86: así se sabe qué optimizar con datos de la Switch.
 */
#include "sampler.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "prof.h"

#ifdef __SWITCH__
#include <switch.h>
#define PERFIL_FILE "sdmc:/switch/XboxNX/perfil.txt"
#endif

#define HSIZE 8192u

typedef struct {
    uint32_t eip, n;
} Slot;

static Slot g_h[HSIZE];
static uint32_t g_total, g_out[PROF_N + 1];
static const volatile uint32_t *g_cur;
static Vm *g_vm;
static const char *g_version;

static void add(uint32_t eip)
{
    unsigned h = (eip * 2654435761u) >> 19;
    for (unsigned i = 0; i < HSIZE; i++) {
        Slot *s = &g_h[(h + i) & (HSIZE - 1)];
        if (s->eip == eip) {
            s->n++;
            return;
        }
        if (!s->eip) {
            s->eip = eip;
            s->n = 1;
            return;
        }
    }
}

#ifdef __SWITCH__
static Thread g_thr;
static bool g_on;

static void report(FILE *f, unsigned secs)
{
    fprintf(f, "\n=== %u s · %u muestras ===\n", secs, (unsigned)g_total);
    if (!g_total) {
        return;
    }
    uint32_t in_jit = 0;
    for (unsigned i = 0; i < HSIZE; i++) {
        in_jit += g_h[i].n;
    }
    fprintf(f, "Código del juego (JIT) %.1f%%; lo demás:", 100.0 * in_jit / g_total);
    for (int p = 0; p <= PROF_N; p++) {
        if (g_out[p]) {
            fprintf(f, " %s %.1f%%", p < PROF_N ? prof_name((ProfPart)p) : "otro", 100.0 * g_out[p] / g_total);
        }
    }
    fprintf(f, "\n");
    /* los 24 bloques con más muestras */
    for (int t = 0; t < 24; t++) {
        unsigned best = HSIZE;
        for (unsigned i = 0; i < HSIZE; i++) {
            if (g_h[i].n && (best == HSIZE || g_h[i].n > g_h[best].n)) {
                best = i;
            }
        }
        if (best == HSIZE) {
            break;
        }
        uint8_t code[96];
        memset(code, 0, sizeof(code));
        if (g_vm) {
            vm_read(g_vm, g_h[best].eip, code, sizeof(code));
        }
        fprintf(f, "%08X %5.2f%% ", (unsigned)g_h[best].eip, 100.0 * g_h[best].n / g_total);
        for (unsigned k = 0; k < sizeof(code); k++) {
            fprintf(f, "%02X", code[k]);
        }
        fprintf(f, "\n");
        g_h[best].n = 0; /* ya salió (se borra todo después) */
    }
}

static void thr(void *arg)
{
    (void)arg;
    FILE *f = fopen(PERFIL_FILE, "w");
    if (f) {
        fprintf(f, "XboxNX v%s · perfil del hilo principal (cada ~30 s: %% del tiempo por bloque, y 96 bytes de su código x86)\n",
                g_version ? g_version : "?");
        fclose(f);
    }
    unsigned secs = 0;
    uint64_t t0 = armGetSystemTick();
    while (g_on) {
        svcSleepThread(250000);
        uint32_t eip = *g_cur;
        int p = prof_now();
        if (eip && (p < 0 || p == PROF_CPU)) {
            add(eip); /* en el código del juego (o en lo que llama del CPU) */
        } else {
            /* fuera del JIT, o dentro pero en el GPU/kernel/audio (lo que una
             * escritura a un registro dispara): la parte */
            g_out[p >= 0 && p < PROF_N ? p : PROF_N]++;
        }
        g_total++;
        uint64_t now = armGetSystemTick();
        if (armTicksToNs(now - t0) >= 30000000000ull) {
            secs += 30;
            t0 = now;
            f = fopen(PERFIL_FILE, "a");
            if (f) {
                report(f, secs);
                fclose(f);
            }
            memset(g_h, 0, sizeof(g_h));
            memset(g_out, 0, sizeof(g_out));
            g_total = 0;
        }
    }
}

void sampler_start(const volatile uint32_t *cur, Vm *vm, const char *version)
{
    if (g_on) {
        return;
    }
    g_cur = cur;
    g_vm = vm;
    g_version = version;
    g_on = true;
    if (R_FAILED(threadCreate(&g_thr, thr, NULL, NULL, 0x8000, 0x2B, 2)) || R_FAILED(threadStart(&g_thr))) {
        g_on = false;
    }
}
#else
void sampler_start(const volatile uint32_t *cur, Vm *vm, const char *version)
{
    (void)cur;
    (void)vm;
    (void)version;
    (void)add;
    (void)g_total;
    (void)g_out;
    (void)g_cur;
    (void)g_vm;
    (void)g_version;
}
#endif
