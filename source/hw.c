/*
 * XboxNX — hardware del Xbox simulado (etapa 3e). Ver hw.h.
 */
#include "hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aci.h"
#include "kernel.h"

/* ---- ventanas ---- */
#define NV2A_BASE  0xFD000000u
#define NV2A_SIZE  0x01000000u
#define APU_BASE   0xFE800000u
#define APU_SIZE   0x00080000u
#define ACI_BASE   0xFEC00000u
#define ACI_SIZE   0x00001000u
#define USB0_BASE  0xFED00000u
#define USB1_BASE  0xFED08000u
#define USB_SIZE   0x00001000u
#define NIC_BASE   0xFEF00000u
#define NIC_SIZE   0x00001000u
#define FLASH_BASE 0xFF000000u

/* ---- NV2A ---- */
#define NV_PMC_BOOT_0          0x000000u
#define NV_PMC_INTR_0          0x000100u
#define NV_PMC_INTR_EN_0       0x000140u
#define NV_PFIFO_RAMHT         0x002210u
#define NV_PFIFO_RUNOUT_STATUS 0x002400u
#define NV_PFIFO_CACHE1_PUSH1  0x003204u
#define NV_PFIFO_CACHE1_STATUS 0x003214u
#define NV_PFIFO_CACHE1_DMA_PUSH 0x003220u
#define NV_PFIFO_CACHE1_DMA_INSTANCE 0x00322Cu
#define NV_PFIFO_CACHE1_DMA_PUT  0x003240u
#define NV_PFIFO_CACHE1_DMA_GET  0x003244u
#define NV_PFIFO_CACHE1_REF      0x003248u
#define NV_PFIFO_CACHE1_DMA_SUBROUTINE 0x00324Cu
#define NV_PVIDEO_BUFFER       0x008700u
#define NV_PVIDEO_STOP         0x008704u
#define NV_PTIMER_INTR_0       0x009100u
#define NV_PTIMER_INTR_EN_0    0x009140u
#define NV_PTIMER_TIME_0       0x009400u
#define NV_PTIMER_TIME_1       0x009410u
#define NV_PTIMER_ALARM_0      0x009420u
#define NV_PFB_CSTATUS         0x10020Cu
#define NV_PFB_WBC             0x100410u
#define NV_PGRAPH_INTR         0x400100u
#define NV_PGRAPH_NSOURCE      0x400108u
#define NV_PGRAPH_INTR_EN      0x400140u
#define NV_PGRAPH_TRAPPED_ADDR 0x400704u
#define NV_PGRAPH_TRAPPED_DATA 0x400708u
#define NV_PGRAPH_SURFACE      0x400710u
#define NV_PGRAPH_INCREMENT    0x40071Cu
#define NV_PGRAPH_FIFO         0x400720u
#define NV_PCRTC_INTR_0        0x600100u
#define NV_PCRTC_INTR_EN_0     0x600140u
#define NV_PCRTC_START         0x600800u
#define NV_PCRTC_RASTER        0x600808u
#define NV_PRAMIN              0x700000u
#define NV_PRAMIN_SIZE         0x100000u
#define NV_USER_DMA_PUT        0x800040u
#define NV_USER_DMA_GET        0x800044u
#define NV_USER_REF            0x800048u

#define PMC_INTR_PGRAPH (1u << 12)
#define PMC_INTR_PTIMER (1u << 20)
#define PMC_INTR_PCRTC  (1u << 24)
#define FRAME_NS  16683333ull /* 59.94 Hz */
#define LINES     525u

#define SURF_WRITE  0x00700000u
#define SURF_READ   0x07000000u
#define SURF_MODULO 0x70000000u

#define NV097_SET_FLIP_READ        0x0120u
#define NV097_SET_FLIP_WRITE       0x0124u
#define NV097_SET_FLIP_MODULO      0x0128u
#define NV097_FLIP_INCREMENT_WRITE 0x012Cu
#define NV097_FLIP_STALL           0x0130u
#define NV097_SET_CONTEXT_DMA_SEMAPHORE 0x01A4u
#define NV097_SET_CONTEXT_DMA_REPORT    0x01A8u
#define NV097_GET_REPORT           0x17D0u
#define NV097_SET_SEMAPHORE_OFFSET 0x1D6Cu
#define NV097_SEMAPHORE_RELEASE    0x1D70u

