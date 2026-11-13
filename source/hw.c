/*
 * XboxNX — hardware del Xbox simulado (etapa 3e). Ver hw.h.
 */
#include "hw.h"
#include "nv2a_sw.h"
#include "gpu.h"
#include "prof.h"
#include "usb/usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aci.h"
#include "apu/apu.h"
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
#define NV097_SET_CONTEXT_DMA_COLOR     0x0194u
#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200u
#define NV097_SET_SURFACE_CLIP_VERTICAL 0x0204u
#define NV097_SET_SURFACE_FORMAT        0x0208u
#define NV097_SET_SURFACE_PITCH         0x020Cu
#define NV097_SET_SURFACE_COLOR_OFFSET  0x0210u
#define NV097_SET_COLOR_CLEAR_VALUE     0x1D90u
#define NV097_CLEAR_SURFACE             0x1D94u
#define NV097_SET_CLEAR_RECT_HORIZONTAL 0x1D98u
#define NV097_SET_CLEAR_RECT_VERTICAL   0x1D9Cu
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760u /* 16: tipo 3:0, tamaño 7:4, paso 31:8 */
#define NV097_SET_BEGIN_END        0x17FCu
#define NV097_ARRAY_ELEMENT16      0x1800u
#define NV097_ARRAY_ELEMENT32      0x1808u
#define NV097_DRAW_ARRAYS          0x1810u
#define NV097_INLINE_ARRAY         0x1818u
#define NV097_SET_VERTEX4F_W       0x1524u
#define NV097_SET_TEXTURE_FORMAT   0x1B04u /* + 0x40 por etapa */
#define NV097_SET_TEXTURE_CONTROL0 0x1B0Cu
#define NV097_SET_TRANSFORM_EXECUTION_MODE 0x1E94u

enum { PB_GO, PB_FLIP, PB_TRAP, PB_FIFO, PB_ERROR };

typedef struct Hw {
    uint8_t *nv;          /* 16 MB de registros del NV2A */
    uint8_t nic[NIC_SIZE];
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
    /* la superficie de color (para borrarla por software) */
    uint32_t color_inst, surf_format, surf_pitch, color_offset, clip_h, clip_v;
    uint32_t last_target;   /* el destino de color del último cuadro terminado */
    uint32_t shown;         /* lo que se mostró la última vez (hw_screen) */
    uint32_t clear_value, clear_h, clear_v;
    /* lo que se ve: AvSetDisplayMode y PCRTC_START */
    uint32_t av_format, av_pitch, av_fb;
    /* lo que se dibuja (para saber qué hace falta en la etapa 4) */
    uint32_t va_format[16], tex_format[4], tex_control[4], xform_mode, prim;
    uint32_t inline_words;
    HwDraws cur;
    int stall;
    HwStats st;
} Hw;

static Hw g_hw;
static uint32_t dma_of_handle(uint32_t handle);

static uint32_t nv_ld(uint32_t off) { return *(uint32_t *)(g_hw.nv + off); }
static void nv_st(uint32_t off, uint32_t v) { *(uint32_t *)(g_hw.nv + off) = v; }

bool hw_init(Kernel *k)
{
    if (!g_hw.nv) {
        g_hw.nv = calloc(1, NV2A_SIZE);
    } else {
        memset(g_hw.nv, 0, NV2A_SIZE);
    }
    if (!g_hw.nv || !apu_init(k)) {
        return false;
    }
    uint8_t *nv = g_hw.nv;
    memset(&g_hw, 0, sizeof(g_hw));
    g_hw.nv = nv;
    sw_init(k->vm->ram, VM_RAM_SIZE, dma_of_handle);
    nv_st(NV_PMC_BOOT_0, 0x02A000A1u);
    nv_st(NV_PFB_CSTATUS, 64u << 20);
    nv_st(NV_PGRAPH_FIFO, 1);
    g_hw.next_vblank = FRAME_NS;
    aci_init(k);
    usb_init(k->vm->ram, VM_RAM_SIZE);
    return true;
}

