/*
 * XboxNX — pruebas de hardware (solo Switch).
 *
 * Lo que la etapa 2 necesita de Horizon:
 *  - JIT: memoria donde escribir código ARM64 y luego ejecutarlo (el
 *    traductor x86->ARM64, FEX).
 *  - Atrapar accesos inválidos (en Android, hakuX usa SIGSEGV para el MMIO
 *    y para saber qué memoria escribió el juego).
 *  - RAM: el Xbox tiene 64 MiB, pero el traductor, las texturas y el
 *    caché de código piden mucho más.
 *  - Una ventana de 4 GiB de direcciones para el espacio de 32 bits del
 *    Xbox.
 */
#include "probe.h"

#include "crash.h"

#include <malloc.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

/* ---- excepciones ---- */

static volatile uintptr_t g_bad_addr = 0x10;

__attribute__((noinline)) static u32 bad_read(void)
{
    volatile u32 *p = (volatile u32 *)g_bad_addr;
    return *p;
}

/* El manejador está en crash.c: con una guardia "quiet" vuelve aquí. */
bool probe_exception(uint32_t *desc, uint64_t *far)
{
    static jmp_buf jb;
    volatile bool hit = false;
    g_crash.valid = false;
    if (setjmp(jb) == 0) {
        crash_guard_push(&jb, true);
        (void)bad_read();
    } else {
        hit = true;
    }
    crash_guard_pop();
    *desc = g_crash.desc;
    *far = g_crash.far;
    g_crash.valid = false;
    return hit;
}

/* ---- JIT ---- */

static void probe_jit(ProbeResult *r)
{
    Jit j;
    Result rc = jitCreate(&j, 0x10000);
    r->jit_rc = rc;
    if (R_FAILED(rc)) {
        return;
    }
    u32 *rw = jitGetRwAddr(&j);
    rw[0] = 0x52800540; /* mov w0, #42 */
    rw[1] = 0xD65F03C0; /* ret */
    armDCacheFlush(rw, 8);
    rc = jitTransitionToExecutable(&j);
    if (R_SUCCEEDED(rc)) {
        void *rx = jitGetRxAddr(&j);
        armICacheInvalidate(rx, 8);
        int (*fn)(void) = (int (*)(void))rx;
        r->jit_value = fn();
        r->jit_ok = r->jit_value == 42;
    } else {
        r->jit_rc = rc;
    }
    jitClose(&j);
}

/* ---- memoria ---- */

static void probe_mem(ProbeResult *r)
{
    svcGetInfo(&r->mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&r->mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    /* El bloque más grande, de 16 MiB en 16 MiB hacia abajo. */
    for (u64 sz = 4096ull << 20; sz >= (16ull << 20); sz -= (16ull << 20)) {
        void *p = malloc(sz);
        if (p) {
            free(p);
            r->biggest_alloc = sz;
            break;
        }
    }
}

static void probe_va(ProbeResult *r)
{
    virtmemLock();
    void *addr = virtmemFindAslr(0x100000000ull, 0x10000);
    virtmemUnlock();
    r->va4g_ok = addr != NULL;
}

void probe_run(ProbeResult *r)
{
    memset(r, 0, sizeof(*r));
    AppletType at = appletGetAppletType();
    r->applet_mode = at != AppletType_Application && at != AppletType_SystemApplication;
    probe_mem(r);
    probe_jit(r);
    probe_va(r);
}
