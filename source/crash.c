/* XboxNX — cierres del propio emulador (ver crash.h). */
#include "crash.h"
#include "clocks.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "prof.h"

CrashInfo g_crash;

#ifdef __SWITCH__
#include <switch.h>

/* libnx corre el manejador sobre esta pila (y no sobre la del hilo que falló). */
__attribute__((aligned(16))) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

#define MAX_GUARDS 8
static struct {
    Handle thread;
    jmp_buf *jb;
    bool quiet;
} g_guard[MAX_GUARDS];
static const uint32_t *g_eip;
static const char *volatile g_step; /* el paso de la salida, o NULL */
static char g_why[160];             /* abort / assert / diag: el motivo */

void crash_step(const char *step) { g_step = step; }

static volatile unsigned g_heap_used, g_heap_total; /* la última medida (para cierre.txt) */

void crash_heap(unsigned *used_mb, unsigned *total_mb)
{
    extern char *fake_heap_start, *fake_heap_end;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    struct mallinfo mi = mallinfo();
#pragma GCC diagnostic pop
    g_heap_used = (unsigned)((size_t)mi.uordblks >> 20);
    g_heap_total = (unsigned)((size_t)(fake_heap_end - fake_heap_start) >> 20);
    *used_mb = g_heap_used;
    *total_mb = g_heap_total;
}

void crash_mem(unsigned *used_mb, unsigned *total_mb)
{
    u64 used = 0, total = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    *used_mb = (unsigned)(used >> 20);
    *total_mb = (unsigned)(total >> 20);
}