enum { PB_GO, PB_FLIP, PB_TRAP, PB_FIFO, PB_ERROR };

typedef struct Ohci {
    uint32_t r[0x60 / 4];
} Ohci;

typedef struct Hw {
    uint8_t *nv;          /* 16 MB de registros del NV2A */
    uint8_t *apu;
    uint8_t nic[NIC_SIZE];
    Ohci usb[2];
    uint64_t next_vblank;
    uint64_t alarm_due;
    bool alarm_on;
    /* el lector del push buffer */
    bool sub_active;
    uint32_t sub_return;
    uint32_t method, left;
    uint8_t subch;
    bool noninc;
    uint32_t obj_inst[8];
    uint8_t obj_class[8];
    uint32_t sem_inst, sem_offset, report_inst;
    int stall;
    HwStats st;
} Hw;

static Hw g_hw;

static uint32_t nv_ld(uint32_t off) { return *(uint32_t *)(g_hw.nv + off); }
static void nv_st(uint32_t off, uint32_t v) { *(uint32_t *)(g_hw.nv + off) = v; }

bool hw_init(Kernel *k)
{
    if (!g_hw.nv) {
        g_hw.nv = calloc(1, NV2A_SIZE);
        g_hw.apu = calloc(1, APU_SIZE);
    } else {
        memset(g_hw.nv, 0, NV2A_SIZE);
        memset(g_hw.apu, 0, APU_SIZE);
    }
    if (!g_hw.nv || !g_hw.apu) {
        return false;
    }
    uint8_t *nv = g_hw.nv, *apu = g_hw.apu;
    memset(&g_hw, 0, sizeof(g_hw));
    g_hw.nv = nv;
    g_hw.apu = apu;
    nv_st(NV_PMC_BOOT_0, 0x02A000A1u);
    nv_st(NV_PFB_CSTATUS, 64u << 20);
    nv_st(NV_PGRAPH_FIFO, 1);
    g_hw.next_vblank = FRAME_NS;
    aci_init(k);
    for (int i = 0; i < 2; i++) {
        Ohci *o = &g_hw.usb[i];
        o->r[0x00 / 4] = 0x10;                   /* HcRevision 1.0 */
        o->r[0x34 / 4] = 0x27782EDF;             /* HcFmInterval */
        o->r[0x48 / 4] = 0x02001204 & ~0xFFu;    /* HcRhDescriptorA: power switching */
        o->r[0x48 / 4] |= 4;                     /* 4 puertos raíz */
    }
    return true;
}

const HwStats *hw_stats(void) { return &g_hw.st; }

/* ================================================================ NV2A */

static uint32_t pmc_intr(void)
{
    uint32_t v = 0;
    if (nv_ld(NV_PCRTC_INTR_0) & nv_ld(NV_PCRTC_INTR_EN_0)) {
        v |= PMC_INTR_PCRTC;
    }
    if (nv_ld(NV_PGRAPH_INTR) & nv_ld(NV_PGRAPH_INTR_EN)) {
        v |= PMC_INTR_PGRAPH;
    }
    if (nv_ld(NV_PTIMER_INTR_0) & nv_ld(NV_PTIMER_INTR_EN_0) & 1) {
        v |= PMC_INTR_PTIMER;
    }
    return v;
}

uint32_t hw_irq_lines(const Kernel *k)
{
    (void)k;
    uint32_t lines = 0;
    if ((nv_ld(NV_PMC_INTR_EN_0) & 1) && pmc_intr()) {
        lines |= 1u << HW_IRQ_NV2A;
    }
    if (aci_irq()) {
        lines |= 1u << HW_IRQ_ACI;
    }
    return lines;
}

static uint32_t pramin(uint32_t off) { return off + 4 <= NV_PRAMIN_SIZE ? nv_ld(NV_PRAMIN + off) : 0; }

