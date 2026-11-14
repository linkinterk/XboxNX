/*
 * XboxNX — hilos de ayuda (ver workers.h). En la Switch, hilos de libnx
 * fijos en los núcleos 1 y 2 (el 0 es el del intérprete); en la PC, pthreads.
 */
#include "workers.h"

#include <stdbool.h>
#include <stddef.h>

#define MAX_HELPERS 2

#ifdef __SWITCH__
#include <setjmp.h>
#include <switch.h>

#include "crash.h"
typedef Mutex WMutex;
typedef CondVar WCond;
#define wm_init(m) mutexInit(m)
#define wm_lock(m) mutexLock(m)
#define wm_unlock(m) mutexUnlock(m)
#define wc_init(c) condvarInit(c)
#define wc_wait(c, m) condvarWait(c, m)
#define wc_all(c) condvarWakeAll(c)
static Thread g_thr[MAX_HELPERS];
#else
#include <pthread.h>
typedef pthread_mutex_t WMutex;
typedef pthread_cond_t WCond;
#define wm_init(m) pthread_mutex_init(m, NULL)
#define wm_lock(m) pthread_mutex_lock(m)
#define wm_unlock(m) pthread_mutex_unlock(m)
#define wc_init(c) pthread_cond_init(c, NULL)
#define wc_wait(c, m) pthread_cond_wait(c, m)
#define wc_all(c) pthread_cond_broadcast(c)
static pthread_t g_thr[MAX_HELPERS];
#endif

static WMutex g_m;
static WCond g_go, g_done;
static int g_helpers;
static unsigned g_gen;      /* cada trabajo nuevo */
static int g_pending;       /* ayudantes que no terminaron */
static WorkFn g_fn;
static void *g_arg;
static bool g_quit;         /* al salir de la app: los ayudantes terminan */

static void helper(void *p)
{
    int me = (int)(size_t)p;
    unsigned seen = 0;
    for (;;) {
        wm_lock(&g_m);
        while (g_gen == seen && !g_quit) {
            wc_wait(&g_go, &g_m);
        }
        if (g_quit) {
            wm_unlock(&g_m);
            return;
        }
        seen = g_gen;
        WorkFn fn = g_fn;
        void *arg = g_arg;
        int n = g_helpers + 1;
        wm_unlock(&g_m);
#ifdef __SWITCH__
        /* si el trabajo falla (un acceso inválido), se abandona y el hilo
         * sigue: el principal ve g_crash y para el juego */
        jmp_buf jb;
        if (setjmp(jb) == 0) {
            crash_guard_push(&jb, false);
            fn(arg, me, n);
        }
        crash_guard_pop();
#else
        fn(arg, me, n);
#endif
        wm_lock(&g_m);
        if (--g_pending == 0) {
            wc_all(&g_done);
        }
        wm_unlock(&g_m);
    }
}

#ifndef __SWITCH__
static void *helper_p(void *p)
{
    helper(p);
    return NULL;
}
#endif

void workers_init(void)
{
    if (g_helpers) {
        return;
    }
    wm_init(&g_m);
    wc_init(&g_go);
    wc_init(&g_done);
    for (int i = 0; i < MAX_HELPERS; i++) {
#ifdef __SWITCH__
        if (R_FAILED(threadCreate(&g_thr[i], helper, (void *)(size_t)(i + 1), NULL, 0x10000, 0x2C, 1 + i)) ||
            R_FAILED(threadStart(&g_thr[i]))) {
            break;
        }
#else
        if (pthread_create(&g_thr[i], NULL, helper_p, (void *)(size_t)(i + 1)) != 0) {
            break;
        }
#endif
        g_helpers++;
    }
}

int workers_count(void) { return g_helpers + 1; }

/* Al salir de la app: si un hilo nuestro sigue vivo, el cargador (hbl) no
 * puede soltar la memoria del programa y la Switch muestra un error
 * (2001-0106) al volver al hbmenu. */
void workers_exit(void)
{
    if (!g_helpers) {
        return;
    }
    wm_lock(&g_m);
    g_quit = true;
    wc_all(&g_go);
    wm_unlock(&g_m);
    for (int i = 0; i < g_helpers; i++) {
#ifdef __SWITCH__
        threadWaitForExit(&g_thr[i]);
        threadClose(&g_thr[i]);
#else
        pthread_join(g_thr[i], NULL);
#endif
    }
    g_helpers = 0;
    g_quit = false;
}

void workers_run(WorkFn fn, void *arg)
{
    if (!g_helpers) {
        fn(arg, 0, 1);
        return;
    }
    wm_lock(&g_m);
    g_fn = fn;
    g_arg = arg;
    g_pending = g_helpers;
    g_gen++;
    wc_all(&g_go);
    wm_unlock(&g_m);
    fn(arg, 0, g_helpers + 1);
    wm_lock(&g_m);
    while (g_pending) {
        wc_wait(&g_done, &g_m);
    }
    wm_unlock(&g_m);
}
