/*
 * XboxNX — pruebas en PC del kernel: etapa 3a (memoria y objetos) y 3b
 * (hilos, esperas, tiempo, temporizadores y DPC con código x86 de verdad).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/cpu.h"
#include "../source/kernel.h"
#include "../source/disc.h"
#include "../source/hw.h"
#include "../source/nv2a_sw.h"
#include "../source/apu/apu.h"
#include "../source/runner.h"

static int g_fail;
#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                            \
        }                                                        \
    } while (0)

static Vm vm;
static Kernel k;

#define SCRATCH 0x00F00000u /* parámetros de salida */

static uint64_t call(const char *name, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    for (unsigned o = 1; o < K_ORDINAL_MAX; o++) {
        const KExport *e = kernel_export(o);
        if (e && !strcmp(e->name, name)) {
            if (!k.impl[o]) {
                printf("FALLO: %s no está implementada\n", name);
                g_fail++;
                return 0xDEAD;
            }
            KCall c = {.k = &k, .vm = &vm, .a = {a0, a1, a2, a3, a4}};
            return k.impl[o](&c);
        }
    }
    printf("FALLO: %s no existe\n", name);
    g_fail++;
    return 0xDEAD;
}

static uint32_t rd(uint32_t va) { return vm_rd32(&vm, va); }
static void wr(uint32_t va, uint32_t v) { vm_wr32(&vm, va, v); }


/* ---- ensamblador mínimo para las pruebas de hilos ---- */
#define CODE 0x00020000u
static uint8_t g_code[0x2000];
static uint32_t g_pc;

static uint32_t here(void) { return CODE + g_pc; }
static void b1(uint8_t v) { g_code[g_pc++] = v; }
static void d4(uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b1((uint8_t)(v >> (8 * i)));
    }
}
static void push(uint32_t v) { b1(0x68); d4(v); }
static void push_mem(uint32_t va) { b1(0xFF); b1(0x35); d4(va); } /* push [va] */
static void store_eax(uint32_t va) { b1(0xA3); d4(va); }           /* mov [va], eax */
static void kcall(const char *name)
{
    for (unsigned o = 1; o < K_ORDINAL_MAX; o++) {
        const KExport *e = kernel_export(o);
        if (e && !strcmp(e->name, name)) {
            b1(0xB8);
            d4(k.tramp_va[o]); /* mov eax, trampolín */
            b1(0xFF);
            b1(0xD0); /* call eax */
            return;
        }
    }
    printf("FALLO: %s no existe\n", name);
    g_fail++;
}

static Runner g_run;
static LoadReport g_load;

/* Reinicia el kernel, copia el código y arranca un hilo en entry. */
static void boot(uint32_t entry)
{
    vm_free(&vm);
    memset(&k, 0, sizeof(k));
    CHECK(vm_init(&vm));
    CHECK(kernel_init(&k, &vm));
    CHECK(vm_alloc_at(&vm, SCRATCH, 0x1000));
    CHECK(vm_alloc_at(&vm, CODE, sizeof(g_code)));
    CHECK(vm_write(&vm, CODE, g_code, g_pc));
    memset(&g_load, 0, sizeof(g_load));
    g_load.entry = entry;
    CHECK(runner_boot(&g_run, &vm, &k, &g_load));
}

static unsigned count_calls(const char *name)
{
    unsigned n = 0, num;
    for (unsigned j = 0; j < runner_log_len(&g_run); j++) {
        const KCallRec *r = runner_log_at(&g_run, j, &num);
        n += r && !strcmp(r->name, name);
    }
    return n;
}