/* Un objeto en la tabla hash del canal (RAMHT): su instancia en PRAMIN, o 0. */
static uint32_t ramht_find(uint32_t handle)
{
    uint32_t ht = nv_ld(NV_PFIFO_RAMHT);
    uint32_t base = (ht & 0x1F0u) << 8;
    uint32_t size = 1u << (((ht >> 16) & 3) + 12);
    unsigned bits = (unsigned)__builtin_ctz(size) - 1;
    uint32_t hash = 0;
    for (uint32_t h = handle; h; h >>= bits) {
        hash ^= h & ((1u << bits) - 1);
    }
    hash ^= (nv_ld(NV_PFIFO_CACHE1_PUSH1) & 0x1Fu) << (bits - 4);
    uint32_t e = base + 8 * (hash & (size / 8 - 1));
    if (pramin(e) != handle || !(pramin(e + 4) & 0x80000000u)) {
        return 0;
    }
    return (pramin(e + 4) & 0xFFFFu) << 4;
}

static uint32_t dma_base(uint32_t inst)
{
    return ((pramin(inst + 8) & 0xFFFFF000u) | (pramin(inst) >> 20)) & 0x07FFFFFFu;
}

static uint32_t surf_get(uint32_t mask) { return (nv_ld(NV_PGRAPH_SURFACE) & mask) >> __builtin_ctz(mask); }

static void surf_set(uint32_t mask, uint32_t v)
{
    nv_st(NV_PGRAPH_SURFACE, (nv_ld(NV_PGRAPH_SURFACE) & ~mask) | ((v << __builtin_ctz(mask)) & mask));
}

/* memoria física del Xbox */
static uint32_t *phys32(Kernel *k, uint32_t pa)
{
    pa &= VM_RAM_SIZE - 1;
    return pa + 4 <= VM_RAM_SIZE ? (uint32_t *)(k->vm->ram + pa) : NULL;
}

/* Un método del push buffer. Devuelve PB_GO o el motivo de parar. */
static int pb_method(Kernel *k, unsigned subch, uint32_t m, uint32_t v)
{
    if (m == 0) { /* SET_OBJECT */
        uint32_t inst = ramht_find(v);
        g_hw.obj_inst[subch] = inst;
        g_hw.obj_class[subch] = inst ? (uint8_t)pramin(inst) : 0;
        return PB_GO;
    }
    if (m == 0x0050) { /* SET_REFERENCE */
        nv_st(NV_PFIFO_CACHE1_REF, v);
        return PB_GO;
    }
    if (m == 0x0100) { /* NO_OPERATION: con argumento, una trampa para el ISR de D3D */
        if (!v) {
            return PB_GO;
        }
        nv_st(NV_PGRAPH_TRAPPED_ADDR, subch << 16 | m);
        nv_st(NV_PGRAPH_TRAPPED_DATA, v);
        nv_st(NV_PGRAPH_NSOURCE, 1);
        nv_st(NV_PGRAPH_INTR, nv_ld(NV_PGRAPH_INTR) | (1u << 20));
        g_hw.st.traps++;
        return PB_TRAP;
    }
    if (g_hw.obj_class[subch] != 0x97) {
        return PB_GO;
    }
    switch (m) { /* Kelvin */
    case NV097_SET_FLIP_READ: surf_set(SURF_READ, v); break;
    case NV097_SET_FLIP_WRITE: surf_set(SURF_WRITE, v); break;
    case NV097_SET_FLIP_MODULO: surf_set(SURF_MODULO, v); break;
    case NV097_FLIP_INCREMENT_WRITE: {
        uint32_t mod = surf_get(SURF_MODULO);
        surf_set(SURF_WRITE, mod ? (surf_get(SURF_WRITE) + 1) % mod : 0);
        g_hw.st.flips++;
        break;
    }
    case NV097_FLIP_STALL:
        if (surf_get(SURF_READ) == surf_get(SURF_WRITE)) {
            return PB_FLIP; /* hasta que el ISR del vblank haga el flip */
        }
        break;
    case NV097_SET_CONTEXT_DMA_SEMAPHORE: g_hw.sem_inst = ramht_find(v); break;
    case NV097_SET_CONTEXT_DMA_REPORT: g_hw.report_inst = ramht_find(v); break;
    case NV097_SET_SEMAPHORE_OFFSET: g_hw.sem_offset = v; break;
    case NV097_SEMAPHORE_RELEASE:
        if (g_hw.sem_inst) {
            uint32_t *p = phys32(k, dma_base(g_hw.sem_inst) + g_hw.sem_offset);
            if (p) {
                *p = v;
                g_hw.st.sem_releases++;
            }
        }
        break;
    case NV097_GET_REPORT: /* sin GPU todavía: ningún píxel pasó, informe listo */
        if (g_hw.report_inst) {
            uint32_t pa = dma_base(g_hw.report_inst) + (v & 0x00FFFFFFu);
            uint32_t *p = phys32(k, pa);
            if (p && phys32(k, pa + 12)) {
                uint64_t ts = k->now_ns;
                p[0] = (uint32_t)ts;
                p[1] = (uint32_t)(ts >> 32);
                p[2] = 0;
                p[3] = 0;
            }
        }
        break;
    default:
        break;
    }
    return PB_GO;
}

