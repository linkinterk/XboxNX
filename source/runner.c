/*
 * XboxNX — ejecución del juego en el intérprete, con varios hilos (etapa 3b).
 */
#include "runner.h"

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
        return; /* a DISPATCH_LEVEL o más no se cambia de hilo */
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
    if (kc.ended) { /* el hilo terminó: no vuelve */
        reschedule(r, false);
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
        CpuStop st = cpu_run(&r->cpu, n);
        uint64_t done = r->cpu.icount - before;
        max = done + 1 >= max ? 0 : max - done - 1; /* al menos 1 por vuelta */
        r->quantum_left = done >= r->quantum_left ? 0 : r->quantum_left - done;
        k->now_ns += done * K_NS_PER_INSTR;
        if (k->cur >= 0 && !k->in_dpc) {
            k->threads[k->cur].instructions += done;
        }
        if (st == CPU_LIMIT) {
            if (!r->quantum_left) {
                reschedule(r, true);
            }
            continue;
        }
        if (st == CPU_KCALL) {
            kernel_call(r);
            continue;
        }
        r->state = RUN_CPU_STOP;
        snprintf(r->end, sizeof(r->end), "%s", cpu_stop_text(st));
    }
    return r->state;
}