static void thread_tests(void)
{
    /* 1) dos hilos: el principal crea otro, que duerme 1 ms, escribe su
     *    contexto y vuelve; el principal espera a que termine. */
    g_pc = 0;
    uint32_t worker = 0, main_va;
    /* trabajador: KeDelayExecutionThread(0, 0, &-10000) */
    worker = here();
    push(SCRATCH + 0x100);
    push(0);
    push(0);
    kcall("KeDelayExecutionThread");
    b1(0x8B); b1(0x44); b1(0x24); b1(0x04); /* mov eax, [esp+4] */
    store_eax(SCRATCH + 4);
    b1(0xB8); d4(0x77); /* mov eax, 0x77 (estado de salida) */
    b1(0xC2); b1(0x04); b1(0x00); /* ret 4 */
    main_va = here();
    /* PsCreateSystemThreadEx(&h, 0, 0x4000, 0, 0, worker, 0x55, 0, 0, 0) */
    push(0); push(0); push(0); push(0x55); push(worker); push(0); push(0); push(0x4000); push(0);
    push(SCRATCH);
    kcall("PsCreateSystemThreadEx");
    store_eax(SCRATCH + 0x10);
    push(0); push(0); push_mem(SCRATCH);
    kcall("NtWaitForSingleObject");
    store_eax(SCRATCH + 8);
    push(7);
    kcall("HalReturnToFirmware");
    b1(0xEB); b1(0xFE);
    boot(main_va);
    wr(SCRATCH + 0x100, (uint32_t)-10000); /* -1 ms en unidades de 100 ns */
    wr(SCRATCH + 0x104, 0xFFFFFFFFu);
    wr(SCRATCH + 4, 0);
    wr(SCRATCH + 8, 0xFFFF);
    RunState st = runner_slice(&g_run, 100000);
    CHECK(st == RUN_SYSTEM_STOP);
    CHECK(rd(SCRATCH + 0x10) == 0);
    CHECK(rd(SCRATCH + 4) == 0x55);
    CHECK(rd(SCRATCH + 8) == 0);
    CHECK(k.now_ns >= 1000000u);       /* saltó el milisegundo */
    CHECK(k.switches >= 2);
    CHECK(kps_alive(&k) == 1);
    CHECK(k.threads[1].state == KT_DEAD && k.threads[1].exit_status == 0x77);
    printf("3b hilos: %u cambios, %llu ns virtuales, fin: %s\n", k.switches, (unsigned long long)k.now_ns,
           g_run.end);

    /* 2) evento: el trabajador espera un evento que el principal señala */
    g_pc = 0;
    worker = here();
    push(0); push(0); push_mem(SCRATCH + 0x20);
    kcall("NtWaitForSingleObject");
    store_eax(SCRATCH + 0x24);
    push(0); /* PsTerminateSystemThread(0) */
    kcall("PsTerminateSystemThread");
    main_va = here();
    /* NtCreateEvent(&h, 0, NotificationEvent, FALSE) */
    push(0); push(0); push(0); push(SCRATCH + 0x20);
    kcall("NtCreateEvent");
    push(0); push(0); push(0); push(0x0); push(worker); push(0); push(0); push(0x4000); push(0);
    push(SCRATCH);
    kcall("PsCreateSystemThreadEx");
    kcall("NtYieldExecution"); /* el trabajador corre y se queda esperando */
    push(0); push_mem(SCRATCH + 0x20);
    kcall("NtSetEvent");
    push(0); push(0); push_mem(SCRATCH);
    kcall("NtWaitForSingleObject");
    push(7);
    kcall("HalReturnToFirmware");
    boot(main_va);
    wr(SCRATCH + 0x24, 0xFFFF);
    CHECK(runner_slice(&g_run, 100000) == RUN_SYSTEM_STOP);
    CHECK(rd(SCRATCH + 0x24) == 0);
    CHECK(k.threads[1].state == KT_DEAD);

    /* 3) temporizador con DPC: el DPC escribe su contexto */
    g_pc = 0;
    uint32_t dpc_fn = here();
    b1(0x8B); b1(0x44); b1(0x24); b1(0x08); /* mov eax, [esp+8] (DeferredContext) */
    store_eax(SCRATCH + 0x30);
    b1(0xC2); b1(0x10); b1(0x00); /* ret 16 */
    main_va = here();
    push(SCRATCH + 0x60); push(dpc_fn); push(SCRATCH + 0x40);
    kcall("KeInitializeDpc"); /* (Dpc, Routine, Context) */
    push(0); push(SCRATCH + 0x80);
    kcall("KeInitializeTimerEx");
    /* KeSetTimer(Timer, DueTime = -5 ms por valor, Dpc) */
    push(SCRATCH + 0x40); push(0xFFFFFFFFu); push((uint32_t)-50000); push(SCRATCH + 0x80);
    kcall("KeSetTimer");
    /* KeWaitForSingleObject(Timer, 0, 0, 0, NULL) */
    push(0); push(0); push(0); push(0); push(SCRATCH + 0x80);
    kcall("KeWaitForSingleObject");
    store_eax(SCRATCH + 0x34);
    push(7);
    kcall("HalReturnToFirmware");
    boot(main_va);
    wr(SCRATCH + 0x34, 0xFFFF);
    CHECK(runner_slice(&g_run, 100000) == RUN_SYSTEM_STOP);
    CHECK(rd(SCRATCH + 0x30) == SCRATCH + 0x60);
    CHECK(rd(SCRATCH + 0x34) == 0);
    CHECK(k.now_ns >= 5000000u && k.dpcs_run == 1);

    /* 4) sección crítica disputada entre dos hilos */
    g_pc = 0;
    worker = here();
    push(SCRATCH + 0x200);
    kcall("RtlEnterCriticalSection"); /* espera: el principal la tiene */
    b1(0xA1); d4(SCRATCH + 0x50);      /* mov eax, [x] */
    b1(0x40);                          /* inc eax */
    store_eax(SCRATCH + 0x50);
    push(SCRATCH + 0x200);
    kcall("RtlLeaveCriticalSection");
    b1(0x31); b1(0xC0);                /* xor eax, eax */
    b1(0xC2); b1(0x04); b1(0x00);
    main_va = here();
    push(SCRATCH + 0x200);
    kcall("RtlInitializeCriticalSection");
    push(SCRATCH + 0x200);
    kcall("RtlEnterCriticalSection");
    push(0); push(0); push(0); push(0); push(worker); push(0); push(0); push(0x4000); push(0);
    push(SCRATCH);
    kcall("PsCreateSystemThreadEx");
    kcall("NtYieldExecution");         /* el trabajador se bloquea en la sección */
    b1(0xC7); b1(0x05); d4(SCRATCH + 0x50); d4(100); /* mov dword [x], 100 */
    push(SCRATCH + 0x200);
    kcall("RtlLeaveCriticalSection");
    push(0); push(0); push_mem(SCRATCH);
    kcall("NtWaitForSingleObject");
    push(7);
    kcall("HalReturnToFirmware");
    boot(main_va);
    CHECK(runner_slice(&g_run, 100000) == RUN_SYSTEM_STOP);
    CHECK(rd(SCRATCH + 0x50) == 101);
    CHECK((int32_t)rd(SCRATCH + 0x200 + 0x10) == -1 && rd(SCRATCH + 0x200 + 0x18) == 0);

    /* 5) bloqueo: esperar para siempre un evento que nadie señala */
    g_pc = 0;
    main_va = here();
    push(0); push(1); push(0); push(SCRATCH + 0x20);
    kcall("NtCreateEvent"); /* sincronización, sin señalar */
    push(0); push(0); push_mem(SCRATCH + 0x20);
    kcall("NtWaitForSingleObject");
    b1(0xEB); b1(0xFE);
    boot(main_va);
    CHECK(runner_slice(&g_run, 100000) == RUN_DEADLOCK);

    /* 6) reparto: dos hilos que nunca ceden se turnan por cuanto */
    g_pc = 0;
    worker = here();
    b1(0xFF); b1(0x05); d4(SCRATCH + 0x70); /* inc dword [y] */
    b1(0xEB); b1(0xF8);                      /* jmp worker */
    main_va = here();
    push(0); push(0); push(0); push(0); push(worker); push(0); push(0); push(0x4000); push(0);
    push(SCRATCH);
    kcall("PsCreateSystemThreadEx");
    uint32_t loop = here();
    b1(0xFF); b1(0x05); d4(SCRATCH + 0x74); /* inc dword [z] */
    b1(0xEB); b1((uint8_t)(loop - (here() + 1)));
    boot(main_va);
    CHECK(runner_slice(&g_run, 9000000) == RUN_ACTIVE);
    CHECK(rd(SCRATCH + 0x70) > 1000000 && rd(SCRATCH + 0x74) > 1000000);
    CHECK(count_calls("PsCreateSystemThreadEx") == 1);
}

/* ---- etapa 3c: archivos ---- */

static uint64_t calln(const char *name, unsigned n, const uint32_t *a)
{
    for (unsigned o = 1; o < K_ORDINAL_MAX; o++) {
        const KExport *e = kernel_export(o);
        if (e && !strcmp(e->name, name) && k.impl[o]) {
            KCall c = {.k = &k, .vm = &vm};
            memcpy(c.a, a, n * 4);
            return k.impl[o](&c);
        }
    }
    printf("FALLO: %s no está implementada\n", name);
    g_fail++;
    return 0xDEAD;
}

#define OA   (SCRATCH + 0x390)
#define STR  (SCRATCH + 0x380)
#define IOSB (SCRATCH + 0x3A0)
#define BUF  (SCRATCH + 0x800)