/* La ranura del hilo h; con claim, toma una libre (sin carreras entre hilos). */
static int guard_slot(Handle h, bool claim)
{
    for (int i = 0; i < MAX_GUARDS; i++) {
        if (__atomic_load_n(&g_guard[i].thread, __ATOMIC_ACQUIRE) == h) {
            return i;
        }
    }
    for (int i = 0; claim && i < MAX_GUARDS; i++) {
        Handle z = 0;
        if (__atomic_compare_exchange_n(&g_guard[i].thread, &z, h, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return i;
        }
    }
    return -1;
}

static int guard_find(Handle h)
{
    int i = guard_slot(h, false);
    return i >= 0 && g_guard[i].jb ? i : -1;
}

void crash_guard_push(jmp_buf *jb, bool quiet)
{
    int i = guard_slot(threadGetCurHandle(), true);
    if (i < 0) {
        return;
    }
    g_guard[i].quiet = quiet;
    g_guard[i].jb = jb;
}

void crash_guard_pop(void)
{
    int i = guard_slot(threadGetCurHandle(), false);
    if (i >= 0) {
        g_guard[i].jb = NULL;
    }
}

void crash_set_eip(const uint32_t *eip) { g_eip = eip; }

/* ---- el informe, sin malloc ni stdio (el fallo pudo ser dentro de ellos) ---- */

static char g_rep[4096];
static size_t g_rn;

static void put(const char *s)
{
    while (*s && g_rn + 1 < sizeof(g_rep)) {
        g_rep[g_rn++] = *s++;
    }
    g_rep[g_rn] = 0;
}

static void hex(uint64_t v)
{
    char b[19];
    int n = 0;
    b[n++] = '0';
    b[n++] = 'x';
    bool any = false;
    for (int i = 15; i >= 0; i--) {
        unsigned d = (unsigned)(v >> (i * 4)) & 0xF;
        if (d || any || i == 0) {
            b[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
            any = true;
        }
    }
    b[n] = 0;
    put(b);
}

/* Una dirección del código: "+0x1234" si es de XboxNX, "JIT+0x..." si es
 * traducida; si no, la dirección tal cual. */
static void addr(uint64_t a)
{
    if (a >= g_crash.base && a < g_crash.base + g_crash.text_size) {
        put("+");
        hex(a - g_crash.base);
    } else if (g_crash.jit_size && a >= g_crash.jit_rx && a < g_crash.jit_rx + g_crash.jit_size) {
        put("JIT+");
        hex(a - g_crash.jit_rx);
    } else {
        hex(a);
    }
}

static const char *desc_name(uint32_t d)
{
    switch (d) {
    case 0x100: return "instrucción inválida o código no ejecutable";
    case 0x101: return "acceso a memoria inválido";
    case 0x102: return "pc desalineado";
    case 0x103: return "pila desalineada";
    case 0x104: return "trampa";
    case 0x106: return "error del sistema (SError)";
    case 0x301: return "llamada al sistema inválida";
    case 0xAB0: return "abort (la app se detuvo a propósito)";
    case 0xAB1: return "assert (una comprobación falló)";
    case 0xAB2: return "libnx: error fatal (diagAbort)";
    default: return "otra";
    }
}

/* Lee 8 bytes solo si la página está mapeada y se puede leer. */
static bool safe_rd64(uint64_t a, uint64_t *v, uint64_t *region_end)
{
    MemoryInfo mi;
    u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || !(mi.perm & Perm_R) || mi.type == MemType_Unmapped) {
        return false;
    }
    if (region_end) {
        *region_end = mi.addr + mi.size;
    }
    if (a + 8 > mi.addr + mi.size) {
        return false;
    }
    memcpy(v, (const void *)a, 8);
    return true;
}

static bool is_call_return(uint64_t a)
{
    if (a < g_crash.base + 4 || a >= g_crash.base + g_crash.text_size || (a & 3)) {
        return false;
    }
    uint32_t ins;
    memcpy(&ins, (const void *)(a - 4), 4);
    return (ins & 0xFC000000u) == 0x94000000u || (ins & 0xFFFFFC1Fu) == 0xD63F0000u; /* bl / blr */
}

static void fill(const ThreadExceptionDump *ctx)
{
    memset(&g_crash, 0, sizeof(g_crash));
    g_crash.valid = true;
    g_crash.desc = ctx->error_desc;
    g_crash.pc = ctx->pc.x;
    g_crash.lr = ctx->lr.x;
    g_crash.sp = ctx->sp.x;
    g_crash.far = ctx->far.x;
    for (int i = 0; i < 29; i++) {
        g_crash.gpr[i] = ctx->cpu_gprs[i].x;
    }
    MemoryInfo mi;
    u32 pi;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&fill))) {
        g_crash.base = mi.addr;
        g_crash.text_size = mi.size;
    }
    uintptr_t rx;
    size_t sz;
    cpu_jit_range(&rx, &sz);
    g_crash.jit_rx = rx;
    g_crash.jit_size = sz;
    g_crash.part = prof_current();
    uint64_t v;
    if (g_eip && safe_rd64((uint64_t)(uintptr_t)g_eip & ~7ull, &v, NULL)) {
        g_crash.guest_eip = *g_eip;
    }
    /* direcciones de vuelta en la pila (16 KB hacia arriba) */
    uint64_t end = 0;
    for (uint64_t a = g_crash.sp & ~7ull; a < (g_crash.sp & ~7ull) + 0x4000 && g_crash.ntrace < 10; a += 8) {
        if (end && a + 8 > end) {
            break;
        }
        if (!safe_rd64(a, &v, &end)) {
            break;
        }
        if (v != g_crash.lr && is_call_return(v)) {
            g_crash.trace[g_crash.ntrace++] = v;
        }
    }
}

extern const char *g_app_version;