/* Recorre el push buffer de GET a PUT (o hasta tener que esperar). */
static void pb_run(Kernel *k)
{
    uint32_t inst = (nv_ld(NV_PFIFO_CACHE1_DMA_INSTANCE) & 0xFFFFu) << 4;
    uint32_t base = 0, limit = 0x07FFFFFFu;
    if (inst && inst + 12 <= NV_PRAMIN_SIZE) {
        base = dma_base(inst);
        limit = pramin(inst + 4);
    }
    uint32_t get = nv_ld(NV_PFIFO_CACHE1_DMA_GET), put = nv_ld(NV_PFIFO_CACHE1_DMA_PUT);
    unsigned branches = 0;
    g_hw.stall = PB_GO;
    for (unsigned budget = 0; get != put && budget < 4000000; budget++) {
        if (!(nv_ld(NV_PGRAPH_FIFO) & 1) || (nv_ld(NV_PGRAPH_INTR) & (1u << 20))) {
            g_hw.stall = (nv_ld(NV_PGRAPH_INTR) & (1u << 20)) ? PB_TRAP : PB_FIFO;
            break;
        }
        if (get > limit) {
            g_hw.stall = PB_ERROR;
            break;
        }
        uint32_t *w = phys32(k, base + get);
        if (!w) {
            g_hw.stall = PB_ERROR;
            break;
        }
        if (g_hw.left) { /* datos del paquete */
            int r = pb_method(k, g_hw.subch, g_hw.method, *w);
            if (r == PB_FLIP) {
                g_hw.stall = r; /* el método se repite cuando haya flip */
                break;
            }
            get += 4;
            g_hw.left--;
            g_hw.st.pb_words++;
            if (!g_hw.noninc) {
                g_hw.method += 4;
            }
            if (r != PB_GO) {
                g_hw.stall = r;
                break;
            }
            continue;
        }
        uint32_t cmd = *w;
        g_hw.st.pb_words++;
        if ((cmd & 0xE0000003u) == 0x20000000u) { /* salto antiguo */
            get = cmd & 0x1FFFFFFCu;
        } else if ((cmd & 3) == 1) { /* salto */
            get = cmd & ~3u;
        } else if ((cmd & 3) == 2) { /* llamada */
            g_hw.sub_active = true;
            g_hw.sub_return = get + 4;
            nv_st(NV_PFIFO_CACHE1_DMA_SUBROUTINE, g_hw.sub_return | 1);
            get = cmd & ~3u;
        } else if (cmd == 0x00020000u) { /* vuelta */
            g_hw.sub_active = false;
            nv_st(NV_PFIFO_CACHE1_DMA_SUBROUTINE, g_hw.sub_return);
            get = g_hw.sub_return;
        } else if ((cmd & 0xE0030003u) == 0 || (cmd & 0xE0030003u) == 0x40000000u) {
            g_hw.method = cmd & 0x1FFCu;
            g_hw.subch = (uint8_t)((cmd >> 13) & 7);
            g_hw.left = (cmd >> 18) & 0x7FFu;
            g_hw.noninc = (cmd & 0x40000000u) != 0;
            get += 4;
            branches = 0;
            continue;
        } else {
            g_hw.stall = PB_ERROR;
            break;
        }
        if (++branches > 65536) {
            g_hw.stall = PB_ERROR;
            break;
        }
    }
    nv_st(NV_PFIFO_CACHE1_DMA_GET, get);
    g_hw.st.pb_stall = (unsigned)g_hw.stall;
}

