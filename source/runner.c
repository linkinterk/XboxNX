/*
 * XboxNX — ejecución del juego en el intérprete, con varios hilos (etapa 3b).
 */
#include "runner.h"
#include "prof.h"

#include "hw.h"

#include <stdio.h>
#include <string.h>

/* Pila del hilo de arranque */
#define MAIN_STACK_SIZE  (256u * 1024u)
#define QUANTUM          2000000u   /* instrucciones por turno (~2 ms virtuales) */

unsigned runner_log_len(const Runner *r)
{
    unsigned cap = RUN_LOG_HEAD + RUN_LOG_TAIL;
    return r->ncalls < cap ? r->ncalls : cap;
}

const KCallRec *runner_log_at(const Runner *r, unsigned j, unsigned *number)
{
    unsigned len = runner_log_len(r);
    if (j >= len) {
        return NULL;
    }
    if (j < RUN_LOG_HEAD) {
        *number = j;
        return &r->head[j];
    }
    unsigned n = r->ncalls - (len - j);
    *number = n;
    return &r->tail[(n - RUN_LOG_HEAD) % RUN_LOG_TAIL];
}

static KCallRec *log_new(Runner *r)
{
    unsigned n = r->ncalls++;
    KCallRec *rec = n < RUN_LOG_HEAD ? &r->head[n] : &r->tail[(n - RUN_LOG_HEAD) % RUN_LOG_TAIL];
    memset(rec, 0, sizeof(*rec));
    return rec;
}

bool runner_boot(Runner *r, Vm *vm, Kernel *k, const LoadReport *load)
{
    cpu_free(&r->cpu); /* los bloques de la ejecución anterior */
    memset(r, 0, sizeof(*r));
    r->k = k;
    r->entry = load->entry;
    uint32_t size = MAIN_STACK_SIZE;
    if (load->stack_commit > size) {
        size = (load->stack_commit + VM_PAGE - 1) & ~(VM_PAGE - 1);
    }
    int ti = kps_thread_create(k, size, 0, 0);
    if (ti < 0) {
        snprintf(r->end, sizeof(r->end), "Sin memoria para el hilo de arranque");
        r->state = RUN_SYSTEM_STOP;
        return false;
    }
    uint32_t v[1] = {K_STUB_RETURN};
    kps_thread_start(k, ti, load->entry, v, 1);
    KThread *t = &k->threads[ti];
    r->stack_base = t->stack_base;
    r->stack_limit = t->stack_limit;

    /* KPCR en fs: NT_TIB, SelfPcr, Prcb, Irql */
    uint32_t pcr = K_KPCR_VA;
    vm_wr32(vm, pcr + 0x00, 0xFFFFFFFFu);
    vm_wr32(vm, pcr + 0x1C, pcr);
    vm_wr32(vm, pcr + 0x20, pcr + 0x28);
    *vm_host(vm, pcr + 0x24) = 0; /* PASSIVE_LEVEL */

    cpu_init(&r->cpu, vm);
    r->cpu.use_blocks = true;
    r->cpu.jit_hot = 4; /* JIT a ARM64 (solo en aarch64): a la cuarta vez (lo que corre pocas veces no vale traducirlo) */
    r->cpu.yield = &r->k->irq_kick;
    r->cpu.mmio = hw_mmio;
    r->cpu.port_in = hw_port_in;
    r->cpu.port_out = hw_port_out;
    r->cpu.dev = k;
    r->cpu.clock_ns = &k->now_ns;
    k->cur = ti;
    t->state = KT_RUNNING;
    kps_load(k, &r->cpu);
    r->quantum_left = QUANTUM;
    r->state = RUN_ACTIVE;
    return true;
}

static void finish(Runner *r, RunState st, const char *fmt, const char *a, uint32_t b)
{
    r->state = st;
    snprintf(r->end, sizeof(r->end), fmt, a, b);
}