static void report(void)
{
    g_rn = 0;
    g_rep[0] = 0;
    put("XboxNX ");
    put(g_app_version ? g_app_version : "?");
    put(": cierre del emulador\n");
    put("tipo ");
    hex(g_crash.desc);
    put(" (");
    put(desc_name(g_crash.desc));
    put(")\npc ");
    addr(g_crash.pc);
    put("\nlr ");
    addr(g_crash.lr);
    put("\ndirección ");
    hex(g_crash.far);
    put("\nparte ");
    put(g_crash.part ? g_crash.part : "?");
    put("\neip x86 ");
    hex(g_crash.guest_eip);
    if (g_why[0]) {
        put("\nmotivo ");
        put(g_why);
    }
    if (g_step) {
        put("\nal salir, en: ");
        put(g_step);
    }
    {
        unsigned u, t;
        crash_mem(&u, &t);
        char mb[48];
        snprintf(mb, sizeof(mb), "\nmemoria %u de %u MB · montón en uso %u de %u MB (última medida)", u, t,
                 g_heap_used, g_heap_total);
        put(mb);
    }
    put("\nsp ");
    hex(g_crash.sp);
    put("  base ");
    hex(g_crash.base);
    put("  JIT ");
    hex(g_crash.jit_rx);
    put("\npila:");
    for (int i = 0; i < g_crash.ntrace; i++) {
        put(" ");
        addr(g_crash.trace[i]);
    }
    put("\n");
    for (int i = 0; i < 29; i++) {
        put("x");
        char b[3] = {(char)('0' + i / 10), (char)('0' + i % 10), 0};
        put(b);
        put(" ");
        hex(g_crash.gpr[i]);
        put(i % 4 == 3 ? "\n" : "  ");
    }
    put("\n");

    /* al salir la SD ya puede estar desmontada: entonces se abre aparte */
    FsFileSystem own, *fs = fsdevGetDeviceFileSystem("sdmc");
    bool opened = false;
    if (!fs) {
        if (R_FAILED(fsOpenSdCardFileSystem(&own))) {
            return;
        }
        fs = &own;
        opened = true;
    }
    static const char path[] = "/switch/XboxNX/cierre.txt";
    fsFsCreateDirectory(fs, "/switch");
    fsFsCreateDirectory(fs, "/switch/XboxNX");
    fsFsDeleteFile(fs, path);
    if (R_SUCCEEDED(fsFsCreateFile(fs, path, (s64)g_rn, 0))) {
        FsFile f;
        if (R_SUCCEEDED(fsFsOpenFile(fs, path, FsOpenMode_Write, &f))) {
            fsFileWrite(&f, 0, g_rep, g_rn, FsWriteOption_Flush);
            fsFileClose(&f);
        }
    }
    fsFsCommit(fs);
    if (opened) {
        fsFsClose(&own);
    }
}

/* Los cierres que no son excepciones (abort, assert, diagAbort): antes daban
 * la pantalla de error de la Switch sin dejar cierre.txt. Se enlazan con
 * -Wl,--wrap (Makefile): toda llamada a abort() pasa primero por aquí. */
static void report_here(uint32_t desc, uint64_t pc)
{
    static volatile int busy;
    if (busy) {
        return;
    }
    busy = 1;
    ThreadExceptionDump ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.error_desc = desc;
    ctx.pc.x = pc;
    ctx.lr.x = pc;
    ctx.sp.x = (u64)(uintptr_t)__builtin_frame_address(0);
    fill(&ctx);
    report();
    clocks_restore();
}

void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void) __attribute__((noreturn));
void __wrap_abort(void)
{
    report_here(g_why[0] && !strncmp(g_why, "assert", 6) ? 0xAB1 : 0xAB0, (u64)(uintptr_t)__builtin_return_address(0));
    __real_abort();
}

void __wrap___assert_func(const char *file, int line, const char *func, const char *expr) __attribute__((noreturn));
void __wrap___assert_func(const char *file, int line, const char *func, const char *expr)
{
    const char *f = file ? strrchr(file, '/') : NULL;
    snprintf(g_why, sizeof(g_why), "assert %s:%d %s: %s", f ? f + 1 : file ? file : "?", line, func ? func : "?",
             expr ? expr : "?");
    __wrap_abort();
}

