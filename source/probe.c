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

#include <malloc.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

/* ---- excepciones ---- */

__attribute__((aligned(16))) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static volatile int g_exc_armed;
static volatile u32 g_exc_desc;
static volatile u64 g_exc_far;
static volatile uintptr_t g_bad_addr = 0x10;
static jmp_buf g_exc_jb;

/*
 * libnx llama a esta función después de svcReturnFromException, ya en el
 * contexto normal del hilo (sobre __nx_exception_stack) y con el estado del
 * fallo en ctx. Si vuelve, libnx hace svcBreak (el informe de error de
 * siempre). Para seguir hay que saltar fuera: longjmp al punto de la prueba.
 */
void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    if (g_exc_armed) {
        g_exc_armed = 0;
        g_exc_desc = ctx->error_desc;
        g_exc_far = ctx->far.x;
        longjmp(g_exc_jb, 1);
    }
}

__attribute__((noinline)) static u32 bad_read(void)
{
    volatile u32 *p = (volatile u32 *)g_bad_addr;
    return *p;
}

bool probe_exception(uint32_t *desc, uint64_t *far)
{
    bool hit = false;
    if (setjmp(g_exc_jb) == 0) {
        g_exc_armed = 1;
        (void)bad_read();
        g_exc_armed = 0;   /* no falló: el acceso no se atrapó */
    } else {
        hit = true;
    }
    *desc = g_exc_desc;
    *far = g_exc_far;
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