static uint32_t nv_read(Kernel *k, uint32_t off, int size)
{
    uint32_t word = off & ~3u, v;
    switch (word) {
    case NV_PMC_INTR_0: v = pmc_intr(); break;
    case NV_PFIFO_RUNOUT_STATUS:
    case NV_PFIFO_CACHE1_STATUS:
        v = nv_ld(NV_PFIFO_CACHE1_DMA_GET) == nv_ld(NV_PFIFO_CACHE1_DMA_PUT) ? 0x10 : 0; /* vacía */
        break;
    case NV_PFIFO_CACHE1_DMA_PUSH:
        v = nv_ld(word) & ~0x10u; /* nunca ocupada */
        break;
    case NV_USER_DMA_PUT:
    case NV_USER_DMA_GET:
    case NV_USER_REF:
        v = nv_ld(word - NV_USER_DMA_PUT + NV_PFIFO_CACHE1_DMA_PUT);
        break;
    case NV_PFB_WBC: v = 0; break;
    case NV_PTIMER_TIME_0: v = (uint32_t)k->now_ns & ~0x1Fu; break;
    case NV_PTIMER_TIME_1: v = (uint32_t)(k->now_ns >> 32) & 0x1FFFFFFFu; break;
    case NV_PCRTC_RASTER: {
        uint64_t since = k->now_ns + FRAME_NS - g_hw.next_vblank;
        v = (uint32_t)((since % FRAME_NS) * LINES / FRAME_NS);
        break;
    }
    default: v = nv_ld(word); break;
    }
    unsigned shift = (off & 3) * 8;
    return size == 4 ? v : (v >> shift) & (size == 1 ? 0xFFu : 0xFFFFu);
}

static void nv_write(Kernel *k, uint32_t off, int size, uint32_t value)
{
    if (size != 4) {
        uint32_t word = off & ~3u, shift = (off & 3) * 8, mask = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
        nv_st(word, (nv_ld(word) & ~mask) | ((value << shift) & mask));
        return;
    }
    switch (off) {
    case NV_PMC_INTR_0:
        return; /* resumen de las unidades */
    case NV_PCRTC_INTR_0:
    case NV_PTIMER_INTR_0:
        nv_st(off, nv_ld(off) & ~value); /* 1 borra */
        return;
    case NV_PGRAPH_INTR:
        nv_st(off, nv_ld(off) & ~value);
        pb_run(k); /* el ISR atendió la trampa: sigue */
        return;
    case NV_PTIMER_ALARM_0: {
        nv_st(off, value);
        uint32_t d = value - ((uint32_t)k->now_ns & ~0x1Fu);
        g_hw.alarm_due = d >= (uint32_t)-1000000000 ? k->now_ns : k->now_ns + d;
        g_hw.alarm_on = true;
        return;
    }
    case NV_PGRAPH_INCREMENT:
        if (value & 2) { /* el flip del ISR del vblank: avanza la lectura */
            uint32_t mod = surf_get(SURF_MODULO);
            surf_set(SURF_READ, mod ? (surf_get(SURF_READ) + 1) % mod : 0);
            pb_run(k);
        }
        return;
    case NV_PGRAPH_FIFO:
        nv_st(off, value);
        if (value & 1) {
            pb_run(k);
        }
        return;
    case NV_USER_DMA_PUT:
    case NV_USER_DMA_GET:
    case NV_USER_REF:
        nv_st(off - NV_USER_DMA_PUT + NV_PFIFO_CACHE1_DMA_PUT, value);
        if (off == NV_USER_DMA_GET) {
            g_hw.left = 0;
            g_hw.sub_active = false;
        }
        if (off == NV_USER_DMA_PUT) {
            pb_run(k);
        }
        return;
    case NV_PVIDEO_STOP:
        nv_st(off, value);
        if (value & 1) {
            nv_st(NV_PVIDEO_BUFFER, 0);
        }
        return;
    default:
        nv_st(off, value);
        return;
    }
}

/* ================================================================ USB (OHCI) */