const HwStats *hw_stats(void) { return &g_hw.st; }

static uint32_t pmc_intr(void);

/* Para la pantalla: el estado de las interrupciones del GPU. */
void hw_irq_debug(char *b, size_t n)
{
    snprintf(b, n, "PGRAPH intr %X en %X · PMC %X en %X · trampa %X: %X (%u) · PCRTC %X en %X",
             (unsigned)nv_ld(NV_PGRAPH_INTR), (unsigned)nv_ld(NV_PGRAPH_INTR_EN), (unsigned)pmc_intr(),
             (unsigned)nv_ld(NV_PMC_INTR_EN_0), (unsigned)nv_ld(NV_PGRAPH_TRAPPED_ADDR),
             (unsigned)nv_ld(NV_PGRAPH_TRAPPED_DATA), (unsigned)g_hw.st.traps, (unsigned)nv_ld(NV_PCRTC_INTR_0),
             (unsigned)nv_ld(NV_PCRTC_INTR_EN_0));
}

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
    if (apu_irq()) {
        lines |= 1u << HW_IRQ_APU;
    }
    if (usb_irq()) {
        lines |= 1u << HW_IRQ_USB0;
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

static uint32_t dma_of_handle(uint32_t handle)
{
    uint32_t inst = ramht_find(handle);
    return inst ? dma_base(inst) : 0;
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

/* CLEAR_SURFACE por software: el rectángulo de la superficie de color
 * (lineal, de 16 o 32 bits) con el color de borrado. Sin dibujo todavía,
 * es lo primero que se ve de un juego. */
static bool g_skipping; /* el cuadro en curso no se dibuja (saltar cuadros) */

static void clear_color(Kernel *k, uint32_t flags)
{
    if (!(flags & 0xF0u) || !g_hw.color_inst || g_skipping) {
        return;
    }
    unsigned fmt = g_hw.surf_format & 0xF, type = (g_hw.surf_format >> 8) & 0xF;
    unsigned aa = (g_hw.surf_format >> 12) & 0xF;
    if (type != 1) {
        return; /* swizzle: más adelante */
    }
    unsigned bpp = fmt == 1 || fmt == 2 || fmt == 3 ? 2 : (fmt >= 4 && fmt <= 8) ? 4 : 0;
    uint32_t pitch = g_hw.surf_pitch & 0xFFFF;
    if (!bpp || !pitch) {
        return;
    }
    unsigned fx = aa ? 2 : 1, fy = aa == 2 ? 2 : 1;
    uint32_t x1 = (g_hw.clear_h & 0xFFFF) * fx, x2 = ((g_hw.clear_h >> 16) + 1) * fx;
    uint32_t y1 = (g_hw.clear_v & 0xFFFF) * fy, y2 = ((g_hw.clear_v >> 16) + 1) * fy;
    uint32_t cw = ((g_hw.clip_h >> 16) ? (g_hw.clip_h >> 16) : 4096) * fx;
    uint32_t ch = ((g_hw.clip_v >> 16) ? (g_hw.clip_v >> 16) : 4096) * fy;
    if (x2 > cw) x2 = cw;
    if (y2 > ch) y2 = ch;
    if (x2 > pitch / bpp) x2 = pitch / bpp;
    if (x1 >= x2 || y1 >= y2) {
        return;
    }
    uint32_t mask = 0;
    if (bpp == 4) {
        mask = (flags & 0x10 ? 0x00FF0000u : 0) | (flags & 0x20 ? 0x0000FF00u : 0) |
               (flags & 0x40 ? 0x000000FFu : 0) | (flags & 0x80 ? 0xFF000000u : 0);
    } else {
        mask = (flags & 0x10 ? 0xF800u : 0) | (flags & 0x20 ? 0x07E0u : 0) | (flags & 0x40 ? 0x001Fu : 0);
    }
    uint32_t base = dma_base(g_hw.color_inst) + g_hw.color_offset;
    uint32_t val = g_hw.clear_value;
    if (bpp == 4 && gpu_enabled()) { /* 4c: la superficie vive en la GPU */
        unsigned m4 = (flags & 0x10 ? 4u : 0) | (flags & 0x20 ? 2u : 0) | (flags & 0x40 ? 1u : 0) | (flags & 0x80 ? 8u : 0);
        if (gpu_clear_color(base, pitch, (int)y2, (int)x1, (int)y1, (int)x2, (int)y2, val, m4)) {
            g_hw.st.clears++;
            return;
        }
    }
    for (uint32_t y = y1; y < y2; y++) {
        uint32_t row = base + y * pitch;
        if (!phys32(k, row + x2 * bpp - 4)) {
            break;
        }
        uint8_t *p = k->vm->ram + ((row + x1 * bpp) & (VM_RAM_SIZE - 1));
        if (bpp == 4) {
            uint32_t *q = (uint32_t *)p;
            for (uint32_t x = x1; x < x2; x++, q++) {
                *q = (*q & ~mask) | (val & mask);
            }
        } else {
            uint16_t *q = (uint16_t *)p;
            for (uint32_t x = x1; x < x2; x++, q++) {
                *q = (uint16_t)((*q & ~mask) | (val & mask));
            }
        }
    }
    g_hw.st.clears++;
}

/* Bytes por vértice de INLINE_ARRAY: los atributos con tamaño (tipos del
 * NV2A: 0 UB_D3D y 4 UB_OGL de 1 byte, 1 S1 y 5 S32K de 2, 2 F de 4, 6 CMP
 * empaquetado en 4). */
static unsigned inline_stride(void)
{
    unsigned bytes = 0;
    for (int i = 0; i < 16; i++) {
        unsigned type = g_hw.va_format[i] & 0xF, size = (g_hw.va_format[i] >> 4) & 0xF;
        if (!size) {
            continue;
        }
        unsigned ts = type == 2 ? 4 : (type == 1 || type == 5) ? 2 : 1;
        bytes += type == 6 ? 4 : size * ts;
    }
    return bytes;
}

/* SET_BEGIN_END y los vértices: cuenta lo que el juego dibuja en el cuadro. */
static void draw_method(uint32_t m, uint32_t v)
{
    HwDraws *d = &g_hw.cur;
    switch (m) {
    case NV097_SET_BEGIN_END:
        if (v) {
            g_hw.prim = v;
            g_hw.inline_words = 0;
            d->draws++;
            d->prims |= 1u << (v & 15);
            if ((g_hw.xform_mode & 3) == 2) {
                d->program++;
            }
            unsigned n = 0;
            for (int i = 0; i < 4; i++) {
                if (g_hw.tex_control[i] & (1u << 30)) {
                    n++;
                    uint8_t f = (uint8_t)(g_hw.tex_format[i] >> 8);
                    bool seen = false;
                    for (unsigned j = 0; j < d->nformats; j++) {
                        seen |= d->formats[j] == f;
                    }
                    if (!seen && d->nformats < 8) {
                        d->formats[d->nformats++] = f;
                    }
                }
            }
            if (n) {
                d->textured++;
            }
        } else if (g_hw.inline_words) {
            unsigned st = inline_stride();
            d->vertices += st ? g_hw.inline_words * 4 / st : 0;
            g_hw.inline_words = 0;
        }
        break;
    case NV097_ARRAY_ELEMENT16: d->vertices += 2; break;
    case NV097_ARRAY_ELEMENT32: d->vertices += 1; break;
    case NV097_DRAW_ARRAYS: d->vertices += (v >> 24) + 1; break;
    case NV097_INLINE_ARRAY: g_hw.inline_words++; break;
    case NV097_SET_VERTEX4F_W: d->vertices++; d->immediate++; break;
    case NV097_SET_TRANSFORM_EXECUTION_MODE: g_hw.xform_mode = v; break;
    default:
        if (m >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT && m < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 64) {
            g_hw.va_format[(m - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4] = v;
        } else if (m >= NV097_SET_TEXTURE_FORMAT && m < NV097_SET_TEXTURE_FORMAT + 0x100 && !((m - NV097_SET_TEXTURE_FORMAT) & 0x3F)) {
            g_hw.tex_format[(m - NV097_SET_TEXTURE_FORMAT) / 0x40] = v;
        } else if (m >= NV097_SET_TEXTURE_CONTROL0 && m < NV097_SET_TEXTURE_CONTROL0 + 0x100 &&
                   !((m - NV097_SET_TEXTURE_CONTROL0) & 0x3F)) {
            g_hw.tex_control[(m - NV097_SET_TEXTURE_CONTROL0) / 0x40] = v;
        }
        break;
    }
}

static unsigned g_frameskip = 3, g_frame_no;
static uint32_t g_last_drawn; /* el destino del último cuadro que sí se dibujó */
void hw_set_frameskip(unsigned n)
{
    g_frameskip = n ? n : 1;
    if (g_frameskip == 1) {
        g_skipping = false;
        sw_set_skip(false);
    }
}
unsigned hw_frameskip(void) { return g_frameskip; }

/* ---- captura de un cuadro (para repetirlo en la PC con tools/replay.c) ---- */

typedef struct { uint32_t m, v, base; } CapRec;
static struct {
    int state;          /* 0 nada, 1 esperando el flip, 2 capturando, 3 listo, -1 error */
    char dir[160];
    uint8_t *ram;       /* la RAM al empezar el cuadro */
    CapRec *rec;
    size_t n, cap;
} g_cap;

void hw_capture_arm(const char *dir)
{
    if (g_cap.state == 1 || g_cap.state == 2) {
        return;
    }
    snprintf(g_cap.dir, sizeof(g_cap.dir), "%s", dir);
    g_cap.state = 1;
}

int hw_capture_state(void) { return g_cap.state; }

static void cap_write(const char *name, const void *p, size_t n)
{
    char path[200];
    snprintf(path, sizeof(path), "%s/%s", g_cap.dir, name);
    remove(path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        g_cap.state = -1;
        return;
    }
    if (fwrite(p, 1, n, f) != n) {
        g_cap.state = -1;
    }
    fclose(f);
}

/* En cada flip: empezar (con la RAM de ese momento) o terminar y guardar. */
static void cap_flip(Kernel *k)
{
    if (g_cap.state == 1) {
        g_cap.ram = malloc(VM_RAM_SIZE);
        if (!g_cap.ram) {
            g_cap.state = -1;
            return;
        }
        memcpy(g_cap.ram, k->vm->ram, VM_RAM_SIZE);
        g_cap.n = 0;
        g_cap.state = 2;
        /* el estado de Kelvin al empezar (lo que el cuadro no vuelve a poner) */
        size_t ss = sw_state_size();
        void *st = malloc(ss);
        if (st) {
            sw_state_get(st);
            cap_write("cuadro_estado.bin", st, ss);
            free(st);
            if (g_cap.state != 2) {
                free(g_cap.ram);
                g_cap.ram = NULL;
            }
        }
    } else if (g_cap.state == 2) {
        g_cap.state = 3;
        cap_write("cuadro_ram.bin", g_cap.ram, VM_RAM_SIZE);
        if (g_cap.state == 3) {
            cap_write("cuadro.bin", g_cap.rec, g_cap.n * sizeof(CapRec));
        }
        free(g_cap.ram);
        g_cap.ram = NULL;
        free(g_cap.rec);
        g_cap.rec = NULL;
        g_cap.cap = 0;
    }
}

static void cap_method(uint32_t m, uint32_t v)
{
    if (g_cap.state != 2) {
        return;
    }
    if (g_cap.n == g_cap.cap) {
        size_t nc = g_cap.cap ? g_cap.cap * 2 : 65536;
        if (nc > (16u << 20)) {
            return;
        }
        CapRec *n = realloc(g_cap.rec, nc * sizeof(CapRec));
        if (!n) {
            return;
        }
        g_cap.rec = n;
        g_cap.cap = nc;
    }
    uint32_t base = 0;
    if (m == 0x0184 || m == 0x0188 || m == 0x0194 || m == 0x0198 || m == 0x019C || m == 0x01A0) {
        base = dma_of_handle(v);
    }
    g_cap.rec[g_cap.n++] = (CapRec){m, v, base};
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
        k->irq_kick = 1; /* que el ISR de D3D la atienda ya, no al final del turno */
        return PB_TRAP;
    }
    if (g_hw.obj_class[subch] != 0x97) {
        return PB_GO;
    }
    draw_method(m, v);
    cap_method(m, v);
    sw_method(m, v);
    switch (m) { /* Kelvin */
    case NV097_SET_FLIP_READ: surf_set(SURF_READ, v); break;
    case NV097_SET_FLIP_WRITE: surf_set(SURF_WRITE, v); break;
    case NV097_SET_FLIP_MODULO: surf_set(SURF_MODULO, v); break;
    case NV097_FLIP_INCREMENT_WRITE: {
        uint32_t mod = surf_get(SURF_MODULO);
        surf_set(SURF_WRITE, mod ? (surf_get(SURF_WRITE) + 1) % mod : 0);
        g_hw.st.flips++;
        cap_flip(k);
        /* saltar cuadros: dibujar solo 1 de cada g_frameskip (no al capturar) */
        g_frame_no++;
        if (!g_skipping && g_hw.color_inst) {
            g_last_drawn = dma_base(g_hw.color_inst) + g_hw.color_offset;
            gpu_flush_surface_async(g_last_drawn); /* 4c: el cuadro terminado, a la RAM (para la pantalla; sin esperar a la GPU) */
        }
        gpu_end_frame();
        g_skipping = g_frameskip > 1 && g_cap.state != 2 && g_cap.state != 1 && (g_frame_no % g_frameskip) != 0;
        sw_set_skip(g_skipping);
        if (g_hw.color_inst) {
            g_hw.last_target = dma_base(g_hw.color_inst) + g_hw.color_offset;
        }
        g_hw.st.last = g_hw.cur; /* el cuadro terminado */
        g_hw.st.total_draws += g_hw.cur.draws;
        memset(&g_hw.cur, 0, sizeof(g_hw.cur));
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
    case NV097_SET_CONTEXT_DMA_COLOR: g_hw.color_inst = ramht_find(v); break;
    case NV097_SET_SURFACE_CLIP_HORIZONTAL: g_hw.clip_h = v; break;
    case NV097_SET_SURFACE_CLIP_VERTICAL: g_hw.clip_v = v; break;
    case NV097_SET_SURFACE_FORMAT: g_hw.surf_format = v; break;
    case NV097_SET_SURFACE_PITCH: g_hw.surf_pitch = v; break;
    case NV097_SET_SURFACE_COLOR_OFFSET: g_hw.color_offset = v; break;
    case NV097_SET_COLOR_CLEAR_VALUE: g_hw.clear_value = v; break;
    case NV097_SET_CLEAR_RECT_HORIZONTAL: g_hw.clear_h = v; break;
    case NV097_SET_CLEAR_RECT_VERTICAL: g_hw.clear_v = v; break;
    case NV097_CLEAR_SURFACE: clear_color(k, v); break;
    case NV097_SEMAPHORE_RELEASE:
        if (g_hw.sem_inst) {
            uint32_t *p = phys32(k, dma_base(g_hw.sem_inst) + g_hw.sem_offset);
            if (p) {
                *p = v;
                g_hw.st.sem_releases++;
            }
        }
        break;
    case NV097_GET_REPORT: /* los píxeles que pasaron Z (pruebas de visibilidad), informe listo */
        if (g_hw.report_inst) {
            uint32_t pa = dma_base(g_hw.report_inst) + (v & 0x00FFFFFFu);
            uint32_t *p = phys32(k, pa);
            if (p && phys32(k, pa + 12)) {
                /* en los cuadros que no se dibujan (saltar cuadros) no hay
                 * cuenta: la última de ese mismo informe */
                static struct {
                    uint32_t off, n;
                } last[64];
                unsigned slot = ((v & 0x00FFFFFFu) >> 4) & 63;
                uint32_t n;
                if (sw_skipping()) {
                    n = last[slot].off == (v & 0x00FFFFFFu) ? last[slot].n : 0;
                } else {
                    n = sw_zpass_result();
                    last[slot].off = v & 0x00FFFFFFu;
                    last[slot].n = n;
                }
                uint64_t ts = k->now_ns;
                p[0] = (uint32_t)ts;
                p[1] = (uint32_t)(ts >> 32);
                p[2] = n;
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
static void pb_run_(Kernel *k);

static void pb_run(Kernel *k)
{
    prof_push(PROF_GPU);
    ZONE_SET(Z_PB);
    pb_run_(k);
    ZONE_END();
    prof_pop();
}

static void pb_run_(Kernel *k)
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
    case NV_PCRTC_START:
        if (value != nv_ld(off)) {
            g_hw.st.frames++;
        }
        nv_st(off, value);
        return;
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

static uint32_t buf_rd(const uint8_t *b, uint32_t off, int size);
static void buf_wr(uint8_t *b, uint32_t off, int size, uint32_t v);

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
    } else if (addr >= USB0_BASE && addr < USB0_BASE + USB_SIZE) {
        uint32_t off = (addr - USB0_BASE) & 0xFFC;
        if (write) {
            usb_write(off, *value);
        } else {
            *value = usb_read(off) >> (8 * (addr & 3));
        }
    } else if (addr >= USB1_BASE && addr < USB1_BASE + USB_SIZE) {
        if (!write) { /* el segundo controlador: sin nada conectado */
            uint32_t off = addr - USB1_BASE;
            *value = off == 0 ? 0x10 : off == 0x48 ? 0x204 : 0;
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
    {
        HwStats *st = &g_hw.st;
        int i = 0;
        while (i < HW_RECENT - 1 && (st->recent[i].addr != addr || st->recent[i].write != write) &&
               st->recent[i].count) {
            i++;
        }
        uint32_t cnt = st->recent[i].addr == addr && st->recent[i].write == write ? st->recent[i].count : 0;
        memmove(&st->recent[1], &st->recent[0], (size_t)i * sizeof(st->recent[0]));
        st->recent[0].addr = addr;
        st->recent[0].value = *value;
        st->recent[0].write = write;
        st->recent[0].count = cnt + 1;
    }
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
    prof_push(PROF_HW);
    aci_tick(k);
    usb_tick(k->now_ns);
    prof_pop();
    prof_push(PROF_APU);
    apu_tick(k);
    prof_pop();
}

bool hw_next_event(const Kernel *k, uint64_t *ns)
{
    if (!g_hw.nv) {
        return false;
    }
    /* solo los eventos futuros: los ya pasados (el APU en su hilo puede ir
     * atrasado) los atiende hw_tick, y no deben frenar el salto del tiempo */
    uint64_t now = k->now_ns, t = g_hw.next_vblank;
    if (g_hw.alarm_on && (nv_ld(NV_PTIMER_INTR_EN_0) & 1) && g_hw.alarm_due > now && g_hw.alarm_due < t) {
        t = g_hw.alarm_due;
    }
    uint64_t f;
    if (k->irq_obj[HW_IRQ_APU] && apu_next_frame(&f) && f > now && f < t) {
        t = f; /* el APU avisa al terminar sus cuadros */
    }
    if (k->irq_obj[HW_IRQ_USB0] && usb_next_frame(&f) && f > now && f < t) {
        t = f; /* los cuadros USB de 1 ms */
    }
    *ns = t;
    return true;
}

/* ================================================================ pantalla */

void hw_set_display(Kernel *k, uint32_t format, uint32_t pitch, uint32_t fb)
{
    (void)k;
    g_hw.av_format = format;
    g_hw.av_pitch = pitch;
    g_hw.av_fb = fb;
    g_hw.st.av_modes++;
    if (fb) {
        nv_st(NV_PCRTC_START, fb);
    }
}

/* La imagen que el Xbox muestra (la de PCRTC_START con el modo de
 * AvSetDisplayMode), en RGBA. false si todavía no hay modo. */
bool hw_screen(const Kernel *k, XImage *out)
{
    unsigned bpp = g_hw.av_format == 0x10 || g_hw.av_format == 0x11 || g_hw.av_format == 0x1C ? 2 : 4;
    uint32_t pitch = g_hw.av_pitch;
    if (!g_hw.nv || !pitch || !k || !k->vm) {
        return false;
    }
    int w = (int)(pitch / bpp);
    int h = w >= 1920 ? 1080 : w >= 1280 ? 720 : 480;
    if (w > 1920) {
        w = 1920;
    }
    if (!out->px || out->w != w || out->h != h) {
        free(out->px);
        out->px = malloc((size_t)w * h * 4);
        if (!out->px) {
            out->w = out->h = 0;
            return false;
        }
        out->w = w;
        out->h = h;
    }
    uint32_t start = nv_ld(NV_PCRTC_START) & (VM_RAM_SIZE - 1);
    if (g_frameskip > 1 && g_last_drawn) {
        /* saltando cuadros, el búfer de PCRTC puede ser uno sin dibujar */
        start = g_last_drawn & (VM_RAM_SIZE - 1);
    }
    /* Si lo que apunta PCRTC está todo negro (todavía no se pintó o el
     * flip no llegó) y hay un cuadro terminado en otro lado, ese. */
    uint32_t alt = g_hw.last_target & (VM_RAM_SIZE - 1);
    if (alt && alt != start && start + pitch * (uint32_t)h <= VM_RAM_SIZE && alt + pitch * (uint32_t)h <= VM_RAM_SIZE) {
        bool black = true;
        for (int i = 0; i < 257 && black; i++) {
            uint32_t y = (uint32_t)(i * 37) % (uint32_t)h, x = (uint32_t)(i * 101) % (uint32_t)w;
            const uint8_t *q = k->vm->ram + start + y * pitch + x * bpp;
            black = !(q[0] | q[1] | (bpp == 4 ? q[2] : 0));
        }
        if (black) {
            start = alt;
        }
    }
    g_hw.shown = start;
    for (int y = 0; y < h; y++) {
        uint8_t *d = out->px + (size_t)y * w * 4;
        uint32_t row = start + (uint32_t)y * pitch;
        if (row + pitch > VM_RAM_SIZE) {
            memset(d, 0, (size_t)w * 4);
            continue;
        }
        const uint8_t *src = k->vm->ram + row;
        if (bpp == 4) { /* BGRA -> RGBA de a 32 bits */
            uint32_t *d32 = (uint32_t *)d;
            for (int x = 0; x < w; x++) {
                uint32_t v;
                memcpy(&v, src + x * 4, 4);
                d32[x] = ((v >> 16) & 0xFFu) | (v & 0xFF00u) | ((v & 0xFFu) << 16) | 0xFF000000u;
            }
            continue;
        }
        for (int x = 0; x < w; x++, d += 4) {
            if (bpp == 4) {
                d[0] = src[x * 4 + 2];
                d[1] = src[x * 4 + 1];
                d[2] = src[x * 4];
            } else {
                uint16_t p = (uint16_t)(src[x * 2] | src[x * 2 + 1] << 8);
                if (g_hw.av_format == 0x11) {
                    d[0] = (uint8_t)((p >> 11) * 255 / 31);
                    d[1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
                } else {
                    d[0] = (uint8_t)(((p >> 10) & 31) * 255 / 31);
                    d[1] = (uint8_t)(((p >> 5) & 31) * 255 / 31);
                }
                d[2] = (uint8_t)((p & 31) * 255 / 31);
            }
            d[3] = 255;
        }
    }
    return true;
}

uint32_t hw_scanout(void) { return g_hw.nv ? nv_ld(NV_PCRTC_START) : 0; }
uint32_t hw_shown(void) { return g_hw.shown; }
uint32_t hw_last_target(void) { return g_hw.last_target; }