void __real_diagAbortWithResult(Result res) __attribute__((noreturn));
void __wrap_diagAbortWithResult(Result res) __attribute__((noreturn));
void __wrap_diagAbortWithResult(Result res)
{
    snprintf(g_why, sizeof(g_why), "diagAbort resultado 0x%X (módulo %u, código %u)", (unsigned)res,
             (unsigned)R_MODULE(res), (unsigned)R_DESCRIPTION(res));
    report_here(0xAB2, (u64)(uintptr_t)__builtin_return_address(0));
    __real_diagAbortWithResult(res);
}

/*
 * libnx llama a esta función después de svcReturnFromException, ya en el
 * contexto normal del hilo (sobre __nx_exception_stack) y con el estado del
 * fallo en ctx. Si vuelve, libnx hace svcBreak (el informe de error de
 * siempre). Para seguir hay que saltar fuera: longjmp a la guardia del hilo.
 */
void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    int gi = guard_find(threadGetCurHandle());
    fill(ctx);
    g_crash.guarded = gi >= 0;
    if (gi < 0 || !g_guard[gi].quiet) {
        report();
    }
    if (gi < 0) {
        clocks_restore(); /* la app se cierra: los relojes como estaban */
    }
    if (gi >= 0) {
        jmp_buf *jb = g_guard[gi].jb;
        g_guard[gi].jb = NULL;
        longjmp(*jb, 1);
    }
}

bool crash_last(char *out, size_t n)
{
    if (!n) {
        return false;
    }
    out[0] = 0;
    FILE *f = fopen("sdmc:/switch/XboxNX/cierre.txt", "r");
    if (!f) {
        return false;
    }
    /* líneas 2 a 5: tipo, pc, lr, dirección, parte, eip */
    char line[160];
    size_t at = 0;
    int ln = 0;
    while (fgets(line, sizeof(line), f) && ln < 7) {
        ln++;
        if (ln == 1) {
            continue;
        }
        size_t l = strcspn(line, "\n");
        line[l] = 0;
        if (ln == 2) {
            char *p = strchr(line, ' ');
            if (p) {
                p = strchr(p + 1, ' ');
            }
            if (p) {
                *p = 0; /* solo "tipo 0x101" */
            }
        }
        int w = snprintf(out + at, n - at, "%s%s", at ? " · " : "", line);
        if (w < 0 || (size_t)w >= n - at) {
            break;
        }
        at += (size_t)w;
    }
    fclose(f);
    return at > 0;
}

#else /* PC: sin manejador */

void crash_guard_push(jmp_buf *jb, bool quiet) { (void)jb, (void)quiet; }
void crash_guard_pop(void) {}
void crash_set_eip(const uint32_t *eip) { (void)eip; }
void crash_step(const char *step) { (void)step; }
void crash_mem(unsigned *used_mb, unsigned *total_mb) { *used_mb = *total_mb = 0; }
void crash_heap(unsigned *used_mb, unsigned *total_mb) { *used_mb = *total_mb = 0; }
bool crash_last(char *out, size_t n)
{
    if (n) {
        out[0] = 0;
    }
    return false;
}

#endif

void crash_summary(char *out, size_t n)
{
    const CrashInfo *c = &g_crash;
    char pc[40], lr[40];
    const uint64_t a[2] = {c->pc, c->lr};
    char *o[2] = {pc, lr};
    for (int i = 0; i < 2; i++) {
        if (a[i] >= c->base && a[i] < c->base + c->text_size) {
            snprintf(o[i], 40, "+0x%llX", (unsigned long long)(a[i] - c->base));
        } else if (c->jit_size && a[i] >= c->jit_rx && a[i] < c->jit_rx + c->jit_size) {
            snprintf(o[i], 40, "JIT+0x%llX", (unsigned long long)(a[i] - c->jit_rx));
        } else {
            snprintf(o[i], 40, "0x%llX", (unsigned long long)a[i]);
        }
    }
    snprintf(out, n, "Fallo de XboxNX en %s: pc %s lr %s dir 0x%llX eip 0x%X (cierre.txt)", c->part ? c->part : "?", pc,
             lr, (unsigned long long)c->far, (unsigned)c->guest_eip);
}