/* OBJECT_ATTRIBUTES con el nombre */
static uint32_t oa(const char *name, uint32_t root)
{
    uint16_t len = (uint16_t)strlen(name);
    vm_write(&vm, SCRATCH + 0x400, name, len);
    wr(STR, len | ((uint32_t)(len + 1) << 16));
    wr(STR + 4, SCRATCH + 0x400);
    wr(OA, root);
    wr(OA + 4, STR);
    wr(OA + 8, 0x40);
    return OA;
}

/* NtCreateFile -> handle o 0 */
static uint32_t create(const char *name, uint32_t access, uint32_t disposition, uint32_t options, uint32_t *st)
{
    uint32_t a[9] = {SCRATCH + 0x3B0, access, oa(name, 0), IOSB, 0, 0x80, 0, disposition, options};
    *st = (uint32_t)calln("NtCreateFile", 9, a);
    return *st == ST_SUCCESS ? rd(SCRATCH + 0x3B0) : 0;
}

static void io_tests(const char *iso)
{
    static XDisc disc;
    vm_free(&vm);
    memset(&k, 0, sizeof(k));
    CHECK(vm_init(&vm));
    CHECK(kernel_init(&k, &vm));
    CHECK(vm_alloc_at(&vm, SCRATCH, 0x2000));
    CHECK(xdisc_open(&disc, iso));
    kio_set_media(&k, &disc, "build-test/hdd");
    uint32_t st;

    /* abrir y leer del disco por D: */
    uint32_t h = create("\\??\\D:\\default.xbe", 0x80100000u, 1, 0x60, &st);
    CHECK(st == ST_SUCCESS && h);
    uint32_t r[8] = {h, 0, 0, 0, IOSB, BUF, 4, 0};
    CHECK(calln("NtReadFile", 8, r) == ST_SUCCESS && rd(IOSB + 4) == 4);
    char magic[5] = {0};
    vm_read(&vm, BUF, magic, 4);
    CHECK(!strcmp(magic, "XBEH"));
    uint32_t q[5] = {h, IOSB, BUF, 0x18, 5};
    CHECK(calln("NtQueryInformationFile", 5, q) == ST_SUCCESS && rd(BUF + 8) > 0x1000);
    CHECK(calln("NtClose", 1, &h) == ST_SUCCESS);
    CHECK(k.files_open == 0);
    /* lo que no está, con su estado */
    create("\\??\\D:\\nada.bin", 0x80000000u, 1, 0, &st);
    CHECK(st == 0xC0000034u);
    create("\\??\\Q:\\x", 0x80000000u, 1, 0, &st);
    CHECK(st == 0xC000003Au);
    /* escribir en el disco: protegido */
    create("\\Device\\CdRom0\\x.txt", 0x40000000u, 2, 0, &st);
    CHECK(st == 0xC00000A2u);

    /* carpeta del disco: listar media\ */
    h = create("\\??\\D:\\media", 0x80000000u, 1, 0x21, &st);
    CHECK(st == ST_SUCCESS);
    uint32_t d[10] = {h, 0, 0, 0, IOSB, BUF, 0x200, 1, 0, 1};
    CHECK(calln("NtQueryDirectoryFile", 10, d) == ST_SUCCESS);
    char name[16] = {0};
    vm_read(&vm, BUF + 0x40, name, rd(BUF + 0x3C) < 15 ? rd(BUF + 0x3C) : 15);
    CHECK(!strcmp(name, "leeme.txt") && rd(BUF + 0x28) == 28);
    d[9] = 0;
    CHECK(calln("NtQueryDirectoryFile", 10, d) == 0x80000006u);
    calln("NtClose", 1, &h);

    /* volumen del disco: GDFX */
    h = create("\\Device\\CdRom0", 0x80000000u, 1, 0x20, &st);
    CHECK(st == ST_SUCCESS);
    uint32_t v[5] = {h, IOSB, BUF, 0x20, 5};
    CHECK(calln("NtQueryVolumeInformationFile", 5, v) == ST_SUCCESS);
    vm_read(&vm, BUF + 12, magic, 4);
    CHECK(!strcmp(magic, "GDFX"));
    calln("NtClose", 1, &h);

    /* disco duro: carpeta, archivo, escribir, leer, renombrar, borrar */
    h = create("\\Device\\Harddisk0\\Partition1\\TDATA", 0xC0000000u, 3, 0x21, &st);
    CHECK(st == ST_SUCCESS);
    calln("NtClose", 1, &h);
    h = create("\\Device\\Harddisk0\\Partition1\\tdata\\prueba.bin", 0xC0000000u, 5, 0x60, &st);
    CHECK(st == ST_SUCCESS && rd(IOSB + 4) != 1);
    vm_write(&vm, BUF, "XboxNX", 6);
    uint32_t w[8] = {h, 0, 0, 0, IOSB, BUF, 6, 0};
    CHECK(calln("NtWriteFile", 8, w) == ST_SUCCESS && rd(IOSB + 4) == 6);
    wr(SCRATCH + 0x3C0, 2);
    wr(SCRATCH + 0x3C4, 0);
    uint32_t r2[8] = {h, 0, 0, 0, IOSB, BUF + 0x100, 16, SCRATCH + 0x3C0};
    CHECK(calln("NtReadFile", 8, r2) == ST_SUCCESS && rd(IOSB + 4) == 4);
    vm_read(&vm, BUF + 0x100, magic, 4);
    CHECK(!memcmp(magic, "oxNX", 4));
    calln("NtClose", 1, &h);
    FILE *hf = fopen("build-test/hdd/Partition1/TDATA/prueba.bin", "rb");
    CHECK(hf != NULL);
    if (hf) {
        fclose(hf);
    }
    /* T: como enlace a la carpeta */
    wr(SCRATCH + 0x300, 4 | (5u << 16));
    wr(SCRATCH + 0x304, SCRATCH + 0x310);
    vm_write(&vm, SCRATCH + 0x310, "\\??\\T:", 6);
    const char *tgt = "\\Device\\Harddisk0\\Partition1\\TDATA";
    wr(SCRATCH + 0x320, (uint32_t)strlen(tgt) | ((uint32_t)(strlen(tgt) + 1) << 16));
    wr(SCRATCH + 0x324, SCRATCH + 0x330);
    vm_write(&vm, SCRATCH + 0x330, tgt, strlen(tgt));
    wr(SCRATCH + 0x300, 6 | (7u << 16));
    uint32_t l[2] = {SCRATCH + 0x300, SCRATCH + 0x320};
    CHECK(calln("IoCreateSymbolicLink", 2, l) == ST_SUCCESS);
    h = create("\\??\\T:\\PRUEBA.BIN", 0x80000000u, 1, 0x60, &st);
    CHECK(st == ST_SUCCESS); /* sin distinguir mayúsculas */
    uint32_t q2[5] = {h, IOSB, BUF, 0x18, 5};
    CHECK(calln("NtQueryInformationFile", 5, q2) == ST_SUCCESS && rd(BUF + 8) == 6);
    calln("NtClose", 1, &h);
    uint32_t del[1] = {oa("\\??\\T:\\prueba.bin", 0)};
    CHECK(calln("NtDeleteFile", 1, del) == ST_SUCCESS);
    hf = fopen("build-test/hdd/Partition1/TDATA/prueba.bin", "rb");
    CHECK(hf == NULL);
    /* crear donde falta la carpeta */
    create("\\Device\\Harddisk0\\Partition1\\no\\x.bin", 0xC0000000u, 2, 0, &st);
    CHECK(st == 0xC000003Au);
    /* partición 0 cruda: ceros */
    h = create("\\Device\\Harddisk0\\Partition0", 0x80000000u, 1, 0x20, &st);
    CHECK(st == ST_SUCCESS);
    wr(BUF, 0xFFFFFFFFu);
    uint32_t r3[8] = {h, 0, 0, 0, IOSB, BUF, 512, 0};
    CHECK(calln("NtReadFile", 8, r3) == ST_SUCCESS && rd(BUF) == 0);
    uint32_t io[10] = {h, 0, 0, 0, IOSB, 0x00074004u, 0, 0, BUF, 32};
    CHECK(calln("NtDeviceIoControlFile", 10, io) == ST_SUCCESS && rd(IOSB + 4) == 32);
    calln("NtClose", 1, &h);
    CHECK(k.files_open == 0);
    printf("3c archivos: %u lecturas, %llu bytes, %u escrituras\n", k.reads, (unsigned long long)k.bytes_read,
           k.writes);
    xdisc_close(&disc);
}

