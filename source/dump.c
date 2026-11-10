/*
 * XboxNX — volcado del estado (botón − en el registro de la ejecución).
 */
#include "dump.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel.h"

static bool write_file(const char *dir, const char *name, const void *p, size_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    remove(path); /* el de un volcado anterior (en la Switch, escribir encima cerraba el programa) */
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(p, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

static const char *state_name(KtState s)
{
    switch (s) {
    case KT_FREE: return "libre";
    case KT_READY: return "listo";
    case KT_RUNNING: return "corre";
    case KT_WAITING: return "espera";
    case KT_DEAD: return "terminado";
    default: return "?";
    }
}

static void text(FILE *f, const Runner *r)
{
    const Kernel *k = r->k;
    const Cpu *c = &r->cpu;
    Vm *vm = c->vm;
    fprintf(f, "XboxNX volcado\nfin: %s\nicount %llu  now_ns %llu  ns/instr %u\n", r->end,
            (unsigned long long)c->icount, (unsigned long long)k->now_ns, k->ns_per_instr);
    fprintf(f, "CPU eip %08X flags %08X fs %08X\n", c->eip, c->flags, c->fs_base);
    static const char *rn[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    for (int i = 0; i < 8; i++) {
        fprintf(f, "  %s %08X\n", rn[i], c->r[i]);
    }
    fprintf(f, "trace:");
    for (unsigned i = 0; i < CPU_TRACE; i++) {
        fprintf(f, " %08X", c->trace[(c->trace_pos + i) % CPU_TRACE]);
    }
    fprintf(f, "\nIRQL %u  in_isr %d in_dpc %d  ndpc %u  isr %u dpc %u  cur %d\n", *vm_host(vm, K_KPCR_VA + 0x24),
            k->in_isr, k->in_dpc, k->ndpc, k->isrs_run, k->dpcs_run, k->cur);
    fprintf(f, "archivos: %u lecturas %llu bytes, %u escrituras, %u abiertos; fallidas %u, última %s\n", k->reads,
            (unsigned long long)k->bytes_read, k->writes, k->files_open, k->open_fails, k->last_fail);
    fprintf(f, "APC: %u encolados %u corridos\n\nHILOS\n", k->apcs_queued, k->apcs_run);
    for (int i = 0; i < K_MAX_THREADS; i++) {
        const KThread *t = &k->threads[i];
        if (t->state == KT_FREE) {
            continue;
        }
        fprintf(f, "[%d] h%u %s%s prio %d susp %u irql %u ethread %08X start %08X pila %08X-%08X instr %llu exit %08X\n",
                i, t->id / 4, state_name(t->state), t->system ? " (sistema)" : "", t->priority, t->suspend,
                t->ctx.irql, t->ethread, t->start, t->stack_limit, t->stack_base,
                (unsigned long long)t->instructions, t->exit_status);
        const uint32_t *rr = t->state == KT_RUNNING ? c->r : t->ctx.r;
        uint32_t eip = t->state == KT_RUNNING ? c->eip : t->ctx.eip;
        fprintf(f, "   eip %08X", eip);
        for (int j = 0; j < 8; j++) {
            fprintf(f, " %s %08X", rn[j], rr[j]);
        }
        fprintf(f, "\n");
        if (t->state == KT_WAITING) {
            fprintf(f, "   espera (%s)%s%s plazo %llu:", t->wait_what ? t->wait_what : "?", t->wait_all ? " todos" : "",
                    t->has_deadline ? " con" : " sin", (unsigned long long)t->deadline_ns);
            for (unsigned j = 0; j < t->nwait; j++) {
                fprintf(f, " %08X", t->wait_obj[j]);
            }
            fprintf(f, "\n");
        }
        /* la pila: 64 palabras */
        uint32_t esp = rr[R_ESP];
        fprintf(f, "   pila %08X:", esp);
        for (int j = 0; j < 64; j++) {
            if (!(j % 8)) {
                fprintf(f, "\n    ");
            }
            uint32_t a = esp + 4 * (uint32_t)j;
            fprintf(f, " %08X", vm_host(vm, a) ? vm_rd32(vm, a) : 0xDEADDEADu);
        }
        fprintf(f, "\n");
    }
    {
        CpuProfile pf;
        cpu_profile((Cpu *)&r->cpu, &pf);
        fprintf(f, "\nPERFIL DEL CPU: %llu instrucciones en %llu bloques, %llu lentas (fuera del JIT)\n",
                (unsigned long long)pf.insns, (unsigned long long)pf.blocks, (unsigned long long)pf.slow);
        for (unsigned t = 0; t < pf.n; t++) {
            fprintf(f, pf.op[t] > 0xFF ? "  0F %02X  %llu\n" : "  %02X     %llu\n", pf.op[t] & 0xFF,
                    (unsigned long long)pf.count[t]);
        }
    }
    fprintf(f, "\nLLAMADAS AL KERNEL (cuenta)\n");
    for (unsigned o = 0; o < K_ORDINAL_MAX; o++) {
        if (r->counts[o]) {
            const KExport *x = kernel_export(o);
            fprintf(f, "  %4u %-36s %u  %.3f s\n", o, x ? x->name : "?", r->counts[o], r->kns[o] / 1e9);
        }
    }
    fprintf(f, "\nARCHIVOS ABIERTOS\n");
    kio_dump(r->k, f);
    fprintf(f, "\nE/S DE ARCHIVOS (últimas %u)\n", runner_io_len());
    for (unsigned j = 0; j < runner_io_len(); j++) {
        const RunIoRec *x = runner_io_at(j);
        const KExport *e = kernel_export(x->ordinal);
        fprintf(f, "%u h%u %s(%08X %08X %08X %08X %08X %08X %08X %08X) = %08X iosb %08X %08X info %08X %08X %08X %08X desde %08X %s\n",
                x->num, x->thread, e ? e->name : "?", x->a[0], x->a[1], x->a[2], x->a[3], x->a[4], x->a[5], x->a[6],
                x->a[7], x->ret, x->iosb[0], x->iosb[1], x->buf[0], x->buf[1], x->buf[2], x->buf[3], x->caller,
                x->note);
    }
    fprintf(f, "\nÚLTIMAS LLAMADAS\n");
    unsigned n = runner_log_len(r);
    for (unsigned j = n > 250 ? n - 250 : 0; j < n; j++) {
        unsigned num;
        const KCallRec *x = runner_log_at(r, j, &num);
        if (!x) {
            continue;
        }
        fprintf(f, "%u h%u %s(%08X, %08X, %08X, %08X) = %08X desde %08X @%llu %s\n", num, x->thread / 4,
                x->name ? x->name : "?", x->args[0], x->args[1], x->args[2], x->args[3], x->ret, x->caller,
                (unsigned long long)x->icount, x->note);
    }
}

bool dump_state(const Runner *r, const XDisc *disc, const char *dir, char *msg, size_t n)
{
    mkdir(dir, 0777);
    const Vm *vm = r->cpu.vm;
    /* primero el texto (lo más útil), sección a sección: si algo falla
     * después, lo escrito queda en la SD */
    char path[512];
    snprintf(path, sizeof(path), "%s/estado.txt", dir);
    remove(path);
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(msg, n, "No se pudo crear %s", path);
        return false;
    }
    setvbuf(f, NULL, _IOLBF, 4096);
    text(f, r);
    fclose(f);
    if (!write_file(dir, "ram.bin", vm->ram, VM_RAM_SIZE) ||
        !write_file(dir, "paginas.bin", vm->pt, (size_t)(1u << 20) * sizeof(uint32_t))) {
        snprintf(msg, n, "No se pudo escribir en %s (%s)", dir, strerror(errno));
        return false;
    }
    /* el default.xbe del disco */
    XDiscFile xf;
    if (disc && xdisc_find(disc, "default.xbe", &xf)) {
        snprintf(path, sizeof(path), "%s/default.xbe", dir);
        remove(path);
        FILE *o = fopen(path, "wb");
        static uint8_t buf[1u << 16];
        for (uint64_t off = 0; o && off < xf.size;) {
            size_t len = xf.size - off < sizeof(buf) ? (size_t)(xf.size - off) : sizeof(buf);
            if (!xdisc_read(disc, &xf, off, buf, len) || fwrite(buf, 1, len, o) != len) {
                break;
            }
            off += len;
        }
        if (o) {
            fclose(o);
        }
    }
    snprintf(msg, n, "Volcado guardado en %s", dir);
    return true;
}
