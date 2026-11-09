/*
 * XboxNX — arranque del juego en el intérprete.
 */
#include "runner.h"

#include <stdio.h>
#include <string.h>

/* Pila del hilo (KiInitializeContextThread; distribución de hakuX ps.c). */
#define STACK_VA         0x7F000000u
#define MAIN_STACK_SIZE  (256u * 1024u)
#define FX_SAVE_AREA     0x210u
#define ETHREAD_SIZE     0x140u
#define KT_TYPE          0x00
#define KT_SIZE          0x02
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
#define ET_START_ADDRESS 0x130

static void list_init(Vm *vm, uint32_t va)
{
    vm_wr32(vm, va, va);
    vm_wr32(vm, va + 4, va);
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
    size = (size + FX_SAVE_AREA + 16 + VM_PAGE - 1) & ~(VM_PAGE - 1);
    if (!vm_alloc_at(vm, STACK_VA - size, size)) {
        snprintf(r->end, sizeof(r->end), "Sin memoria para la pila del hilo");
        r->state = RUN_SYSTEM_STOP;
        return false;
    }
    uint32_t base = STACK_VA - size, top = STACK_VA, tib = top - FX_SAVE_AREA;
    r->stack_base = top;
    r->stack_limit = base;

    /* área FX: control word 0x27F, MXCSR 0x1F80, NPX sin cargar */
    vm_write(vm, tib, (uint8_t[2]){0x7F, 0x02}, 2);
    vm_wr32(vm, tib + 0x18, 0x1F80);
    vm_wr32(vm, tib + 0x200, 0x0A);

    /* ETHREAD (KTHREAD al principio) */
    uint32_t et = K_ETHREAD_VA;
    uint8_t *kt = vm_host(vm, et);
    memset(kt, 0, ETHREAD_SIZE);
    kt[KT_TYPE] = 6; /* THREAD_OBJECT */
    kt[KT_SIZE] = 0x110 / 4;
    kt[KT_NPX_STATE] = 0x0A;
    list_init(vm, et + KT_WAIT_LIST);
    list_init(vm, et + KT_MUTANT_LIST);
    list_init(vm, et + KT_APC_LISTS);
    list_init(vm, et + KT_APC_LISTS + 8);
    vm_wr32(vm, et + KT_STACK_BASE, top);
    vm_wr32(vm, et + KT_STACK_LIMIT, base);
    vm_wr32(vm, et + KT_KERNEL_STACK, tib & ~15u);
    vm_wr32(vm, et + KT_TLS_DATA, 0);
    kt[KT_PRIORITY] = 8;
    kt[KT_BASE_PRIORITY] = 8;
    vm_wr32(vm, et + KT_QUANTUM, 36);
    vm_wr32(vm, et + ET_START_ADDRESS, load->entry);

    /* KPCR en fs: NT_TIB, SelfPcr, Prcb, Irql, CurrentThread */
    uint32_t pcr = K_KPCR_VA;
    vm_wr32(vm, pcr + 0x00, 0xFFFFFFFFu);
    vm_wr32(vm, pcr + 0x04, tib);
    vm_wr32(vm, pcr + 0x08, base);
    vm_wr32(vm, pcr + 0x1C, pcr);
    vm_wr32(vm, pcr + 0x20, pcr + 0x28);
    *vm_host(vm, pcr + 0x24) = 0; /* PASSIVE_LEVEL */
    vm_wr32(vm, pcr + 0x28, et);

    cpu_init(&r->cpu, vm);
    r->cpu.fs_base = pcr;
    uint32_t esp = (tib & ~15u) - 16 - 4;
    vm_wr32(vm, esp, K_STUB_RETURN);
    r->cpu.r[R_ESP] = esp;
    r->cpu.eip = load->entry;
    r->state = RUN_ACTIVE;
    return true;
}

static void finish(Runner *r, RunState st, const char *fmt, const char *a, uint32_t b)
{
    r->state = st;
    snprintf(r->end, sizeof(r->end), fmt, a, b);
}

/* Una llamada al kernel: se registra y se "contesta" con 0. */
static void kernel_call(Runner *r)
{
    Cpu *c = &r->cpu;
    unsigned ord = c->kcall_ordinal;
    uint32_t esp = c->r[R_ESP];
    if (ord == K_ORD_THREAD_RETURN) {
        finish(r, RUN_THREAD_END, "La rutina del hilo volvió (eax 0x%s%08X)", "", c->r[R_EAX]);
        return;
    }
    const KExport *e = kernel_export(ord);
    if (r->ncalls < RUN_MAX_CALLS) {
        KCallRec *rec = &r->calls[r->ncalls];
        rec->ordinal = ord;
        rec->name = e ? e->name : "?";
        rec->kind = e ? e->kind : KX_TODO;
        rec->caller = vm_rd32(c->vm, esp);
        rec->icount = c->icount;
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
        bool seen = false;
        for (unsigned i = 0; i < r->ncalls; i++) {
            seen = seen || r->calls[i].ordinal == ord;
        }
        r->distinct += seen ? 0 : 1;
        r->ncalls++;
    }
    if (!e) {
        finish(r, RUN_SYSTEM_STOP, "Llamada a un ordinal del kernel desconocido%s (%u)", "", ord);
        return;
    }
    if (ord == 258) { /* PsTerminateSystemThread */
        finish(r, RUN_THREAD_END, "El juego terminó este hilo (PsTerminateSystemThread%s, estado 0x%X)", "",
               vm_rd32(c->vm, esp + 4));
        return;
    }
    if (ord == 49) { /* HalReturnToFirmware */
        finish(r, RUN_SYSTEM_STOP, "El juego pidió volver al menú del Xbox (HalReturnToFirmware%s %u)", "",
               vm_rd32(c->vm, esp + 4));
        return;
    }
    if (ord == 95) { /* KeBugCheck */
        finish(r, RUN_SYSTEM_STOP, "Error fatal del juego (KeBugCheck%s 0x%X)", "", vm_rd32(c->vm, esp + 4));
        return;
    }
    if (r->ncalls >= RUN_MAX_CALLS) {
        finish(r, RUN_CALL_LIMIT, "Se registraron %s%u llamadas: alto", "", RUN_MAX_CALLS);
        return;
    }
    /* volver al que llamó con éxito */
    c->r[R_EAX] = 0;
    c->eip = vm_rd32(c->vm, esp);
    c->r[R_ESP] = esp + 4 + kernel_pop_bytes(e);
    c->stop = CPU_RUNNING;
}

RunState runner_slice(Runner *r, uint64_t max)
{
    while (r->state == RUN_ACTIVE && max) {
        uint64_t before = r->cpu.icount;
        CpuStop st = cpu_run(&r->cpu, max);
        uint64_t done = r->cpu.icount - before;
        max = done >= max ? 0 : max - done - 1;
        if (st == CPU_LIMIT) {
            break;
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