/* ---- etapa 3d: sistema ---- */
static void sys_tests(void)
{
    vm_free(&vm);
    memset(&k, 0, sizeof(k));
    CHECK(vm_init(&vm));
    CHECK(kernel_init(&k, &vm));
    CHECK(vm_alloc_at(&vm, SCRATCH, 0x2000));
    kio_set_media(&k, NULL, "build-test/hdd");
    remove("build-test/hdd/eeprom.bin"); /* una consola nueva en cada prueba */

    /* RtlInitAnsiString y la comparación sin mayúsculas */
    vm_write(&vm, SCRATCH + 0x100, "\\Device\\CdRom0", 15);
    uint32_t a2[2] = {SCRATCH, SCRATCH + 0x100};
    calln("RtlInitAnsiString", 2, a2);
    CHECK((rd(SCRATCH) & 0xFFFF) == 14 && (rd(SCRATCH) >> 16) == 15 && rd(SCRATCH + 4) == SCRATCH + 0x100);
    vm_write(&vm, SCRATCH + 0x120, "\\device\\cdrom0", 15);
    uint32_t b2[2] = {SCRATCH + 8, SCRATCH + 0x120};
    calln("RtlInitAnsiString", 2, b2);
    uint32_t eq[3] = {SCRATCH, SCRATCH + 8, 1};
    CHECK(calln("RtlEqualString", 3, eq) == 1);
    eq[2] = 0;
    CHECK(calln("RtlEqualString", 3, eq) == 0);
    /* a Unicode con su búfer del pool, y de vuelta */
    uint32_t u[3] = {SCRATCH + 0x10, SCRATCH, 1};
    CHECK(calln("RtlAnsiStringToUnicodeString", 3, u) == ST_SUCCESS && (rd(SCRATCH + 0x10) & 0xFFFF) == 28);
    CHECK((rd(rd(SCRATCH + 0x14) + 2) & 0xFFFF) == 'D');
    uint32_t f1[1] = {SCRATCH + 0x10};
    calln("RtlFreeUnicodeString", 1, f1);
    CHECK(rd(SCRATCH + 0x14) == 0);
    /* errores */
    uint32_t e1[1] = {0xC0000034u};
    CHECK(calln("RtlNtStatusToDosError", 1, e1) == 2);
    /* tiempo: ida y vuelta */
    wr(SCRATCH + 0x40, 0x5A4D5000u);
    wr(SCRATCH + 0x44, 0x01DB1C2Au);
    uint32_t t1[2] = {SCRATCH + 0x40, SCRATCH + 0x50};
    calln("RtlTimeToTimeFields", 2, t1);
    uint32_t t2[2] = {SCRATCH + 0x50, SCRATCH + 0x60};
    CHECK(calln("RtlTimeFieldsToTime", 2, t2) == 1);
    CHECK(rd(SCRATCH + 0x64) == 0x01DB1C2Au && rd(SCRATCH + 0x60) / 10000 == 0x5A4D5000u / 10000);
    /* memoria que cruza páginas */
    uint32_t m1[3] = {SCRATCH + 0xFF0, 0x40, 0xAB};
    calln("RtlFillMemory", 3, m1);
    CHECK((rd(SCRATCH + 0x1000) & 0xFF) == 0xAB);
    uint32_t m2[3] = {SCRATCH + 0xFF8, SCRATCH + 0xFF0, 0x40};
    calln("RtlMoveMemory", 3, m2);
    uint32_t m3[3] = {SCRATCH + 0xFF0, SCRATCH + 0xFF8, 0x40};
    CHECK(calln("RtlCompareMemory", 3, m3) == 0x40);

    /* configuración: idioma inglés, región NTSC */
    uint32_t q[5] = {7, SCRATCH + 0x70, SCRATCH + 0x74, 4, SCRATCH + 0x78};
    CHECK(calln("ExQueryNonVolatileSetting", 5, q) == ST_SUCCESS && rd(SCRATCH + 0x74) == 1 &&
          rd(SCRATCH + 0x70) == 4 && rd(SCRATCH + 0x78) == 4);
    uint32_t q2[5] = {0x11, 0, SCRATCH + 0x74, 4, 0};
    CHECK(calln("ExQueryNonVolatileSetting", 5, q2) == ST_SUCCESS);
    uint32_t q3[5] = {0x999, 0, SCRATCH + 0x74, 4, 0};
    CHECK(calln("ExQueryNonVolatileSetting", 5, q3) == 0xC0000034u);
    /* guardar el idioma (español = 3) y leerlo */
    wr(SCRATCH + 0x7C, 3);
    uint32_t sv[4] = {7, 4, SCRATCH + 0x7C, 4};
    CHECK(calln("ExSaveNonVolatileSetting", 4, sv) == ST_SUCCESS);
    CHECK(calln("ExQueryNonVolatileSetting", 5, q) == ST_SUCCESS && rd(SCRATCH + 0x74) == 3);

    /* SHA-1("abc") = a9993e36... */
    vm_write(&vm, SCRATCH + 0x200, "abc", 3);
    uint32_t s1[1] = {SCRATCH + 0x300};
    calln("XcSHAInit", 1, s1);
    uint32_t s2[3] = {SCRATCH + 0x300, SCRATCH + 0x200, 3};
    calln("XcSHAUpdate", 3, s2);
    uint32_t s3[2] = {SCRATCH + 0x300, SCRATCH + 0x380};
    calln("XcSHAFinal", 2, s3);
    CHECK(rd(SCRATCH + 0x380) == 0x363E99A9u);
    /* RC4 dos veces = el original */
    vm_write(&vm, SCRATCH + 0x200, "Halo", 4);
    vm_write(&vm, SCRATCH + 0x210, "clave", 5);
    uint32_t r1[3] = {SCRATCH + 0x400, 5, SCRATCH + 0x210};
    uint32_t r2[3] = {SCRATCH + 0x400, 4, SCRATCH + 0x200};
    calln("XcRC4Key", 3, r1);
    calln("XcRC4Crypt", 3, r2);
    CHECK(rd(SCRATCH + 0x200) != 0x6F6C6148u);
    calln("XcRC4Key", 3, r1);
    calln("XcRC4Crypt", 3, r2);
    CHECK(rd(SCRATCH + 0x200) == 0x6F6C6148u);
    /* la clave del disco duro quedó en XboxHDKey */
    CHECK(rd(kernel_data_va(&k, "XboxHDKey")) == 0x30303030u);
    printf("3d sistema: %u funciones reales\n", kernel_real_count(&k));
}

