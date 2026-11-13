/*
 * XboxNX — los relojes de la Switch mientras corre la app.
 *
 * Un homebrew arranca con los relojes de modo portátil: CPU a 1020 MHz,
 * GPU a 307 MHz. El emulador está limitado por un núcleo de la CPU, así que
 * se sube a 1785 MHz (lo más alto que usan los juegos de Nintendo), la
 * memoria a 1600 MHz y el GPU a lo que usan los juegos (768 MHz en la base,
 * 460.8 MHz en portátil). Al salir se dejan como estaban.
 *
 * Idea tomada de Halo Content Evolved (Watanoge), escrito de nuevo aquí con
 * el servicio clkrst de libnx.
 */
#include "clocks.h"

#include <stdio.h>

#ifdef __SWITCH__
#include <switch.h>

#define CPU_HZ 1785000000u
#define MEM_HZ 1600000000u
#define GPU_DOCK_HZ 768000000u
#define GPU_HAND_HZ 460800000u

static u32 g_orig[3];   /* cpu, memoria, gpu (0: no se pudo leer) */
static bool g_raised;
static char g_info[96] = "relojes: sin cambiar";

static const PcvModuleId k_mod[3] = {PcvModuleId_CpuBus, PcvModuleId_EMC, PcvModuleId_GPU};

static u32 get_hz(PcvModuleId m)
{
    ClkrstSession s;
    u32 hz = 0;
    if (R_SUCCEEDED(clkrstOpenSession(&s, m, 3))) {
        clkrstGetClockRate(&s, &hz);
        clkrstCloseSession(&s);
    }
    return hz;
}

static void set_hz(PcvModuleId m, u32 hz)
{
    ClkrstSession s;
    if (hz && R_SUCCEEDED(clkrstOpenSession(&s, m, 3))) {
        clkrstSetClockRate(&s, hz);
        clkrstCloseSession(&s);
    }
}

static void describe(const char *pre)
{
    snprintf(g_info, sizeof(g_info), "%sCPU %u MHz · memoria %u MHz · GPU %u MHz", pre,
             (unsigned)(get_hz(k_mod[0]) / 1000000), (unsigned)(get_hz(k_mod[1]) / 1000000),
             (unsigned)(get_hz(k_mod[2]) / 1000000));
}

void clocks_raise(void)
{
    if (g_raised || R_FAILED(clkrstInitialize())) {
        if (!g_raised) {
            snprintf(g_info, sizeof(g_info), "relojes: clkrst no disponible");
        }
        return;
    }
    for (int i = 0; i < 3; i++) {
        g_orig[i] = get_hz(k_mod[i]);
    }
    u32 gpu = appletGetOperationMode() == AppletOperationMode_Console ? GPU_DOCK_HZ : GPU_HAND_HZ;
    const u32 want[3] = {CPU_HZ, MEM_HZ, gpu};
    for (int i = 0; i < 3; i++) {
        if (g_orig[i] && g_orig[i] < want[i]) { /* nunca se baja lo que ya puso sys-clk */
            set_hz(k_mod[i], want[i]);
        }
    }
    g_raised = true;
    describe("");
    clkrstExit();
}

void clocks_restore(void)
{
    if (!g_raised || R_FAILED(clkrstInitialize())) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        set_hz(k_mod[i], g_orig[i]);
    }
    g_raised = false;
    clkrstExit();
}

const char *clocks_info(void) { return g_info; }

#else /* PC */

void clocks_raise(void) {}
void clocks_restore(void) {}
const char *clocks_info(void) { return "relojes: PC"; }

#endif