/* Elige el siguiente hilo; si no queda ninguno, termina la ejecución. */
static void reschedule(Runner *r, bool preempt)
{
    r->quantum_left = QUANTUM;
    if (preempt && *vm_host(r->cpu.vm, K_KPCR_VA + 0x24) >= 2) {
        /* a DISPATCH_LEVEL o más no se cambia de hilo; las interrupciones sí */
        if (kps_interrupt(r->k, &r->cpu)) {
            r->cpu.stop = CPU_RUNNING;
        }
        return;
    }
    if (kps_schedule(r->k, &r->cpu, preempt)) {
        r->cpu.stop = CPU_RUNNING;
        return;
    }
    unsigned alive = kps_alive(r->k);
    if (!alive) {
        finish(r, RUN_THREAD_END, "Todos los hilos del juego terminaron%s (%u)", "", 0);
    } else {
        finish(r, RUN_DEADLOCK, "Bloqueo: los %s%u hilos esperan algo que nunca llegará", "", alive);
    }
}

/* Lo que el juego dejó para el reinicio (LAUNCH_DATA_HEADER: tipo, título, ruta). */
static void read_launch(Runner *r)
{
    uint32_t slot = kernel_data_va(r->k, "LaunchDataPage");
    uint32_t page = slot ? vm_rd32(r->cpu.vm, slot) : 0;
    if (!page || !vm_host(r->cpu.vm, page)) {
        return;
    }
    r->has_launch = true;
    r->launch_type = vm_rd32(r->cpu.vm, page);
    r->launch_title = vm_rd32(r->cpu.vm, page + 4);
    unsigned i = 0;
    for (; i + 1 < sizeof(r->launch_path); i++) {
        uint8_t ch = *vm_host(r->cpu.vm, page + 8 + i);
        if (!ch) {
            break;
        }
        r->launch_path[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    r->launch_path[i] = 0;
}

/* ---- registro de E/S (el volcado lo escribe) ---- */

static RunIoRec g_io[RUN_IO_LOG];
static unsigned g_io_n;

const RunIoRec *runner_io_at(unsigned j)
{
    unsigned len = runner_io_len();
    if (j >= len) {
        return NULL;
    }
    unsigned first = g_io_n > RUN_IO_LOG ? g_io_n - RUN_IO_LOG : 0;
    return &g_io[(first + j) % RUN_IO_LOG];
}

unsigned runner_io_len(void) { return g_io_n < RUN_IO_LOG ? g_io_n : RUN_IO_LOG; }

static void io_log(Runner *r, unsigned ord, const KCall *kc, uint32_t ret, uint32_t caller, uint32_t thread)
{
    int iosb = -1, info = -1;
    switch (ord) {
    case 190: iosb = 3; break;                 /* NtCreateFile */
    case 202: iosb = 3; break;                 /* NtOpenFile */
    case 219: case 220: case 236: iosb = 4; info = 7; break; /* NtReadFile, NtReadFileScatter, NtWriteFile (info: ByteOffset) */
    case 211: iosb = 1; info = 2; break;       /* NtQueryInformationFile */
    case 226: iosb = 1; info = 2; break;       /* NtSetInformationFile */
    case 198: iosb = 1; break;                 /* NtFlushBuffersFile */
    case 218: iosb = 1; info = 2; break;       /* NtQueryVolumeInformationFile */
    case 207: iosb = 4; break;                 /* NtQueryDirectoryFile */
    case 196: case 200: iosb = 4; break;       /* NtDeviceIoControlFile, NtFsControlFile */
    case 187: case 210: break;                 /* NtClose, NtQueryFullAttributesFile */
    default: return;
    }
    RunIoRec *x = &g_io[g_io_n++ % RUN_IO_LOG];
    memset(x, 0, sizeof(*x));
    x->num = r->ncalls;
    x->ordinal = (uint16_t)ord;
    x->thread = (uint16_t)(thread / 4);
    x->caller = caller;
    memcpy(x->a, kc->a, sizeof(x->a));
    x->ret = ret;
    Vm *vm = r->cpu.vm;
    if (iosb >= 0 && kc->a[iosb] && vm_host(vm, kc->a[iosb])) {
        x->iosb[0] = vm_rd32(vm, kc->a[iosb]);
        x->iosb[1] = vm_rd32(vm, kc->a[iosb] + 4);
    }
    if (info >= 0 && kc->a[info] && vm_host(vm, kc->a[info])) {
        for (int i = 0; i < 4; i++) {
            x->buf[i] = vm_host(vm, kc->a[info] + 4 * (uint32_t)i) ? vm_rd32(vm, kc->a[info] + 4 * (uint32_t)i) : 0;
        }
    }
    size_t nl = strnlen(kc->note, sizeof(kc->note)), cap = sizeof(x->note) - 1;
    const char *from = nl > cap ? kc->note + nl - cap : kc->note; /* el final de la ruta */
    memcpy(x->note, from, nl > cap ? cap : nl);
}

static void kernel_call(Runner *r)
{
    Cpu *c = &r->cpu;
    Kernel *k = r->k;
    unsigned ord = c->kcall_ordinal;
    uint32_t esp = c->r[R_ESP];
    if (ord == K_ORD_THREAD_RETURN) { /* la rutina del hilo volvió */
        kps_exit(k, c->r[R_EAX]);
        reschedule(r, false);
        return;
    }
    if (ord == K_ORD_DPC_RETURN) {
        kps_dpc_return(k, c);
        reschedule(r, false);
        return;
    }
    if (ord == K_ORD_APC_RETURN) {
        kps_apc_return(k, c);
        c->stop = CPU_RUNNING;
        return;
    }
    if (ord == K_ORD_ISR_RETURN) {
        kps_isr_return(k);
        reschedule(r, false);
        return;
    }
    const KExport *e = kernel_export(ord);
    KCallRec *rec = log_new(r);
    rec->ordinal = ord;
    rec->name = e ? e->name : "?";
    rec->kind = e ? e->kind : KX_TODO;
    rec->caller = vm_rd32(c->vm, esp);
    rec->icount = c->icount;
    rec->thread = k->cur >= 0 ? k->threads[k->cur].id : 0;
    if (e && e->conv == KC_FAST) {
        rec->args[0] = c->r[R_ECX];
        rec->args[1] = c->r[R_EDX];
        rec->args[2] = vm_rd32(c->vm, esp + 4);
        rec->args[3] = vm_rd32(c->vm, esp + 8);
    } else {
        for (int i = 0; i < 4; i++) {
            rec->args[i] = vm_rd32(c->vm, esp + 4 + 4 * (uint32_t)i);
        }
    }
    if (ord < K_ORDINAL_MAX) {
        r->counts[ord]++;
    }
    if (ord < K_ORDINAL_MAX && !r->seen[ord]) {
        r->seen[ord] = 1;
        r->distinct++;
    }
    if (!e) {
        finish(r, RUN_SYSTEM_STOP, "Llamada a un ordinal del kernel desconocido%s (%u)", "", ord);
        return;
    }
    if (ord == 49) { /* HalReturnToFirmware */
        rec->real = true;
        read_launch(r);
        finish(r, RUN_SYSTEM_STOP, "El juego pidió volver al menú del Xbox (HalReturnToFirmware%s %u)", "",
               vm_rd32(c->vm, esp + 4));
        return;
    }
    if (ord == 95) { /* KeBugCheck */
        rec->real = true;
        finish(r, RUN_SYSTEM_STOP, "Error fatal del juego (KeBugCheck%s 0x%X)", "", vm_rd32(c->vm, esp + 4));
        return;
    }
    /* la implementación real, o un 0 simulado */
    uint64_t ret = 0;
    KFn fn = k->impl[ord];
    KCall kc = {.k = k, .vm = c->vm, .cpu = c};
    if (fn) {
        unsigned at = 0;
        if (e->conv == KC_FAST) {
            kc.a[at++] = c->r[R_ECX];
            kc.a[at++] = c->r[R_EDX];
        }
        for (unsigned i = 0; at < 12; i++) {
            kc.a[at++] = vm_rd32(c->vm, esp + 4 + 4 * i);
        }
        ret = fn(&kc);
    } else {
        r->simulated++;
    }
    rec->real = fn != NULL;
    rec->ret = (uint32_t)ret;
    memcpy(rec->note, kc.note, sizeof(rec->note));
    io_log(r, ord, &kc, (uint32_t)ret, rec->caller, rec->thread);
    if (kc.ended) { /* el hilo terminó: no vuelve */
        reschedule(r, false);
        return;
    }
    if (kc.apc) { /* espera alertable con APC pendientes: corren y vuelve STATUS_USER_APC */
        kps_apc_start(k, c, vm_rd32(c->vm, esp), esp + 4 + kernel_pop_bytes(e));
        c->stop = CPU_RUNNING;
        return;
    }
    if (kc.tail_eip && kc.tail_n == 3) { /* otra función de 3 argumentos con la misma vuelta */
        for (int i = 0; i < 3; i++) {
            vm_wr32(c->vm, esp + 4 + 4 * (uint32_t)i, kc.tail_args[i]);
        }
        c->eip = kc.tail_eip;
        c->stop = CPU_RUNNING;
        return;
    }
    if (kc.tail_eip) { /* saltar a tail_eip(tail_arg) con la vuelta del que llamó */
        uint32_t back = vm_rd32(c->vm, esp);
        uint32_t nesp = esp + 4 + kernel_pop_bytes(e) - 8;
        vm_wr32(c->vm, nesp, back);
        vm_wr32(c->vm, nesp + 4, kc.tail_arg);
        c->r[R_ESP] = nesp;
        c->eip = kc.tail_eip;
        c->stop = CPU_RUNNING;
        return;
    }
    /* volver al que llamó */
    c->r[R_EAX] = (uint32_t)ret;
    c->r[R_EDX] = (uint32_t)(ret >> 32);
    c->eip = vm_rd32(c->vm, esp);
    c->r[R_ESP] = esp + 4 + kernel_pop_bytes(e);
    c->stop = CPU_RUNNING;
    if (kc.block || kc.resched) {
        reschedule(r, false);
    }
}

RunState runner_slice(Runner *r, uint64_t max)
{
    Kernel *k = r->k;
    while (r->state == RUN_ACTIVE && max) {
        uint64_t n = max < r->quantum_left ? max : r->quantum_left;
        uint64_t before = r->cpu.icount;
        r->cpu.clock_icount = before; /* rdtsc: k->now_ns vale hasta aquí */
        r->cpu.ns_per_instr = k->ns_per_instr ? k->ns_per_instr : 1;
        prof_push(PROF_CPU);
        CpuStop st = cpu_run(&r->cpu, n);
        prof_pop();
        uint64_t done = r->cpu.icount - before;
        max = done + 1 >= max ? 0 : max - done - 1; /* al menos 1 por vuelta */
        r->quantum_left = done >= r->quantum_left ? 0 : r->quantum_left - done;
        k->now_ns += done * (k->ns_per_instr ? k->ns_per_instr : 1);
        if (k->cur >= 0 && !k->in_dpc) {
            k->threads[k->cur].instructions += done;
        }
        if (st == CPU_LIMIT) {
            if (k->irq_kick) {
                /* una interrupción del hardware (la trampa del GPU): su ISR ya,
                 * sin esperar al final del turno; luego sus DPC y el mismo hilo */
                k->irq_kick = 0;
                if (r->quantum_left && kps_interrupt(k, &r->cpu)) {
                    r->cpu.stop = CPU_RUNNING;
                }
            }
            if (r->cpu.idle) {
                /* un bucle que solo espera: en vez de dar vueltas millones de
                 * veces, el tiempo salta al siguiente evento (vblank, USB,
                 * temporizador) y se reparte el turno */
                r->cpu.idle = false;
                kps_idle_skip(k);
                r->quantum_left = 0;
                r->idle_skips++;
            }
            if (!r->quantum_left) {
                reschedule(r, true);
            }
            continue;
        }
        if (st == CPU_KCALL) {
            prof_push(PROF_KERNEL);
            unsigned ord = r->cpu.kcall_ordinal;
            uint64_t t0 = prof_now_ns();
            kernel_call(r);
            uint64_t dt = prof_now_ns() - t0;
            if (ord < K_ORDINAL_MAX) {
                r->kns[ord] += dt;
            }
            r->kns_total += dt;
            prof_pop();
            continue;
        }
        r->state = RUN_CPU_STOP;
        snprintf(r->end, sizeof(r->end), "%s", cpu_stop_text(st));
    }
    return r->state;
}