/* ---- etapa 3e: hardware e interrupciones ---- */
static void mov_mem_imm(uint32_t va, uint32_t v) { b1(0xC7); b1(0x05); d4(va); d4(v); }

static void hw_tests(void)
{
    const uint32_t EV = SCRATCH + 0x600, DPC = SCRATCH + 0x620, KI = SCRATCH + 0x640;
    g_pc = 0;
    uint32_t isr = here();
    mov_mem_imm(0xFD600100u, 1);                 /* PCRTC_INTR_0: atendido */
    b1(0xFF); b1(0x05); d4(SCRATCH + 0x500);     /* inc dword [contador] */
    push(0); push(0); push(DPC);
    kcall("KeInsertQueueDpc");
    b1(0xB8); d4(1);
    b1(0xC2); b1(0x08); b1(0x00);                /* ret 8 */
    uint32_t dpc = here();
    push(0); push(0); push(EV);
    kcall("KeSetEvent");
    b1(0xC2); b1(0x10); b1(0x00);
    uint32_t sync = here();
    b1(0x8B); b1(0x44); b1(0x24); b1(0x04); b1(0x40); b1(0xC2); b1(0x04); b1(0x00);
    uint32_t main_va = here();
    push(0); push(1); push(EV);
    kcall("KeInitializeEvent");
    push(0); push(dpc); push(DPC);
    kcall("KeInitializeDpc");
    push(0); push(0); push(23); push(0x33); push(0x77); push(isr); push(KI);
    kcall("KeInitializeInterrupt");
    push(KI);
    kcall("KeConnectInterrupt");
    store_eax(SCRATCH + 0x504);
    mov_mem_imm(0xFD000140u, 1);                 /* PMC_INTR_EN_0 */
    mov_mem_imm(0xFD600140u, 1);                 /* PCRTC_INTR_EN_0: el vblank */
    push(0); push(0); push(0); push(0); push(EV);
    kcall("KeWaitForSingleObject");
    store_eax(SCRATCH + 0x508);
    push(41); push(sync); push(KI);
    kcall("KeSynchronizeExecution");
    store_eax(SCRATCH + 0x50C);
    b1(0xA1); d4(0xFD000000u);                   /* mov eax, [PMC_BOOT_0] */
    store_eax(SCRATCH + 0x510);
    b1(0xBA); d4(0x8008);                        /* mov edx, 0x8008 */
    b1(0xED);                                    /* in eax, dx: el reloj ACPI */
    store_eax(SCRATCH + 0x514);
    push(7);
    kcall("HalReturnToFirmware");
    boot(main_va);
    CHECK(runner_slice(&g_run, 1000000) == RUN_SYSTEM_STOP);
    CHECK(rd(SCRATCH + 0x504) == 1);
    CHECK(rd(SCRATCH + 0x508) == 0);
    CHECK(rd(SCRATCH + 0x500) >= 1 && k.isrs_run >= 1 && k.dpcs_run >= 1);
    CHECK(rd(SCRATCH + 0x50C) == 42);
    CHECK(rd(SCRATCH + 0x510) == 0x02A000A1u);
    CHECK(rd(SCRATCH + 0x514) > 50000);           /* > 15 ms a 3,375 MHz */
    CHECK(k.now_ns >= 16000000u);
    printf("3e hardware: %u ISR, %u DPC, %.1f ms virtuales, fin: %s\n", k.isrs_run, k.dpcs_run, k.now_ns / 1e6,
           g_run.end);

    /* AC97: el códec listo, el SigmaTel, y un descriptor de 480 muestras que se consume en 10 ms */
    uint32_t v = 0;
    CHECK(hw_mmio(&k, 0xFEC00130u, 4, false, &v) && (v & 0x100));
    CHECK(hw_mmio(&k, 0xFEC0007Cu, 2, false, &v) && v == 0x8384);
    uint32_t bd = vm_phys(&vm, SCRATCH + 0x700);
    wr(SCRATCH + 0x700, 0x00100000u);
    wr(SCRATCH + 0x704, 0x80000000u | 480);      /* IOC, 480 muestras de 16 bits */
    v = bd;
    hw_mmio(&k, 0xFEC00110u, 4, true, &v);       /* PO BDBAR */
    v = 0;
    hw_mmio(&k, 0xFEC00115u, 1, true, &v);       /* LVI 0 */
    v = 0x11;
    hw_mmio(&k, 0xFEC0011Bu, 1, true, &v);       /* CR: RPBM | IOCE */
    k.now_ns += 10000000u;
    hw_tick(&k);
    CHECK(hw_mmio(&k, 0xFEC00116u, 2, false, &v) && (v & 0x08) && (v & 0x01)); /* BCIS y DCH */
    CHECK(hw_irq_lines(&k) & (1u << HW_IRQ_ACI));

    /* APU: la cola del VP libre, la memoria del programa del GP (24 bits) y el
     * contador de muestras a 48 kHz */
    CHECK(hw_mmio(&k, 0xFE820010u, 4, false, &v) && v == 0x80);
    v = 0xAB123456u;
    hw_mmio(&k, 0xFE830000u + 0xA000u + 4 * 7, 4, true, &v);
    CHECK(hw_mmio(&k, 0xFE830000u + 0xA000u + 4 * 7, 4, false, &v) && v == 0x123456);
    uint32_t c0 = 0, c1 = 0;
    hw_mmio(&k, 0xFE80200Cu, 4, false, &c0);
    k.now_ns += 1000000u;
    hw_tick(&k);
    hw_mmio(&k, 0xFE80200Cu, 4, false, &c1);
    CHECK(c1 - c0 == 48);

    /* USB: el control del jugador 1 (su hub) conectado en el puerto raíz 3;
     * operativo, cada milisegundo virtual es un cuadro (SF) */
    CHECK(hw_mmio(&k, 0xFED00000u, 4, false, &v) && v == 0x10);
    CHECK(hw_mmio(&k, 0xFED00054u + 8, 4, false, &v) && (v & 1));
    v = 2u << 6; /* HCFS operativo */
    hw_mmio(&k, 0xFED00004u, 4, true, &v);
    k.now_ns += 3000000u;
    hw_tick(&k);
    CHECK(hw_mmio(&k, 0xFED0000Cu, 4, false, &v) && (v & 4));
    CHECK(hw_mmio(&k, 0xFED0003Cu, 4, false, &v) && v >= 2);

    /* GPU: un push buffer que borra un rectángulo de la superficie de color
     * (CLEAR_SURFACE por software) y la pantalla que se ve */
    {
        const uint32_t NV = 0xFD000000u, PB = 0x00F00000u, SURF = 0x01000000u;
        const uint32_t H_KELVIN = 0xBEEF0001u, H_COLOR = 0xBEEF0002u;
        v = 0x100; /* RAMHT en 0x10000 de PRAMIN, 4 KB */
        hw_mmio(&k, NV + 0x2210u, 4, true, &v);
        uint32_t handles[2] = {H_KELVIN, H_COLOR}, insts[2] = {0x2000, 0x2100};
        for (int i = 0; i < 2; i++) {
            uint32_t hash = 0;
            for (uint32_t h = handles[i]; h; h >>= 11) {
                hash ^= h & 0x7FF;
            }
            uint32_t e = 0x10000 + 8 * (hash & 511);
            v = handles[i];
            hw_mmio(&k, NV + 0x700000u + e, 4, true, &v);
            v = 0x80000000u | (insts[i] >> 4);
            hw_mmio(&k, NV + 0x700000u + e + 4, 4, true, &v);
        }
        v = 0x97;
        hw_mmio(&k, NV + 0x700000u + 0x2000, 4, true, &v);
        v = 0x3D;
        hw_mmio(&k, NV + 0x700000u + 0x2100, 4, true, &v);
        v = SURF;
        hw_mmio(&k, NV + 0x700000u + 0x2108, 4, true, &v);
        static const uint32_t pb[] = {
            (1u << 18) | 0x0000, H_KELVIN,               /* SET_OBJECT */
            (1u << 18) | 0x0194, H_COLOR,                /* SET_CONTEXT_DMA_COLOR */
            (6u << 18) | 0x01FC, 0, 64u << 16, 32u << 16, 0x108, 256, 0x1000, /* clip, formato, pitch, offset */
            (1u << 18) | 0x1D90, 0xFF336699u,            /* SET_COLOR_CLEAR_VALUE */
            (2u << 18) | 0x1D98, (63u << 16) | 2, (31u << 16) | 1, /* el rectángulo */
            (1u << 18) | 0x1D94, 0xF0,                   /* CLEAR_SURFACE: R, G, B, A */
            (1u << 18) | 0x1760, 0x2032,                 /* atributo 0: 3 float, paso 32 */
            (1u << 18) | 0x17FC, 5,                      /* triángulos */
            (1u << 18) | 0x1810, (5u << 24) | 0,         /* DRAW_ARRAYS: 6 vértices */
            (1u << 18) | 0x17FC, 0,
            (1u << 18) | 0x17FC, 6,                      /* tira con INLINE_ARRAY: 4 vértices */
            0x40000000u | (12u << 18) | 0x1818, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            (1u << 18) | 0x17FC, 0,
            (1u << 18) | 0x012C, 0,                      /* FLIP_INCREMENT_WRITE: fin del cuadro */
        };
        memcpy(vm.ram + PB, pb, sizeof(pb));
        v = 1;
        hw_mmio(&k, NV + 0x400720u, 4, true, &v);       /* PGRAPH_FIFO abierta */
        v = PB;
        hw_mmio(&k, NV + 0x800044u, 4, true, &v);       /* GET */
        v = PB + (uint32_t)sizeof(pb);
        hw_mmio(&k, NV + 0x800040u, 4, true, &v);       /* PUT: corre */
        const uint32_t *fb = (const uint32_t *)(vm.ram + SURF + 0x1000);
        CHECK(hw_stats()->clears == 1);
        const HwDraws *dw = &hw_stats()->last;
        CHECK(dw->draws == 2 && dw->vertices == 10 && dw->prims == ((1u << 5) | (1u << 6)));
        CHECK(fb[1 * 64 + 2] == 0xFF336699u && fb[31 * 64 + 63] == 0xFF336699u);
        CHECK(fb[0] == 0 && fb[1 * 64 + 1] == 0);
        /* un dibujo: vertex program MOV oPos, v0; MOV oD0, v3 y un abanico
         * de vértices inmediatos rojo, de (8, 4) a (24, 12) */
        {
            uint32_t q[128];
            unsigned n = 0;
            static const uint32_t prog[8] = {0, 0x0020001B, 0x0836106C, 0x2070F800,
                                             0, 0x0020061B, 0x0836106C, 0x2070F819};
            q[n++] = (1u << 18) | 0x1E94, q[n++] = 2;              /* modo: programa */
            q[n++] = (1u << 18) | 0x1E9C, q[n++] = 0;              /* cargar en la ranura 0 */
            q[n++] = (8u << 18) | 0x0B00;
            for (int i = 0; i < 8; i++) {
                q[n++] = prog[i];
            }
            q[n++] = (1u << 18) | 0x1EA0, q[n++] = 0;              /* empieza en 0 */
            q[n++] = (1u << 18) | 0x17FC, q[n++] = 7;              /* abanico */
            static const float xy[4][2] = {{8, 4}, {24, 4}, {24, 12}, {8, 12}};
            for (int i = 0; i < 4; i++) {
                q[n++] = (1u << 18) | (0x1940 + 12), q[n++] = 0xFF0000FFu; /* v3: rojo */
                q[n++] = (4u << 18) | 0x1A00;
                float f[4] = {xy[i][0], xy[i][1], 0, 1};
                for (int j = 0; j < 4; j++) {
                    memcpy(&q[n++], &f[j], 4);
                }
            }
            q[n++] = (1u << 18) | 0x17FC, q[n++] = 0;
            uint32_t at = PB + 0x1000;
            memcpy(vm.ram + at, q, n * 4);
            v = at;
            hw_mmio(&k, NV + 0x800044u, 4, true, &v);
            v = at + n * 4;
            hw_mmio(&k, NV + 0x800040u, 4, true, &v);
            CHECK(sw_stats()->skipped_ff == 2 && sw_stats()->triangles == 2 && sw_stats()->pixels == 16 * 8);
            CHECK(fb[5 * 64 + 9] == 0xFFFF0000u && fb[11 * 64 + 23] == 0xFFFF0000u);
            CHECK(fb[3 * 64 + 9] == 0xFF336699u && fb[5 * 64 + 25] == 0xFF336699u);
        }
        /* la pantalla: AvSetDisplayMode con ese búfer, 64 píxeles de ancho */
        hw_set_display(&k, 0x1E, 256, SURF + 0x1000);
        XImage img = {0};
        CHECK(hw_screen(&k, &img) && img.w == 64 && img.px[(1 * 64 + 2) * 4] == 0x33 &&
              img.px[(1 * 64 + 2) * 4 + 2] == 0x99);
        free(img.px);
    }
}

