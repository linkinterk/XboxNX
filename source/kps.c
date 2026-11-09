/*
 * XboxNX — kernel de Xbox, etapa 3b: hilos, esperas y tiempo.
 *
 * Planificador por prioridad con cuantos sobre el intérprete, esperas sobre
 * los DISPATCHER_HEADER de eventos, semáforos, mutex, hilos y temporizadores,
 * secciones críticas, IRQL, temporizadores con DPC y tiempo virtual.
 *
 * Semántica y distribución de estructuras según hakuX (native/kernel/ps.c,
 * ke.c y timer.c, GPL-3.0-or-later); implementación propia.
 */
#include <string.h>

#include "cpu.h"
#include "kernel.h"

/* KTHREAD / ETHREAD (como hakuX ps.c) */
#define ETHREAD_SIZE     0x140u
#define KT_TYPE          0x00
#define KT_SIZE          0x02
#define KT_SIGNAL        0x04
#define KT_WAIT_LIST     0x08
#define KT_MUTANT_LIST   0x10
#define KT_STACK_BASE    0x1C
#define KT_STACK_LIMIT   0x20
#define KT_KERNEL_STACK  0x24
#define KT_TLS_DATA      0x28
#define KT_NPX_STATE     0x30
#define KT_PRIORITY      0x32
#define KT_APC_LISTS     0x34
#define KT_QUANTUM       0x6C
#define KT_BASE_PRIORITY 0x70
#define ET_EXIT_STATUS   0x120
#define ET_UNIQUE_THREAD 0x12C
#define ET_START_ADDRESS 0x130
#define FX_SAVE_AREA     0x210u
#define DEFAULT_STACK    0x10000u
#define KERNEL_STACK_MIN 0x3000u
#define DEFAULT_PRIORITY 8

/* dispatcher */
#define DH_TYPE   0x00
#define DH_INSERTED 0x03
#define DH_SIGNAL 0x04
#define DH_WAIT   0x08
#define T_EVENT_NOTIFY 0
#define T_EVENT_SYNC   1
#define T_MUTANT       2
#define T_SEMAPHORE    5
#define T_THREAD       6
#define T_TIMER_NOTIFY 8
#define T_TIMER_SYNC   9
#define MUT_OWNER 0x18
#define SEM_LIMIT 0x10
#define CS_LOCK   0x10
#define CS_REC    0x14
#define CS_OWNER  0x18
/* KDPC */
#define DPC_INSERTED 0x02
#define DPC_ROUTINE  0x0C
#define DPC_CONTEXT  0x10
#define DPC_ARG1     0x14
#define DPC_ARG2     0x18
/* KTIMER */
#define TMR_DUE    0x10
#define TMR_DPC    0x20
#define TMR_PERIOD 0x24

#define PCR_EXCEPTION_LIST 0x00
#define PCR_STACK_BASE     0x04
#define PCR_STACK_LIMIT    0x08
#define PCR_IRQL           0x24
#define PCR_CURRENT_THREAD 0x28

#define WAIT_ALL 0
#define PERF_FREQUENCY 3375000ull
#define BOOT_FILETIME  0x01C0739BEB8F8000ull /* hora de la consola al arrancar */
#define NT_CURRENT_THREAD 0xFFFFFFFEu
#define QUANTUM_INSTR  2000000u

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
static uint8_t rd8(Kernel *k, uint32_t va) { return *vm_host(k->vm, va); }
static void wr8(Kernel *k, uint32_t va, uint8_t v) { *vm_host(k->vm, va) = v; }
static uint32_t rd(Kernel *k, uint32_t va) { return vm_rd32(k->vm, va); }
static void wr(Kernel *k, uint32_t va, uint32_t v) { vm_wr32(k->vm, va, v); }
static uint64_t rd64(Kernel *k, uint32_t va) { return (uint64_t)rd(k, va) | ((uint64_t)rd(k, va + 4) << 32); }

static KThread *cur(Kernel *k) { return k->cur >= 0 ? &k->threads[k->cur] : NULL; }
static uint32_t me(Kernel *k) { return k->cur >= 0 ? k->threads[k->cur].ethread : 0; }

static uint64_t system_time(Kernel *k) { return BOOT_FILETIME + k->now_ns / 100; }

void kps_update_tick(Kernel *k)
{
    uint32_t tc = kernel_data_va(k, "KeTickCount");
    if (tc) {
        wr(k, tc, (uint32_t)(k->now_ns / 1000000u));
    }
}

/* ------------------------------------------------------- hilos */

static void list_init(Kernel *k, uint32_t va)
{
    wr(k, va, va);
    wr(k, va + 4, va);
}

