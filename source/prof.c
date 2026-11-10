/* XboxNX — en qué se va el tiempo real (ver prof.h). */
#include "prof.h"

#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
static uint64_t now(void) { return armTicksToNs(armGetSystemTick()); }
#else
#include <time.h>
static uint64_t now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
#endif

static uint64_t g_acc[PROF_N];
static ProfPart g_stack[16];
static int g_depth;
static uint64_t g_since;

void prof_push(ProfPart p)
{
    uint64_t t = now();
    if (g_depth > 0) {
        g_acc[g_stack[g_depth - 1]] += t - g_since;
    }
    if (g_depth < 16) {
        g_stack[g_depth] = p;
    }
    g_depth++;
    g_since = t;
}

void prof_pop(void)
{
    uint64_t t = now();
    if (g_depth > 0 && g_depth <= 16) {
        g_acc[g_stack[g_depth - 1]] += t - g_since;
    }
    if (g_depth > 0) {
        g_depth--;
    }
    g_since = t;
}

void prof_take(unsigned permille[PROF_N])
{
    uint64_t total = 0;
    for (int i = 0; i < PROF_N; i++) {
        total += g_acc[i];
    }
    for (int i = 0; i < PROF_N; i++) {
        permille[i] = total ? (unsigned)(g_acc[i] * 1000 / total) : 0;
    }
    memset(g_acc, 0, sizeof(g_acc));
}

const char *prof_name(ProfPart p)
{
    static const char *n[PROF_N] = {"CPU", "kernel", "audio", "GPU", "hardware", "pantalla"};
    return p < PROF_N ? n[p] : "?";
}

const char *prof_current(void)
{
    if (g_depth <= 0) {
        return "fuera";
    }
    return prof_name(g_stack[(g_depth > 16 ? 16 : g_depth) - 1]);
}

void prof_reset(int depth)
{
    if (depth >= 0 && depth < g_depth) {
        g_depth = depth;
    }
}

int prof_depth(void) { return g_depth; }

uint64_t prof_now_ns(void) { return now(); }