int main(int argc, char **argv)
{
    if (getenv("APU_HILO")) {
        apu_set_threaded(true); /* el audio en su hilo, como en la Switch */
    }
    CHECK(vm_init(&vm));
    CHECK(kernel_init(&k, &vm));
    CHECK(vm_alloc_at(&vm, SCRATCH, 0x1000));
    printf("funciones reales: %u\n", kernel_real_count(&k));
    CHECK(kernel_real_count(&k) >= 40);

    /* ---- contigua ---- */
    uint32_t free0 = vm_free_pages(&vm);
    uint32_t a = (uint32_t)call("MmAllocateContiguousMemory", 0x1000, 0, 0, 0, 0);
    CHECK(a >= 0x80000000u && a < 0x84000000u && !(a & 0xFFF));
    CHECK(vm_free_pages(&vm) == free0 - 1);
    wr(a, 0x12345678);
    CHECK(rd(a) == 0x12345678);
    uint32_t b = (uint32_t)call("MmAllocateContiguousMemory", 0x2001, 0, 0, 0, 0);
    CHECK(b && b != a);
    CHECK(call("MmQueryAllocationSize", b, 0, 0, 0, 0) == 0x3000);
    CHECK(call("MmGetPhysicalAddress", b, 0, 0, 0, 0) == (b & 0x7FFFFFFF));
    call("MmFreeContiguousMemory", a, 0, 0, 0, 0);
    uint32_t a2 = (uint32_t)call("MmAllocateContiguousMemory", 0x1000, 0, 0, 0, 0);
    CHECK(a2 == a); /* la más baja libre otra vez */
    /* Ex con límites y alineación: el bloque fijo de Halo desde 0x61000 */
    uint32_t c = (uint32_t)call("MmAllocateContiguousMemoryEx", 0x3000, 0x61000, 0x6FFFF, 0x2000, 4);
    CHECK(c && (c & 0x7FFFFFFF) >= 0x62000 && !((c & 0x7FFFFFFF) & 0x1FFF));
    CHECK((c & 0x7FFFFFFF) + 0x3000 <= 0x70000);
    /* un pedido imposible devuelve 0 */
    CHECK(call("MmAllocateContiguousMemoryEx", 0x20000, 0x61000, 0x6FFFF, 0, 4) == 0);

    /* ---- virtual ---- */
    wr(SCRATCH, 0);
    wr(SCRATCH + 4, 0x5000);
    CHECK(call("NtAllocateVirtualMemory", SCRATCH, 0, SCRATCH + 4, 0x3000, 4) == ST_SUCCESS);
    uint32_t v = rd(SCRATCH);
    CHECK(v >= 0x10000 && v < 0x7FFE0000 && !(v & 0xFFFF));
    CHECK(rd(SCRATCH + 4) == 0x5000);
    CHECK(vm_phys(&vm, v) != VM_NO_PHYS && vm_phys(&vm, v + 0x4000) != VM_NO_PHYS);
    /* no puede chocar con la reserva que ya existe */
    wr(SCRATCH + 8, v);
    wr(SCRATCH + 12, 0x1000);
    CHECK(call("NtAllocateVirtualMemory", SCRATCH + 8, 0, SCRATCH + 12, 0x2000, 4) == ST_CONFLICTING_ADDRESSES);
    /* NtQueryVirtualMemory */
    CHECK(call("NtQueryVirtualMemory", v + 0x1234, SCRATCH + 0x100, 0, 0, 0) == ST_SUCCESS);
    CHECK(rd(SCRATCH + 0x100) == v + 0x1000 && rd(SCRATCH + 0x104) == v && rd(SCRATCH + 0x10C) == 0x4000 &&
          rd(SCRATCH + 0x110) == 0x1000);
    /* liberar todo */
    uint32_t before = vm_free_pages(&vm);
    wr(SCRATCH + 4, 0);
    CHECK(call("NtFreeVirtualMemory", SCRATCH, SCRATCH + 4, 0x8000, 0, 0) == ST_SUCCESS);
    CHECK(vm_free_pages(&vm) == before + 5);
    CHECK(vm_phys(&vm, v) == VM_NO_PHYS);
    /* reservar en una dirección, luego commit de una parte y decommit */
    wr(SCRATCH, 0x00400000);
    wr(SCRATCH + 4, 0x100000);
    CHECK(call("NtAllocateVirtualMemory", SCRATCH, 0, SCRATCH + 4, 0x2000, 4) == ST_SUCCESS);
    CHECK(rd(SCRATCH) == 0x00400000 && vm_phys(&vm, 0x00400000) == VM_NO_PHYS);
    wr(SCRATCH, 0x00402000);
    wr(SCRATCH + 4, 0x1800);
    CHECK(call("NtAllocateVirtualMemory", SCRATCH, 0, SCRATCH + 4, 0x1000, 4) == ST_SUCCESS);
    CHECK(rd(SCRATCH) == 0x00402000 && rd(SCRATCH + 4) == 0x2000);
    CHECK(vm_phys(&vm, 0x00402000) != VM_NO_PHYS && vm_phys(&vm, 0x00403FFF) != VM_NO_PHYS);
    CHECK(vm_phys(&vm, 0x00404000) == VM_NO_PHYS);
    wr(SCRATCH, 0x00402000);
    wr(SCRATCH + 4, 0x1000);
    CHECK(call("NtFreeVirtualMemory", SCRATCH, SCRATCH + 4, 0x4000, 0, 0) == ST_SUCCESS);
    CHECK(vm_phys(&vm, 0x00402000) == VM_NO_PHYS && vm_phys(&vm, 0x00403000) != VM_NO_PHYS);
    /* commit fuera de reservas */
    wr(SCRATCH, 0x07000000);
    wr(SCRATCH + 4, 0x1000);
    CHECK(call("NtAllocateVirtualMemory", SCRATCH, 0, SCRATCH + 4, 0x1000, 4) == ST_NOT_COMMITTED);

    /* ---- pool ---- */
    uint32_t p1 = (uint32_t)call("ExAllocatePoolWithTag", 100, 0x41424344, 0, 0, 0);
    CHECK(p1 >= 0x80000000u && !(p1 & 0xF));
    CHECK(call("ExQueryPoolBlockSize", p1, 0, 0, 0, 0) == 100);
    uint32_t p2 = (uint32_t)call("ExAllocatePool", 0x3000, 0, 0, 0, 0); /* páginas propias */
    CHECK(p2 && call("ExQueryPoolBlockSize", p2, 0, 0, 0, 0) == 0x3000);
    wr(p2 + 0x2FFC, 7);
    CHECK(rd(p2 + 0x2FFC) == 7);
    uint32_t fp = vm_free_pages(&vm);
    call("ExFreePool", p2, 0, 0, 0, 0);
    CHECK(vm_free_pages(&vm) == fp + 4);
    call("ExFreePool", p1, 0, 0, 0, 0);
    uint32_t p3 = (uint32_t)call("ExAllocatePool", 100, 0, 0, 0, 0);
    CHECK(p3 == p1); /* el hueco se reutiliza */
    /* crecer el pool más allá de su primer trozo */
    uint32_t last = 0;
    for (int i = 0; i < 200; i++) {
        last = (uint32_t)call("ExAllocatePool", 1000, 0, 0, 0, 0);
    }
    CHECK(last != 0);

    /* ---- objetos ---- */
    CHECK(call("NtCreateEvent", SCRATCH + 0x20, 0, 1, 0, 0) == ST_SUCCESS);
    uint32_t he = rd(SCRATCH + 0x20);
    CHECK(he && !(he & 3));
    CHECK(call("NtSetEvent", he, SCRATCH + 0x24, 0, 0, 0) == ST_SUCCESS && rd(SCRATCH + 0x24) == 0);
    CHECK(call("NtSetEvent", he, SCRATCH + 0x24, 0, 0, 0) == ST_SUCCESS && rd(SCRATCH + 0x24) == 1);
    /* referencia por handle con el tipo correcto y con otro */
    uint32_t evtype = kernel_data_va(&k, "ExEventObjectType");
    uint32_t semtype = kernel_data_va(&k, "ExSemaphoreObjectType");
    CHECK(call("ObReferenceObjectByHandle", he, evtype, SCRATCH + 0x28, 0, 0) == ST_SUCCESS);
    uint32_t eo = rd(SCRATCH + 0x28);
    CHECK(eo && *vm_host(&vm, eo) == 1 /* sincronización */ && rd(eo + 4) == 1);
    CHECK(rd(eo - 0x10) == 2); /* PointerCount: creación + referencia */
    CHECK(call("ObReferenceObjectByHandle", he, semtype, SCRATCH + 0x28, 0, 0) == ST_OBJECT_TYPE_MISMATCH);
    CHECK(call("NtClose", he, 0, 0, 0, 0) == ST_SUCCESS);
    CHECK(call("NtClose", he, 0, 0, 0, 0) == ST_INVALID_HANDLE);
    CHECK(call("NtClose", 0, 0, 0, 0, 0) == ST_INVALID_HANDLE);
    /* semáforo */
    CHECK(call("NtCreateSemaphore", SCRATCH + 0x30, 0, 1, 3, 0) == ST_SUCCESS);
    uint32_t hs = rd(SCRATCH + 0x30);
    CHECK(call("NtReleaseSemaphore", hs, 2, SCRATCH + 0x34, 0, 0) == ST_SUCCESS && rd(SCRATCH + 0x34) == 1);
    CHECK(call("NtReleaseSemaphore", hs, 1, 0, 0, 0) == 0xC0000047u);
    CHECK(call("NtCreateSemaphore", SCRATCH + 0x30, 0, 5, 3, 0) == ST_INVALID_PARAMETER);
    /* mutex */
    CHECK(call("NtCreateMutant", SCRATCH + 0x38, 0, 1, 0, 0) == ST_SUCCESS);
    uint32_t hm = rd(SCRATCH + 0x38);
    CHECK(call("NtReleaseMutant", hm, SCRATCH + 0x3C, 0, 0, 0) == ST_SUCCESS && rd(SCRATCH + 0x3C) == 0);
    CHECK(call("NtReleaseMutant", hm, 0, 0, 0, 0) == 0xC0000046u);
    /* duplicar y cerrar el origen */
    CHECK(call("NtDuplicateObject", hm, SCRATCH + 0x40, 1, 0, 0) == ST_SUCCESS);
    CHECK(rd(SCRATCH + 0x40) != hm && call("NtClose", hm, 0, 0, 0, 0) == ST_INVALID_HANDLE);

    /* ---- estadísticas ---- */
    wr(SCRATCH + 0x200, 0x24);
    CHECK(call("MmQueryStatistics", SCRATCH + 0x200, 0, 0, 0, 0) == ST_SUCCESS);
    CHECK(rd(SCRATCH + 0x204) == 16384 && rd(SCRATCH + 0x208) == vm_free_pages(&vm));
    wr(SCRATCH + 0x200, 0);
    CHECK(call("MmQueryStatistics", SCRATCH + 0x200, 0, 0, 0, 0) == ST_INVALID_PARAMETER);

    /* ---- pila del sistema ---- */
    uint32_t st = kmm_stack_alloc(&k, 0x3000);
    CHECK(st >= 0xD0000000u && vm_phys(&vm, st) != VM_NO_PHYS && vm_phys(&vm, st - 1) == VM_NO_PHYS);

    thread_tests();
    hw_tests();
    if (argc > 1) {
        io_tests(argv[1]);
    }
    sys_tests();

    vm_free(&vm);
    printf(g_fail ? "%d fallos\n" : "todo bien\n", g_fail);
    return g_fail ? 1 : 0;
}