static uint32_t ohci_read(Kernel *k, Ohci *o, uint32_t off)
{
    if (off == 0x3C) { /* HcFmNumber: un cuadro por milisegundo */
        return (uint32_t)(k->now_ns / 1000000u) & 0xFFFF;
    }
    if (off == 0x38) { /* HcFmRemaining */
        return 0x2EDF - (uint32_t)((k->now_ns % 1000000u) * 0x2EDF / 1000000u);
    }
    return off < sizeof(o->r) ? o->r[off / 4] : 0;
}

static void ohci_write(Ohci *o, uint32_t off, uint32_t v)
{
    if (off >= sizeof(o->r)) {
        return;
    }
    switch (off) {
    case 0x00: return; /* HcRevision */
    case 0x08: /* HcCommandStatus: el reinicio termina al instante */
        if (v & 1) {
            uint32_t a = o->r[0x48 / 4];
            memset(o->r, 0, sizeof(o->r));
            o->r[0x00 / 4] = 0x10;
            o->r[0x34 / 4] = 0x27782EDF;
            o->r[0x48 / 4] = a;
        }
        o->r[off / 4] |= v & ~1u;
        return;
    case 0x0C: o->r[off / 4] &= ~v; return; /* HcInterruptStatus: 1 borra */
    case 0x10: o->r[0x10 / 4] |= v; o->r[0x14 / 4] = o->r[0x10 / 4]; return; /* enable */
    case 0x14: o->r[0x10 / 4] &= ~v; o->r[0x14 / 4] = o->r[0x10 / 4]; return; /* disable */
    default: o->r[off / 4] = v; return;
    }
}

/* ================================================================ APU (mínimo) */

/* Lo justo para que DirectSound arranque (la etapa 3f trae el APU entero,
 * el de xemu que usa hakuX): la cola de métodos del VP siempre vacía, el
 * contador de muestras a 48 kHz, la escritura "mágica" de notificaciones y
 * las interrupciones que se borran escribiendo 1. */
#define APU_ISTS      0x1000u
#define APU_FEMEMADDR 0x1324u
#define APU_FEMEMDATA 0x1334u
#define APU_XGSCNT    0x200Cu
#define APU_VP        0x20000u
#define APU_PIO_FREE  0x10u

static uint32_t buf_rd(const uint8_t *b, uint32_t off, int size);
static void buf_wr(uint8_t *b, uint32_t off, int size, uint32_t v);

static uint32_t apu_read(Kernel *k, uint32_t off, int size)
{
    if (off == APU_VP + APU_PIO_FREE) {
        return 0x80;
    }
    if (off == APU_XGSCNT) {
        return (uint32_t)(k->now_ns * 6 / 125000) - buf_rd(g_hw.apu, off, 4); /* 48000 / 1e9 */
    }
    return buf_rd(g_hw.apu, off, size);
}

static void apu_write(Kernel *k, uint32_t off, int size, uint32_t v)
{
    switch (off) {
    case APU_ISTS:
        buf_wr(g_hw.apu, off, 4, buf_rd(g_hw.apu, off, 4) & ~v);
        return;
    case APU_FEMEMDATA: { /* la notificación se escribe en FEMEMADDR al momento */
        uint32_t *p = phys32(k, buf_rd(g_hw.apu, APU_FEMEMADDR, 4));
        if (p) {
            *p = v;
        }
        buf_wr(g_hw.apu, off, 4, v);
        return;
    }
    case APU_XGSCNT:
        buf_wr(g_hw.apu, off, 4, (uint32_t)(k->now_ns * 6 / 125000) - v);
        return;
    default:
        buf_wr(g_hw.apu, off, size, v);
        return;
    }
}

/* ================================================================ acceso */

static uint32_t buf_rd(const uint8_t *b, uint32_t off, int size)
{
    uint32_t v = 0;
    memcpy(&v, b + off, (size_t)size);
    return v;
}

static void buf_wr(uint8_t *b, uint32_t off, int size, uint32_t v) { memcpy(b + off, &v, (size_t)size); }