int kps_thread_create(Kernel *k, uint32_t stack_size, uint32_t tls_size, uint32_t ext_size)
{
    int slot = -1;
    for (int i = 0; i < K_MAX_THREADS; i++) {
        if (k->threads[i].state == KT_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    uint32_t et = kob_create(k, OB_THREAD, ETHREAD_SIZE + ext_size);
    if (!et) {
        return -1;
    }
    for (uint32_t i = 0; i < ETHREAD_SIZE + ext_size; i += 4) {
        wr(k, et + i, 0);
    }
    uint32_t tls = align_up(tls_size, 4);
    uint32_t size = align_up((stack_size ? stack_size : DEFAULT_STACK) + FX_SAVE_AREA + tls + 16, VM_PAGE);
    if (size < KERNEL_STACK_MIN) {
        size = KERNEL_STACK_MIN;
    }
    uint32_t base = kmm_stack_alloc(k, size);
    if (!base) {
        return -1;
    }
    uint32_t top = base + size, tib = top - FX_SAVE_AREA;
    /* área FX: control word 0x27F, MXCSR 0x1F80, NPX sin cargar */
    wr8(k, tib, 0x7F);
    wr8(k, tib + 1, 0x02);
    wr(k, tib + 0x18, 0x1F80);
    wr(k, tib + 0x200, 0x0A);
    wr8(k, et + KT_TYPE, T_THREAD);
    wr8(k, et + KT_SIZE, 0x110 / 4);
    wr8(k, et + KT_NPX_STATE, 0x0A);
    list_init(k, et + KT_WAIT_LIST);
    list_init(k, et + KT_MUTANT_LIST);
    list_init(k, et + KT_APC_LISTS);
    list_init(k, et + KT_APC_LISTS + 8);
    wr(k, et + KT_STACK_BASE, top);
    wr(k, et + KT_STACK_LIMIT, base);
    wr(k, et + KT_KERNEL_STACK, (tib - tls) & ~15u);
    wr(k, et + KT_TLS_DATA, tls ? tib - tls : 0);
    if (tls >= 4) {
        wr(k, tib - tls, tib - tls + 4); /* XAPI indexa el TLS desde aquí */
    }
    wr8(k, et + KT_PRIORITY, DEFAULT_PRIORITY);
    wr8(k, et + KT_BASE_PRIORITY, DEFAULT_PRIORITY);
    wr(k, et + KT_QUANTUM, 36);

    KThread *t = &k->threads[slot];
    memset(t, 0, sizeof(*t));
    t->state = KT_READY;
    t->id = ++k->next_id * 4;
    t->ethread = et;
    t->stack_base = top;
    t->stack_limit = base;
    t->tib = tib;
    t->exception_list = 0xFFFFFFFFu;
    t->priority = DEFAULT_PRIORITY;
    t->ctx.flags = 0x202;
    t->ctx.fpu_cw = 0x027F;
    t->ctx.mxcsr = 0x1F80;
    t->ctx.r[R_ESP] = ((tib - tls) & ~15u) - 16;
    wr(k, et + ET_UNIQUE_THREAD, t->id);
    return slot;
}

void kps_thread_start(Kernel *k, int ti, uint32_t eip, const uint32_t *v, unsigned n)
{
    KThread *t = &k->threads[ti];
    uint32_t esp = t->ctx.r[R_ESP] - 4 * n;
    for (unsigned i = 0; i < n; i++) {
        wr(k, esp + 4 * i, v[i]);
    }
    t->ctx.r[R_ESP] = esp;
    t->ctx.eip = eip;
    t->start = eip;
    wr(k, t->ethread + ET_START_ADDRESS, eip);
}

void kps_save(Kernel *k, Cpu *c)
{
    KThread *t = cur(k);
    if (!t || k->in_dpc) {
        return;
    }
    memcpy(t->ctx.r, c->r, sizeof(t->ctx.r));
    t->ctx.eip = c->eip;
    t->ctx.flags = c->flags;
    t->ctx.fpu_cw = c->fpu_cw;
    t->ctx.fpu_sw = c->fpu_sw;
    t->ctx.mxcsr = c->mxcsr;
    t->exception_list = rd(k, K_KPCR_VA + PCR_EXCEPTION_LIST);
}

void kps_load(Kernel *k, Cpu *c)
{
    KThread *t = cur(k);
    if (!t) {
        return;
    }
    memcpy(c->r, t->ctx.r, sizeof(c->r));
    c->eip = t->ctx.eip;
    c->flags = t->ctx.flags;
    c->fpu_cw = t->ctx.fpu_cw;
    c->fpu_sw = t->ctx.fpu_sw;
    c->mxcsr = t->ctx.mxcsr;
    c->fs_base = K_KPCR_VA;
    wr(k, K_KPCR_VA + PCR_EXCEPTION_LIST, t->exception_list);
    wr(k, K_KPCR_VA + PCR_STACK_BASE, t->tib);
    wr(k, K_KPCR_VA + PCR_STACK_LIMIT, t->stack_limit);
    wr(k, K_KPCR_VA + PCR_CURRENT_THREAD, t->ethread);
    k->current_thread = t->ethread;
}

void kps_exit(Kernel *k, uint32_t status)
{
    KThread *t = cur(k);
    if (!t) {
        return;
    }
    t->state = KT_DEAD;
    t->exit_status = status;
    wr(k, t->ethread + ET_EXIT_STATUS, status);
    wr(k, t->ethread + KT_SIGNAL, 1); /* el objeto hilo queda señalado */
}

unsigned kps_alive(const Kernel *k)
{
    unsigned n = 0;
    for (int i = 0; i < K_MAX_THREADS; i++) {
        n += k->threads[i].state != KT_FREE && k->threads[i].state != KT_DEAD;
    }
    return n;
}

static int thread_of(Kernel *k, uint32_t ethread)
{
    for (int i = 0; i < K_MAX_THREADS; i++) {
        if (k->threads[i].state != KT_FREE && k->threads[i].ethread == ethread) {
            return i;
        }
    }
    return -1;
}

static int thread_of_handle(Kernel *k, uint32_t h)
{
    if (h == NT_CURRENT_THREAD) {
        return k->cur;
    }
    ObType t;
    uint32_t o = kob_lookup(k, h, &t);
    return o && t == OB_THREAD ? thread_of(k, o) : -1;
}

/* ------------------------------------------------------- esperas */

static bool signaled(Kernel *k, uint32_t o, uint32_t who)
{
    uint8_t type = rd8(k, o + DH_TYPE);
    int32_t s = (int32_t)rd(k, o + DH_SIGNAL);
    if (type == T_MUTANT) {
        return s > 0 || rd(k, o + MUT_OWNER) == who;
    }
    return s > 0;
}

static void satisfy(Kernel *k, uint32_t o, uint32_t who)
{
    uint8_t type = rd8(k, o + DH_TYPE);
    int32_t s = (int32_t)rd(k, o + DH_SIGNAL);
    switch (type) {
    case T_EVENT_SYNC:
    case T_TIMER_SYNC:
        wr(k, o + DH_SIGNAL, 0);
        break;
    case T_SEMAPHORE:
        wr(k, o + DH_SIGNAL, (uint32_t)(s - 1));
        break;
    case T_MUTANT:
        wr(k, o + DH_SIGNAL, (uint32_t)(s - 1));
        wr(k, o + MUT_OWNER, who);
        break;
    default:
        break;
    }
}

/* Intenta cumplir la espera: devuelve el estado o -1 si todavía no. */
static int64_t try_wait(Kernel *k, const uint32_t *objs, unsigned n, bool all, uint32_t who)
{
    if (all) {
        for (unsigned i = 0; i < n; i++) {
            if (!signaled(k, objs[i], who)) {
                return -1;
            }
        }
        for (unsigned i = 0; i < n; i++) {
            satisfy(k, objs[i], who);
        }
        return 0;
    }
    for (unsigned i = 0; i < n; i++) {
        if (signaled(k, objs[i], who)) {
            satisfy(k, objs[i], who);
            return (int64_t)i;
        }
    }
    return -1;
}

/* Plazo de un LARGE_INTEGER* de tiempo (100 ns; negativo relativo,
 * positivo absoluto). false si no hay plazo (NULL: infinito). */
static bool deadline_of(Kernel *k, uint32_t ptr, uint64_t *out)
{
    if (!ptr) {
        return false;
    }
    int64_t v = (int64_t)rd64(k, ptr);
    if (v < 0) {
        *out = k->now_ns + (uint64_t)(-v) * 100u;
    } else {
        uint64_t st = system_time(k);
        *out = k->now_ns + ((uint64_t)v > st ? ((uint64_t)v - st) * 100u : 0);
    }
    return true;
}

static uint64_t wait_begin(KCall *c, const uint32_t *objs, unsigned n, bool all, uint32_t timeout_ptr, uint32_t cs,
                           const char *what)
{
    Kernel *k = c->k;
    KThread *t = cur(k);
    if (n > K_MAX_WAIT) {
        return ST_INVALID_PARAMETER;
    }
    if (n) {
        int64_t st = try_wait(k, objs, n, all, me(k));
        if (st >= 0) {
            return (uint64_t)st;
        }
    }
    uint64_t dl = 0;
    bool has = deadline_of(k, timeout_ptr, &dl);
    if (has && dl <= k->now_ns) {
        if (!n && !cs) {
            c->resched = true; /* esperar 0: ceder */
            return ST_SUCCESS;
        }
        return ST_TIMEOUT;
    }
    if (!t || k->in_dpc) {
        return ST_TIMEOUT;
    }
    if (n) {
        memcpy(t->wait_obj, objs, n * sizeof(uint32_t));
    }
    t->nwait = n;
    t->wait_all = all;
    t->has_deadline = has;
    t->deadline_ns = dl;
    t->wait_cs = cs;
    t->wait_what = what;
    t->state = KT_WAITING;
    c->block = true;
    return 0;
}

/* Despierta a los que ya pueden seguir. Devuelve la prioridad más alta
 * despertada (o -1). */
static int wake(Kernel *k)
{
    int best = -1;
    for (int i = 0; i < K_MAX_THREADS; i++) {
        KThread *t = &k->threads[i];
        if (t->state != KT_WAITING) {
            continue;
        }
        bool done = false;
        uint32_t status = 0;
        if (t->wait_cs) {
            if (signaled(k, t->wait_cs, t->ethread)) {
                satisfy(k, t->wait_cs, t->ethread);
                wr(k, t->wait_cs + CS_OWNER, t->ethread);
                wr(k, t->wait_cs + CS_REC, 1);
                done = true;
            }
        } else if (t->nwait) {
            int64_t st = try_wait(k, t->wait_obj, t->nwait, t->wait_all, t->ethread);
            if (st >= 0) {
                done = true;
                status = (uint32_t)st;
            }
        }
        if (!done && t->has_deadline && t->deadline_ns <= k->now_ns) {
            done = true;
            status = t->nwait ? ST_TIMEOUT : ST_SUCCESS;
        }
        if (done) {
            t->ctx.r[R_EAX] = status;
            t->state = KT_READY;
            t->wait_what = NULL;
            if (t->priority > best) {
                best = t->priority;
            }
        }
    }
    return best;
}

void kps_signaled(KCall *c)
{
    int best = wake(c->k);
    KThread *t = cur(c->k);
    if (t && best > t->priority) {
        c->resched = true;
    }
}

/* ------------------------------------------------------- temporizadores y DPC */

static bool dpc_queue(Kernel *k, uint32_t dpc, uint32_t a1, uint32_t a2)
{
    if (rd8(k, dpc + DPC_INSERTED) || k->ndpc >= K_MAX_DPCS) {
        return false;
    }
    wr8(k, dpc + DPC_INSERTED, 1);
    wr(k, dpc + DPC_ARG1, a1);
    wr(k, dpc + DPC_ARG2, a2);
    k->dpc_queue[k->ndpc++] = dpc;
    return true;
}

static bool dpc_remove(Kernel *k, uint32_t dpc)
{
    for (unsigned i = 0; i < k->ndpc; i++) {
        if (k->dpc_queue[i] == dpc) {
            memmove(&k->dpc_queue[i], &k->dpc_queue[i + 1], (k->ndpc - i - 1) * sizeof(uint32_t));
            k->ndpc--;
            wr8(k, dpc + DPC_INSERTED, 0);
            return true;
        }
    }
    return false;
}

static int timer_find(Kernel *k, uint32_t timer)
{
    for (unsigned i = 0; i < k->ntimers; i++) {
        if (k->timers[i].timer == timer) {
            return (int)i;
        }
    }
    return -1;
}

static bool timer_cancel(Kernel *k, uint32_t timer)
{
    int i = timer_find(k, timer);
    if (i < 0) {
        return false;
    }
    k->timers[i] = k->timers[--k->ntimers];
    wr8(k, timer + DH_INSERTED, 0);
    return true;
}

static void timers_fire(Kernel *k)
{
    for (unsigned i = 0; i < k->ntimers;) {
        KTimerRec *t = &k->timers[i];
        if (t->due_ns > k->now_ns) {
            i++;
            continue;
        }
        wr(k, t->timer + DH_SIGNAL, 1);
        if (t->dpc) {
            uint64_t st = system_time(k);
            dpc_queue(k, t->dpc, (uint32_t)st, (uint32_t)(st >> 32));
        }
        if (t->period_ns) {
            t->due_ns += t->period_ns;
            if (t->due_ns <= k->now_ns) {
                t->due_ns = k->now_ns + t->period_ns;
            }
            i++;
        } else {
            wr8(k, t->timer + DH_INSERTED, 0);
            k->timers[i] = k->timers[--k->ntimers];
        }
    }
}

bool kps_next_deadline(const Kernel *k, uint64_t *ns)
{
    bool any = false;
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < K_MAX_THREADS; i++) {
        const KThread *t = &k->threads[i];
        if (t->state == KT_WAITING && t->has_deadline && t->deadline_ns < best) {
            best = t->deadline_ns;
            any = true;
        }
    }
    for (unsigned i = 0; i < k->ntimers; i++) {
        if (k->timers[i].due_ns < best) {
            best = k->timers[i].due_ns;
            any = true;
        }
    }
    *ns = best;
    return any;
}

static void dpc_start(Kernel *k, Cpu *c)
{
    uint32_t dpc = k->dpc_queue[0];
    memmove(&k->dpc_queue[0], &k->dpc_queue[1], (k->ndpc - 1) * sizeof(uint32_t));
    k->ndpc--;
    wr8(k, dpc + DPC_INSERTED, 0);
    if (!k->dpc_stack_top) {
        uint32_t base = kmm_stack_alloc(k, 0x8000);
        k->dpc_stack_top = base ? base + 0x8000 : 0;
    }
    uint32_t esp = k->dpc_stack_top - 16 - 20;
    uint32_t v[5] = {K_STUB_DPC_RETURN, dpc, rd(k, dpc + DPC_CONTEXT), rd(k, dpc + DPC_ARG1), rd(k, dpc + DPC_ARG2)};
    for (int i = 0; i < 5; i++) {
        wr(k, esp + 4 * (uint32_t)i, v[i]);
    }
    memset(c->r, 0, sizeof(c->r));
    c->r[R_ESP] = esp;
    c->eip = rd(k, dpc + DPC_ROUTINE);
    c->flags = 0x202;
    wr8(k, K_KPCR_VA + PCR_IRQL, 2); /* DISPATCH_LEVEL */
    k->in_dpc = true;
    k->dpcs_run++;
}

void kps_dpc_return(Kernel *k, Cpu *c)
{
    (void)c;
    k->in_dpc = false;
    wr8(k, K_KPCR_VA + PCR_IRQL, 0);
}

/* ------------------------------------------------------- planificador */

bool kps_schedule(Kernel *k, Cpu *c, bool preempt)
{
    if (k->in_dpc) {
        return true;
    }
    KThread *t = cur(k);
    int prev = k->cur;
    if (t && (t->state == KT_RUNNING || t->state == KT_WAITING)) {
        kps_save(k, c);
        if (t->state == KT_RUNNING) {
            t->state = KT_READY;
        }
    }
    for (int guard = 0; guard < 100000; guard++) {
        timers_fire(k);
        wake(k);
        kps_update_tick(k);
        if (k->ndpc) {
            dpc_start(k, c);
            return true;
        }
        /* el de más prioridad; entre iguales, el siguiente al actual. Cada 8
         * cuantos, el siguiente sin mirar prioridad (que nadie se quede sin CPU). */
        bool rr = preempt && (++k->quantum_count % 8) == 0;
        int pick = -1;
        for (int n = 1; n <= K_MAX_THREADS; n++) {
            int i = ((prev < 0 ? 0 : prev) + n) % K_MAX_THREADS;
            KThread *x = &k->threads[i];
            if (x->state != KT_READY || x->suspend) {
                continue;
            }
            if (pick < 0 || (!rr && x->priority > k->threads[pick].priority)) {
                pick = i;
                if (rr) {
                    break;
                }
            }
        }
        if (pick >= 0) {
            if (pick != prev) {
                k->switches++;
            }
            k->cur = pick;
            k->threads[pick].state = KT_RUNNING;
            kps_load(k, c);
            return true;
        }
        /* nadie listo: saltar al siguiente plazo */
        uint64_t next;
        if (!kps_next_deadline(k, &next)) {
            k->cur = -1;
            return false;
        }
        if (next > k->now_ns) {
            k->now_ns = next;
        }
    }
    return false;
}

/* ------------------------------------------------------- llamadas */

/* PsCreateSystemThreadEx(*ThreadHandle, ThreadExtensionSize, KernelStackSize,
 *   TlsDataSize, *ThreadId, StartRoutine, StartContext, CreateSuspended,
 *   DebuggerThread, SystemRoutine). Con SystemRoutine el hilo arranca en
 *   SystemRoutine(StartRoutine, StartContext); si no, en StartRoutine(StartContext). */
static uint64_t PsCreateSystemThreadEx(KCall *c)
{
    Kernel *k = c->k;
    int ti = kps_thread_create(k, c->a[2], c->a[3], c->a[1]);
    if (ti < 0) {
        return ST_INSUFFICIENT_RESOURCES;
    }
    if (c->a[9]) {
        uint32_t v[3] = {K_STUB_RETURN, c->a[5], c->a[6]};
        kps_thread_start(k, ti, c->a[9], v, 3);
    } else {
        uint32_t v[2] = {K_STUB_RETURN, c->a[6]};
        kps_thread_start(k, ti, c->a[5], v, 2);
    }
    KThread *t = &k->threads[ti];
    t->start = c->a[5];
    uint32_t h = kob_handle(k, t->ethread, OB_THREAD);
    if (c->a[0]) {
        wr(k, c->a[0], h);
    }
    if (c->a[4]) {
        wr(k, c->a[4], t->id);
    }
    t->suspend = (c->a[7] & 0xFF) ? 1 : 0;
    return ST_SUCCESS;
}

/* PsTerminateSystemThread(ExitStatus) */
static uint64_t PsTerminateSystemThread(KCall *c)
{
    kps_exit(c->k, c->a[0]);
    c->ended = true;
    return 0;
}

/* NtResumeThread(Handle, *PreviousSuspendCount) */
static uint64_t NtResumeThread(KCall *c)
{
    int ti = thread_of_handle(c->k, c->a[0]);
    if (ti < 0) {
        return ST_INVALID_HANDLE;
    }
    KThread *t = &c->k->threads[ti];
    if (c->a[1]) {
        wr(c->k, c->a[1], t->suspend);
    }
    if (t->suspend) {
        t->suspend--;
    }
    return ST_SUCCESS;
}

/* NtSuspendThread(Handle, *PreviousSuspendCount) */
static uint64_t NtSuspendThread(KCall *c)
{
    int ti = thread_of_handle(c->k, c->a[0]);
    if (ti < 0) {
        return ST_INVALID_HANDLE;
    }
    KThread *t = &c->k->threads[ti];
    if (c->a[1]) {
        wr(c->k, c->a[1], t->suspend);
    }
    t->suspend++;
    if (ti == c->k->cur) {
        c->resched = true;
    }
    return ST_SUCCESS;
}

/* KeSetBasePriorityThread(Thread, Increment) -> incremento anterior */
static uint64_t KeSetBasePriorityThread(KCall *c)
{
    int ti = thread_of(c->k, c->a[0]);
    if (ti < 0) {
        return 0;
    }
    KThread *t = &c->k->threads[ti];
    int old = t->priority - DEFAULT_PRIORITY;
    int inc = (int32_t)c->a[1];
    if (inc < -2) {
        inc = -2;
    }
    if (inc > 2) {
        inc = 2;
    }
    t->priority = DEFAULT_PRIORITY + inc;
    wr8(c->k, t->ethread + KT_PRIORITY, (uint8_t)t->priority);
    c->resched = true;
    return (uint32_t)old;
}

/* KeSetPriorityThread(Thread, Priority) -> prioridad anterior */
static uint64_t KeSetPriorityThread(KCall *c)
{
    int ti = thread_of(c->k, c->a[0]);
    if (ti < 0) {
        return 0;
    }
    KThread *t = &c->k->threads[ti];
    int old = t->priority;
    t->priority = (int32_t)c->a[1];
    c->resched = true;
    return (uint32_t)old;
}

static uint64_t KeQueryBasePriorityThread(KCall *c)
{
    int ti = thread_of(c->k, c->a[0]);
    return ti < 0 ? 0 : (uint32_t)(c->k->threads[ti].priority - DEFAULT_PRIORITY);
}

static uint64_t KeGetCurrentThread(KCall *c) { return me(c->k); }

static uint64_t Zero(KCall *c)
{
    (void)c;
    return 0;
}

/* ---- esperas ---- */

/* KeWaitForSingleObject(Object, WaitReason, WaitMode, Alertable, *Timeout) */
static uint64_t KeWaitForSingleObject(KCall *c)
{
    return wait_begin(c, &c->a[0], 1, false, c->a[4], 0, "KeWaitForSingleObject");
}

/* KeWaitForMultipleObjects(Count, Object[], WaitType, WaitReason, WaitMode, Alertable,
 *   *Timeout, WaitBlockArray) */
static uint64_t KeWaitForMultipleObjects(KCall *c)
{
    uint32_t n = c->a[0], objs[K_MAX_WAIT];
    if (!n || n > K_MAX_WAIT) {
        return ST_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < n; i++) {
        objs[i] = rd(c->k, c->a[1] + 4 * i);
    }
    return wait_begin(c, objs, n, c->a[2] == WAIT_ALL, c->a[6], 0, "KeWaitForMultipleObjects");
}

static bool handle_obj(Kernel *k, uint32_t h, uint32_t *o)
{
    if (h == NT_CURRENT_THREAD) {
        *o = me(k);
        return *o != 0;
    }
    ObType t;
    *o = kob_lookup(k, h, &t);
    if (*o && t == OB_FILE) {
        *o += K_FO_EVENT; /* esperar un archivo es esperar su E/S */
    }
    return *o != 0;
}

/* NtWaitForSingleObject(Handle, Alertable, *Timeout) */
static uint64_t NtWaitForSingleObject(KCall *c)
{
    uint32_t o;
    if (!handle_obj(c->k, c->a[0], &o)) {
        return ST_INVALID_HANDLE;
    }
    return wait_begin(c, &o, 1, false, c->a[2], 0, "NtWaitForSingleObject");
}

/* NtWaitForSingleObjectEx(Handle, WaitMode, Alertable, *Timeout) */
static uint64_t NtWaitForSingleObjectEx(KCall *c)
{
    uint32_t o;
    if (!handle_obj(c->k, c->a[0], &o)) {
        return ST_INVALID_HANDLE;
    }
    return wait_begin(c, &o, 1, false, c->a[3], 0, "NtWaitForSingleObjectEx");
}

/* NtWaitForMultipleObjectsEx(Count, Handles[], WaitType, WaitMode, Alertable, *Timeout) */
static uint64_t NtWaitForMultipleObjectsEx(KCall *c)
{
    uint32_t n = c->a[0], objs[K_MAX_WAIT];
    if (!n || n > K_MAX_WAIT) {
        return ST_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (!handle_obj(c->k, rd(c->k, c->a[1] + 4 * i), &objs[i])) {
            return ST_INVALID_HANDLE;
        }
    }
    return wait_begin(c, objs, n, c->a[2] == WAIT_ALL, c->a[5], 0, "NtWaitForMultipleObjectsEx");
}

/* NtSignalAndWaitForSingleObjectEx(SignalHandle, WaitHandle, WaitMode, Alertable, *Timeout) */
static uint64_t NtSignalAndWaitForSingleObjectEx(KCall *c)
{
    Kernel *k = c->k;
    uint32_t so, wo;
    if (!handle_obj(k, c->a[0], &so) || !handle_obj(k, c->a[1], &wo)) {
        return ST_INVALID_HANDLE;
    }
    uint8_t type = rd8(k, so + DH_TYPE);
    int32_t s = (int32_t)rd(k, so + DH_SIGNAL);
    if (type == T_SEMAPHORE) {
        wr(k, so + DH_SIGNAL, (uint32_t)(s + 1));
    } else if (type == T_MUTANT) {
        wr(k, so + DH_SIGNAL, (uint32_t)(s + 1));
        if (s + 1 > 0) {
            wr(k, so + MUT_OWNER, 0);
        }
    } else {
        wr(k, so + DH_SIGNAL, 1);
    }
    kps_signaled(c);
    return wait_begin(c, &wo, 1, false, c->a[4], 0, "NtSignalAndWaitForSingleObjectEx");
}

/* KeDelayExecutionThread(WaitMode, Alertable, *Interval) */
static uint64_t KeDelayExecutionThread(KCall *c)
{
    return wait_begin(c, NULL, 0, false, c->a[2], 0, "KeDelayExecutionThread");
}

/* NtYieldExecution() */
static uint64_t NtYieldExecution(KCall *c)
{
    c->resched = true;
    return ST_SUCCESS;
}

/* ---- objetos del despachador (Ke) ---- */

static void header(Kernel *k, uint32_t o, uint8_t type, uint8_t size, int32_t signal)
{
    wr8(k, o + DH_TYPE, type);
    wr8(k, o + 1, 0);
    wr8(k, o + 2, size);
    wr8(k, o + DH_INSERTED, 0);
    wr(k, o + DH_SIGNAL, (uint32_t)signal);
    list_init(k, o + DH_WAIT);
}

/* KeInitializeEvent(Event, Type, State) */
static uint64_t KeInitializeEvent(KCall *c)
{
    header(c->k, c->a[0], (uint8_t)(c->a[1] ? T_EVENT_SYNC : T_EVENT_NOTIFY), 4, (c->a[2] & 0xFF) ? 1 : 0);
    return 0;
}

/* KePulseEvent(Event, Increment, Wait) -> previo: despierta a los que esperan y queda en 0 */
static uint64_t KePulseEvent(KCall *c)
{
    int32_t prev = (int32_t)rd(c->k, c->a[0] + DH_SIGNAL);
    wr(c->k, c->a[0] + DH_SIGNAL, 1);
    kps_signaled(c);
    wr(c->k, c->a[0] + DH_SIGNAL, 0);
    return (uint32_t)prev;
}

/* KeInitializeSemaphore(Semaphore, Count, Limit) */
static uint64_t KeInitializeSemaphore(KCall *c)
{
    header(c->k, c->a[0], T_SEMAPHORE, 5, (int32_t)c->a[1]);
    wr(c->k, c->a[0] + SEM_LIMIT, c->a[2]);
    return 0;
}

/* KeReleaseSemaphore(Semaphore, Increment, Adjustment, Wait) -> cuenta previa */
static uint64_t KeReleaseSemaphore(KCall *c)
{
    int32_t prev = (int32_t)rd(c->k, c->a[0] + DH_SIGNAL);
    wr(c->k, c->a[0] + DH_SIGNAL, (uint32_t)(prev + (int32_t)c->a[2]));
    kps_signaled(c);
    return (uint32_t)prev;
}

/* KeInitializeMutant(Mutant, InitialOwner) */
static uint64_t KeInitializeMutant(KCall *c)
{
    bool owned = (c->a[1] & 0xFF) != 0;
    header(c->k, c->a[0], T_MUTANT, 8, owned ? 0 : 1);
    list_init(c->k, c->a[0] + 0x10);
    wr(c->k, c->a[0] + MUT_OWNER, owned ? me(c->k) : 0);
    wr8(c->k, c->a[0] + 0x1C, 0);
    return 0;
}

/* KeReleaseMutant(Mutant, Increment, Abandoned, Wait) -> estado previo */
static uint64_t KeReleaseMutant(KCall *c)
{
    int32_t prev = (int32_t)rd(c->k, c->a[0] + DH_SIGNAL);
    wr(c->k, c->a[0] + DH_SIGNAL, (uint32_t)(prev + 1));
    if (prev + 1 > 0) {
        wr(c->k, c->a[0] + MUT_OWNER, 0);
        kps_signaled(c);
    }
    return (uint32_t)prev;
}

/* ---- secciones críticas ---- */

/* RtlInitializeCriticalSection(CriticalSection) */
static uint64_t RtlInitializeCriticalSection(KCall *c)
{
    header(c->k, c->a[0], T_EVENT_SYNC, 4, 0);
    wr(c->k, c->a[0] + CS_LOCK, 0xFFFFFFFFu);
    wr(c->k, c->a[0] + CS_REC, 0);
    wr(c->k, c->a[0] + CS_OWNER, 0);
    return 0;
}

/* RtlEnterCriticalSection(CriticalSection) */
static uint64_t RtlEnterCriticalSection(KCall *c)
{
    Kernel *k = c->k;
    uint32_t cs = c->a[0], m = me(k);
    int32_t lock = (int32_t)rd(k, cs + CS_LOCK) + 1;
    wr(k, cs + CS_LOCK, (uint32_t)lock);
    if (lock == 0) {
        wr(k, cs + CS_OWNER, m);
        wr(k, cs + CS_REC, 1);
    } else if (rd(k, cs + CS_OWNER) == m) {
        wr(k, cs + CS_REC, rd(k, cs + CS_REC) + 1);
    } else {
        /* el evento del principio de la sección: quien la suelte lo señala */
        if (signaled(k, cs, m)) {
            satisfy(k, cs, m);
            wr(k, cs + CS_OWNER, m);
            wr(k, cs + CS_REC, 1);
        } else {
            wait_begin(c, NULL, 0, false, 0, cs, "RtlEnterCriticalSection");
        }
    }
    return 0;
}

/* RtlLeaveCriticalSection(CriticalSection) */
static uint64_t RtlLeaveCriticalSection(KCall *c)
{
    Kernel *k = c->k;
    uint32_t cs = c->a[0];
    uint32_t rec = rd(k, cs + CS_REC) - 1;
    wr(k, cs + CS_REC, rec);
    int32_t lock = (int32_t)rd(k, cs + CS_LOCK) - 1;
    wr(k, cs + CS_LOCK, (uint32_t)lock);
    if (rec) {
        return 0;
    }
    wr(k, cs + CS_OWNER, 0);
    if (lock >= 0) {
        wr(k, cs + DH_SIGNAL, 1); /* uno que espera la toma */
        kps_signaled(c);
    }
    return 0;
}

/* RtlTryEnterCriticalSection(CriticalSection) -> BOOLEAN */
static uint64_t RtlTryEnterCriticalSection(KCall *c)
{
    Kernel *k = c->k;
    uint32_t cs = c->a[0], m = me(k);
    if ((int32_t)rd(k, cs + CS_LOCK) == -1) {
        wr(k, cs + CS_LOCK, 0);
        wr(k, cs + CS_OWNER, m);
        wr(k, cs + CS_REC, 1);
        return 1;
    }
    if (rd(k, cs + CS_OWNER) == m) {
        wr(k, cs + CS_LOCK, rd(k, cs + CS_LOCK) + 1);
        wr(k, cs + CS_REC, rd(k, cs + CS_REC) + 1);
        return 1;
    }
    return 0;
}

/* ---- IRQL ---- */

static uint64_t KfRaiseIrql(KCall *c) /* fastcall: CL = NewIrql */
{
    uint8_t old = rd8(c->k, K_KPCR_VA + PCR_IRQL);
    wr8(c->k, K_KPCR_VA + PCR_IRQL, (uint8_t)c->a[0]);
    return old;
}

static uint64_t KfLowerIrql(KCall *c)
{
    wr8(c->k, K_KPCR_VA + PCR_IRQL, (uint8_t)c->a[0]);
    return 0;
}

static uint64_t KeRaiseIrqlToDpcLevel(KCall *c)
{
    uint8_t old = rd8(c->k, K_KPCR_VA + PCR_IRQL);
    wr8(c->k, K_KPCR_VA + PCR_IRQL, 2);
    return old;
}

static uint64_t KeRaiseIrqlToSynchLevel(KCall *c)
{
    uint8_t old = rd8(c->k, K_KPCR_VA + PCR_IRQL);
    wr8(c->k, K_KPCR_VA + PCR_IRQL, 0x1C);
    return old;
}

static uint64_t KeGetCurrentIrql(KCall *c) { return rd8(c->k, K_KPCR_VA + PCR_IRQL); }

/* ---- Interlocked (fastcall) ---- */

static uint64_t InterlockedIncrement(KCall *c)
{
    uint32_t v = rd(c->k, c->a[0]) + 1;
    wr(c->k, c->a[0], v);
    return v;
}
static uint64_t InterlockedDecrement(KCall *c)
{
    uint32_t v = rd(c->k, c->a[0]) - 1;
    wr(c->k, c->a[0], v);
    return v;
}
static uint64_t InterlockedExchange(KCall *c)
{
    uint32_t old = rd(c->k, c->a[0]);
    wr(c->k, c->a[0], c->a[1]);
    return old;
}
static uint64_t InterlockedExchangeAdd(KCall *c)
{
    uint32_t old = rd(c->k, c->a[0]);
    wr(c->k, c->a[0], old + c->a[1]);
    return old;
}
/* InterlockedCompareExchange(Destination, Exchange, Comparand) */
static uint64_t InterlockedCompareExchange(KCall *c)
{
    uint32_t old = rd(c->k, c->a[0]);
    if (old == c->a[2]) {
        wr(c->k, c->a[0], c->a[1]);
    }
    return old;
}

/* ---- temporizadores y DPC ---- */

static uint64_t set_timer(KCall *c, uint32_t timer, uint64_t due, uint32_t period_ms, uint32_t dpc)
{
    Kernel *k = c->k;
    bool was = timer_cancel(k, timer);
    if (k->ntimers >= K_MAX_TIMERS) {
        return was;
    }
    int64_t v = (int64_t)due;
    uint64_t at;
    if (v < 0) {
        at = k->now_ns + (uint64_t)(-v) * 100u;
    } else {
        uint64_t st = system_time(k);
        at = k->now_ns + ((uint64_t)v > st ? ((uint64_t)v - st) * 100u : 0);
    }
    k->timers[k->ntimers++] = (KTimerRec){timer, dpc, at, (uint64_t)period_ms * 1000000u};
    wr(k, timer + DH_SIGNAL, 0);
    wr8(k, timer + DH_INSERTED, 1);
    wr(k, timer + TMR_DUE, (uint32_t)due);
    wr(k, timer + TMR_DUE + 4, (uint32_t)(due >> 32));
    wr(k, timer + TMR_DPC, dpc);
    wr(k, timer + TMR_PERIOD, period_ms);
    return was;
}

/* KeSetTimer(Timer, DueTime (por valor), Dpc) */
static uint64_t KeSetTimer(KCall *c)
{
    return set_timer(c, c->a[0], (uint64_t)c->a[1] | ((uint64_t)c->a[2] << 32), 0, c->a[3]);
}

/* KeSetTimerEx(Timer, DueTime (por valor), Period (ms), Dpc) */
static uint64_t KeSetTimerEx(KCall *c)
{
    return set_timer(c, c->a[0], (uint64_t)c->a[1] | ((uint64_t)c->a[2] << 32), c->a[3], c->a[4]);
}

/* KeCancelTimer(Timer) -> estaba puesto */
static uint64_t KeCancelTimer(KCall *c) { return timer_cancel(c->k, c->a[0]); }

/* KeInsertQueueDpc(Dpc, SystemArgument1, SystemArgument2) -> BOOLEAN */
static uint64_t KeInsertQueueDpc(KCall *c) { return dpc_queue(c->k, c->a[0], c->a[1], c->a[2]); }

/* KeRemoveQueueDpc(Dpc) -> BOOLEAN */
static uint64_t KeRemoveQueueDpc(KCall *c) { return dpc_remove(c->k, c->a[0]); }

static uint64_t KeIsExecutingDpc(KCall *c) { return c->k->in_dpc; }

/* ---- tiempo ---- */

/* KeQuerySystemTime(*CurrentTime) */
static uint64_t KeQuerySystemTime(KCall *c)
{
    uint64_t t = system_time(c->k);
    wr(c->k, c->a[0], (uint32_t)t);
    wr(c->k, c->a[0] + 4, (uint32_t)(t >> 32));
    return 0;
}

/* KeQueryInterruptTime() -> 100 ns desde el arranque */
static uint64_t KeQueryInterruptTime(KCall *c) { return c->k->now_ns / 100; }

/* KeQueryPerformanceCounter() */
static uint64_t KeQueryPerformanceCounter(KCall *c)
{
    return (uint64_t)((unsigned __int128)c->k->now_ns * PERF_FREQUENCY / 1000000000u);
}

static uint64_t KeQueryPerformanceFrequency(KCall *c)
{
    (void)c;
    return PERF_FREQUENCY;
}

/* KeTickCount es un dato; NtQueryTimerResolution etc. no hacen falta aún. */

void kps_register(Kernel *k)
{
    kernel_impl(k, "PsCreateSystemThreadEx", PsCreateSystemThreadEx);
    kernel_impl(k, "PsTerminateSystemThread", PsTerminateSystemThread);
    kernel_impl(k, "NtResumeThread", NtResumeThread);
    kernel_impl(k, "NtSuspendThread", NtSuspendThread);
    kernel_impl(k, "KeSetBasePriorityThread", KeSetBasePriorityThread);
    kernel_impl(k, "KeSetPriorityThread", KeSetPriorityThread);
    kernel_impl(k, "KeQueryBasePriorityThread", KeQueryBasePriorityThread);
    kernel_impl(k, "KeGetCurrentThread", KeGetCurrentThread);
    kernel_impl(k, "KeSetDisableBoostThread", Zero);
    kernel_impl(k, "KeBoostPriorityThread", Zero);
    kernel_impl(k, "KeEnterCriticalRegion", Zero);
    kernel_impl(k, "KeLeaveCriticalRegion", Zero);
    kernel_impl(k, "KeSaveFloatingPointState", Zero);
    kernel_impl(k, "KeRestoreFloatingPointState", Zero);
    kernel_impl(k, "KeStallExecutionProcessor", Zero);
    kernel_impl(k, "KeWaitForSingleObject", KeWaitForSingleObject);
    kernel_impl(k, "KeWaitForMultipleObjects", KeWaitForMultipleObjects);
    kernel_impl(k, "NtWaitForSingleObject", NtWaitForSingleObject);
    kernel_impl(k, "NtWaitForSingleObjectEx", NtWaitForSingleObjectEx);
    kernel_impl(k, "NtWaitForMultipleObjectsEx", NtWaitForMultipleObjectsEx);
    kernel_impl(k, "NtSignalAndWaitForSingleObjectEx", NtSignalAndWaitForSingleObjectEx);
    kernel_impl(k, "KeDelayExecutionThread", KeDelayExecutionThread);
    kernel_impl(k, "NtYieldExecution", NtYieldExecution);
    kernel_impl(k, "KeInitializeEvent", KeInitializeEvent);
    kernel_impl(k, "KePulseEvent", KePulseEvent);
    kernel_impl(k, "KeInitializeSemaphore", KeInitializeSemaphore);
    kernel_impl(k, "KeReleaseSemaphore", KeReleaseSemaphore);
    kernel_impl(k, "KeInitializeMutant", KeInitializeMutant);
    kernel_impl(k, "KeReleaseMutant", KeReleaseMutant);
    kernel_impl(k, "RtlInitializeCriticalSection", RtlInitializeCriticalSection);
    kernel_impl(k, "RtlEnterCriticalSection", RtlEnterCriticalSection);
    kernel_impl(k, "RtlLeaveCriticalSection", RtlLeaveCriticalSection);
    kernel_impl(k, "RtlTryEnterCriticalSection", RtlTryEnterCriticalSection);
    kernel_impl(k, "KfRaiseIrql", KfRaiseIrql);
    kernel_impl(k, "KfLowerIrql", KfLowerIrql);
    kernel_impl(k, "KeRaiseIrqlToDpcLevel", KeRaiseIrqlToDpcLevel);
    kernel_impl(k, "KeRaiseIrqlToSynchLevel", KeRaiseIrqlToSynchLevel);
    kernel_impl(k, "KeGetCurrentIrql", KeGetCurrentIrql);
    kernel_impl(k, "InterlockedIncrement", InterlockedIncrement);
    kernel_impl(k, "InterlockedDecrement", InterlockedDecrement);
    kernel_impl(k, "InterlockedExchange", InterlockedExchange);
    kernel_impl(k, "InterlockedExchangeAdd", InterlockedExchangeAdd);
    kernel_impl(k, "InterlockedCompareExchange", InterlockedCompareExchange);
    kernel_impl(k, "KeSetTimer", KeSetTimer);
    kernel_impl(k, "KeSetTimerEx", KeSetTimerEx);
    kernel_impl(k, "KeCancelTimer", KeCancelTimer);
    kernel_impl(k, "KeInsertQueueDpc", KeInsertQueueDpc);
    kernel_impl(k, "KeRemoveQueueDpc", KeRemoveQueueDpc);
    kernel_impl(k, "KeIsExecutingDpc", KeIsExecutingDpc);
    kernel_impl(k, "KeQuerySystemTime", KeQuerySystemTime);
    kernel_impl(k, "KeQueryInterruptTime", KeQueryInterruptTime);
    kernel_impl(k, "KeQueryPerformanceCounter", KeQueryPerformanceCounter);
    kernel_impl(k, "KeQueryPerformanceFrequency", KeQueryPerformanceFrequency);
}