bool hw_mmio(void *kernel, uint32_t addr, int size, bool write, uint32_t *value)
{
    Kernel *k = kernel;
    if (addr < NV2A_BASE || !g_hw.nv) {
        return false;
    }
    g_hw.st.accesses++;
    g_hw.st.last_addr = addr;
    g_hw.st.last_write = write;
    if (addr - NV2A_BASE + (uint32_t)size <= NV2A_SIZE) {
        uint32_t off = addr - NV2A_BASE;
        if (write) {
            nv_write(k, off, size, *value);
        } else {
            *value = nv_read(k, off, size);
        }
    } else if (addr >= APU_BASE && addr - APU_BASE + (uint32_t)size <= APU_SIZE) {
        if (write) {
            apu_write(k, addr - APU_BASE, size, *value);
        } else {
            *value = apu_read(k, addr - APU_BASE, size);
        }
    } else if (addr >= ACI_BASE && addr - ACI_BASE + (uint32_t)size <= ACI_SIZE) {
        if (write) {
            aci_write(k, addr - ACI_BASE, size, *value);
        } else {
            *value = aci_read(k, addr - ACI_BASE, size);
        }
    } else if (addr >= USB0_BASE && addr < USB1_BASE + USB_SIZE &&
               ((addr - USB0_BASE) & 0x7FFF) + (uint32_t)size <= USB_SIZE) {
        Ohci *o = &g_hw.usb[addr >= USB1_BASE];
        uint32_t off = (addr - USB0_BASE) & 0xFFC;
        if (write) {
            ohci_write(o, off, *value);
        } else {
            *value = ohci_read(k, o, off);
        }
    } else if (addr >= NIC_BASE && addr - NIC_BASE + (uint32_t)size <= NIC_SIZE) {
        if (write) {
            buf_wr(g_hw.nic, addr - NIC_BASE, size, *value);
        } else {
            *value = buf_rd(g_hw.nic, addr - NIC_BASE, size);
        }
    } else if (addr >= FLASH_BASE) {
        if (!write) {
            *value = 0; /* la flash: nada que leer */
        }
    } else {
        return false;
    }
    g_hw.st.last_value = *value;
    return true;
}

/* ================================================================ puertos */

#define ACPI_PM_TIMER   0x8008u
#define MCPX_GPIO_FIELD 0x80C0u

uint32_t hw_port_in(void *kernel, uint16_t port, int size)
{
    Kernel *k = kernel;
    uint32_t ones = size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
    switch (port) {
    case ACPI_PM_TIMER: /* 3,375 MHz */
        return (uint32_t)((unsigned __int128)k->now_ns * 3375000u / 1000000000u);
    case MCPX_GPIO_FIELD: /* bit 5: el campo de video, alterno en cada vblank */
        return (ones & ~0x20u) | ((g_hw.st.vblanks & 1) ? 0x20u : 0);
    default:
        return ones;
    }
}

void hw_port_out(void *kernel, uint16_t port, int size, uint32_t value)
{
    (void)kernel;
    (void)port;
    (void)size;
    (void)value;
}

/* ================================================================ tiempo */

void hw_tick(Kernel *k)
{
    if (!g_hw.nv) {
        return;
    }
    if (k->now_ns >= g_hw.next_vblank) {
        uint64_t missed = (k->now_ns - g_hw.next_vblank) / FRAME_NS;
        g_hw.next_vblank += (missed + 1) * FRAME_NS;
        g_hw.st.vblanks++;
        nv_st(NV_PCRTC_INTR_0, nv_ld(NV_PCRTC_INTR_0) | 1);
    }
    if (g_hw.alarm_on && k->now_ns >= g_hw.alarm_due) {
        nv_st(NV_PTIMER_INTR_0, nv_ld(NV_PTIMER_INTR_0) | 1);
        g_hw.alarm_due += 1ull << 32;
    }
    aci_tick(k);
}

bool hw_next_event(const Kernel *k, uint64_t *ns)
{
    (void)k;
    if (!g_hw.nv) {
        return false;
    }
    uint64_t t = g_hw.next_vblank;
    if (g_hw.alarm_on && (nv_ld(NV_PTIMER_INTR_EN_0) & 1) && g_hw.alarm_due < t) {
        t = g_hw.alarm_due;
    }
    *ns = t;
    return true;
}
