/*
 * XboxNX (de hakuX, GPL-3) — el primer controlador USB OHCI del MCPX
 * (OpenHCI 1.0a): registros en 0xFED00000, IRQ 1, cuatro puertos raíz.
 *
 * La pila USB del propio juego (XAPI) lo maneja. Cada cuadro de 1 ms (de
 * tiempo virtual, en usb_tick) hace la lista periódica de la tabla de la
 * HCCA, luego las de control y bulk, luego la cola de terminados. Las
 * transferencias terminan al instante; un NAK deja el TD para otro cuadro.
 *
 * Los controles van directo en los puertos raíz (el jugador 1 en el 3),
 * cada uno detrás de su hub de 3 puertos con el control en el puerto 1,
 * como en las consolas posteriores.
 */
#include "usb/usb.h"

#include <stdio.h>
#include <stdlib.h>

#define OHCI_BASE  0xFED00000u
#define OHCI_SIZE  0x1000u
#define OHCI_IRQ   1
#define ROOT_PORTS 4

/* HcControl */
#define CTL_PLE      (1u << 2)
#define CTL_IE       (1u << 3)
#define CTL_CLE      (1u << 4)
#define CTL_BLE      (1u << 5)
#define CTL_HCFS     (3u << 6)
#define HCFS_OPERATIONAL (2u << 6)
#define HCFS_SUSPEND     (3u << 6)
#define CTL_IR       (1u << 8)

/* HcCommandStatus */
#define CS_HCR (1u << 0)
#define CS_CLF (1u << 1)
#define CS_BLF (1u << 2)
#define CS_OCR (1u << 3)

/* HcInterruptStatus / Enable */
#define INT_SO   (1u << 0)
#define INT_WDH  (1u << 1)
#define INT_SF   (1u << 2)
#define INT_RD   (1u << 3)
#define INT_UE   (1u << 4)
#define INT_FNO  (1u << 5)
#define INT_RHSC (1u << 6)
#define INT_OC   (1u << 30)
#define INT_MIE  (1u << 31)

/* HcRhDescriptorA */
#define RHA_NPS     (1u << 9)
#define RHA_RW_MASK (0x00001b00u | (0xffu << 24))

/* HcRhStatus */
#define RHS_LPS  (1u << 0)
#define RHS_OCI  (1u << 1)
#define RHS_DRWE (1u << 15)
#define RHS_LPSC (1u << 16)
#define RHS_OCIC (1u << 17)
#define RHS_CRWE (1u << 31)

/* HcRhPortStatus */
#define PORT_CCS  (1u << 0)
#define PORT_PES  (1u << 1)
#define PORT_PSS  (1u << 2)
#define PORT_POCI (1u << 3)
#define PORT_PRS  (1u << 4)
#define PORT_PPS  (1u << 8)
#define PORT_LSDA (1u << 9)
#define PORT_CSC  (1u << 16)
#define PORT_PESC (1u << 17)
#define PORT_PSSC (1u << 18)
#define PORT_OCIC (1u << 19)
#define PORT_PRSC (1u << 20)
#define PORT_WTC  (PORT_CSC | PORT_PESC | PORT_PSSC | PORT_OCIC | PORT_PRSC)

/* Endpoint descriptor */
#define ED_FA(x)  ((x) & 0x7f)
#define ED_EN(x)  (((x) >> 7) & 0xf)
#define ED_D(x)   (((x) >> 11) & 3)
#define ED_K      (1u << 14)
#define ED_F      (1u << 15)
#define ED_H      1u
#define ED_C      2u
#define PTR_MASK  0xfffffff0u

/* General transfer descriptor */
#define TD_R       (1u << 18)
#define TD_DP(x)   (((x) >> 19) & 3)
#define TD_DI(x)   (((x) >> 21) & 7)
#define TD_T0      (1u << 24)
#define TD_T1      (1u << 25)
#define TD_EC_MASK (3u << 26)
#define TD_CC_SHIFT 28

#define PID_SETUP 0
#define PID_OUT   1
#define PID_IN    2

#define CC_NOERROR           0
#define CC_STALL             4
#define CC_DEVICENOTRESPONDING 5
#define CC_DATAUNDERRUN      9

typedef struct Ohci {
    uint8_t *ram;
    uint32_t ram_size;
    uint64_t now_ns, next_frame_ns;

    uint32_t ctl, status, intr_status, intr;
    uint32_t hcca, ctrl_head, ctrl_cur, bulk_head, bulk_cur, per_cur;
    uint32_t done;
    int done_count;
    uint32_t fi, fsmps, fit, pstart, lst;
    uint16_t frame_number;
    uint64_t frame_t0_ns;
    uint32_t rhdesc_a, rhdesc_b, rhstatus;
    uint32_t port[ROOT_PORTS];
    NcUsbDev *dev[ROOT_PORTS];
    bool irq;
    uint64_t frames, tds;
} Ohci;

static Ohci g_ohci;
extern unsigned g_polls;
static UsbStats g_stats;
static const bool g_trace = false;

/* Each player's controller (a hub with the gamepad on its port 1), made at
 * boot; its root port has it while the player has a controller
 * (nc_dev_ohci_set_pads). */
static const unsigned k_port_map[4] = {3, 4, 1, 2};
static NcUsbDev *g_pad_hub[4];
static bool g_ready;                    /* nc_dev_ohci_init done */

/* Guest physical memory, bounds-checked. */
static uint8_t *phys(Ohci *o, uint32_t pa, uint32_t len)
{
    if ((uint64_t)pa + len > o->ram_size) {
        return NULL;
    }
    return o->ram + pa;
}

static bool rd_words(Ohci *o, uint32_t pa, uint32_t *w, unsigned n)
{
    const uint8_t *p = phys(o, pa, 4 * n);
    if (!p) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        w[i] = nc_rd32(p + 4 * i);
    }
    return true;
}

static void wr_words(Ohci *o, uint32_t pa, const uint32_t *w, unsigned n)
{
    uint8_t *p = phys(o, pa, 4 * n);
    if (p) {
        for (unsigned i = 0; i < n; i++) {
            nc_wr32(p + 4 * i, w[i]);
        }
    }
}

static void update_irq(Ohci *o)
{
    const bool level = (o->intr & INT_MIE) && (o->intr_status & o->intr & ~INT_MIE);
    if (level != o->irq) {
        o->irq = level;

    }
}

static void set_interrupt(Ohci *o, uint32_t bits)
{
    o->intr_status |= bits;
    update_irq(o);
}

/* ------------------------------------------------------------- root hub */

static void port_attach(Ohci *o, unsigned i)
{
    if (o->dev[i]) {
        o->port[i] |= PORT_CCS | PORT_CSC | (o->dev[i]->low_speed ? PORT_LSDA : 0);
        set_interrupt(o, INT_RHSC);
    }
}

static void roothub_reset(Ohci *o)
{
    o->rhdesc_a = RHA_NPS | ROOT_PORTS;
    o->rhdesc_b = 0;
    o->rhstatus = 0;
    for (unsigned i = 0; i < ROOT_PORTS; i++) {
        o->port[i] = PORT_PPS;
        if (o->dev[i]) {
            nc_usb_reset(o->dev[i]);
            port_attach(o, i);
        }
    }
}

static void soft_reset(Ohci *o)
{
    o->ctl = (o->ctl & CTL_IR) | HCFS_SUSPEND;
    o->status = 0;
    o->intr_status = 0;
    o->intr = INT_MIE;
    o->hcca = 0;
    o->ctrl_head = o->ctrl_cur = o->bulk_head = o->bulk_cur = o->per_cur = 0;
    o->done = 0;
    o->done_count = 7;
    o->fsmps = 0x2778;
    o->fi = 0x2edf;
    o->fit = 0;
    o->frame_number = 0;
    o->pstart = 0;
    o->lst = 0x628;
    update_irq(o);
}

/* SetPortEnable/Suspend/Reset on a port with nothing on it only sets CSC. */
static bool port_set_if_connected(Ohci *o, unsigned i, uint32_t bit)
{
    if (!bit) {
        return false;
    }
    if (!(o->port[i] & PORT_CCS)) {
        o->port[i] |= PORT_CSC;
        return false;
    }
    if (o->port[i] & bit) {
        return false;
    }
    o->port[i] |= bit;
    return true;
}

static void port_write(Ohci *o, unsigned i, uint32_t val)
{
    const uint32_t old = o->port[i];
    if (val & PORT_WTC) {
        o->port[i] &= ~(val & PORT_WTC);
    }
    if (val & PORT_CCS) { /* ClearPortEnable */
        o->port[i] &= ~PORT_PES;
    }
    port_set_if_connected(o, i, val & PORT_PES);
    port_set_if_connected(o, i, val & PORT_PSS);
    if (val & PORT_POCI) { /* ClearSuspendStatus */
        if (o->port[i] & PORT_PSS) {
            o->port[i] = (o->port[i] & ~PORT_PSS) | PORT_PSSC;
        }
    }
    if (port_set_if_connected(o, i, val & PORT_PRS)) {
        nc_usb_reset(o->dev[i]);
        o->port[i] &= ~PORT_PRS;
        o->port[i] |= PORT_PES | PORT_PRSC;
    }
    /* Power is never switched (NPS): the port stays powered. */
    if (old != o->port[i]) {
        set_interrupt(o, INT_RHSC);
    }
}

static NcUsbDev *find_device(Ohci *o, uint8_t addr)
{
    for (unsigned i = 0; i < ROOT_PORTS; i++) {
        if (o->dev[i] && (o->port[i] & PORT_PES) && !(o->port[i] & PORT_PSS)) {
            NcUsbDev *d = nc_usb_find(o->dev[i], addr);
            if (d) {
                return d;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ transfers */

/* A TD's buffer: CBP..BE inclusive, possibly crossing into BE's page. */
static uint32_t td_len(uint32_t cbp, uint32_t be)
{
    if (!cbp) {
        return 0;
    }
    if ((cbp ^ be) & ~0xfffu) {
        return (0x1000u - (cbp & 0xfffu)) + (be & 0xfffu) + 1;
    }
    return be - cbp + 1;
}

static void td_copy(Ohci *o, uint32_t cbp, uint32_t be, uint8_t *buf, uint32_t len, bool to_guest)
{
    uint32_t first = len;
    if ((cbp ^ be) & ~0xfffu) {
        first = MIN(len, 0x1000u - (cbp & 0xfffu));
    }
    uint8_t *p = phys(o, cbp, first);
    if (p) {
        to_guest ? memcpy(p, buf, first) : memcpy(buf, p, first);
    }
    if (len > first) {
        p = phys(o, be & ~0xfffu, len - first);
        if (p) {
            to_guest ? memcpy(p, buf + first, len - first) : memcpy(buf + first, p, len - first);
        }
    }
}

/* Service the head TD of an ED. Returns false if the ED is waiting (NAK). */
static bool service_td(Ohci *o, uint32_t ed_pa, uint32_t ed[4])
{
    const uint32_t td_pa = ed[2] & PTR_MASK;
    uint32_t td[4];
    if (!rd_words(o, td_pa, td, 4)) {
        ed[2] |= ED_H;
        return false;
    }
    int pid;
    switch (ED_D(ed[0])) {
    case 1:
        pid = PID_OUT;
        break;
    case 2:
        pid = PID_IN;
        break;
    default:
        pid = (int)TD_DP(td[0]);
        break;
    }
    const uint32_t len = td_len(td[1], td[3]);
    static uint8_t buf[8192];
    if (len > sizeof(buf)) {
        ed[2] |= ED_H;
        return false;
    }
    if (pid != PID_IN && len) {
        td_copy(o, td[1], td[3], buf, len, false);
    }

    NcUsbDev *d = find_device(o, (uint8_t)ED_FA(ed[0]));
    const int ep = (int)ED_EN(ed[0]);
    int r;
    if (!d) {
        r = -100;
    } else if (ep == 0) {
        r = pid == PID_SETUP ? (len == 8 ? nc_usb_setup(d, buf) : NC_USB_STALL)
          : pid == PID_IN    ? nc_usb_ep0_in(d, buf, (int)len)
                             : nc_usb_ep0_out(d, buf, (int)len);
    } else {
        r = d->data ? d->data(d, pid == PID_IN, ep, buf, (int)len) : NC_USB_STALL;
    }
    if (r == NC_USB_NAK) {
        return false;
    }
    o->tds++;
    if (g_trace) {
        static const char *const k_pid[] = {"SETUP", "OUT", "IN", "?"};
        char hex[3 * 8 + 1] = "";
        const int show = r > 0 ? MIN(r, 8) : 0;
        for (int i = 0; i < show; i++) {
            snprintf(hex + 3 * i, 4, " %02x", buf[i]);
        }
        nc_log("hakuX-native-usb", "td %08x addr %u ep %d %s len %u -> %d%s (mps %u%s)", td_pa,
               (unsigned)ED_FA(ed[0]), ep, k_pid[pid & 3], len, r, hex, (ed[0] >> 16) & 0x7ff,
               td[0] & TD_R ? ", rounding" : "");
    }

    /* Whatever moved, CBP moves past it: all of it clears CBP; a short
     * packet leaves CBP at the first byte that didn't arrive (the host
     * driver counts a short transfer from it, error or not). */
    if (r >= 0) {
        if (pid == PID_IN && r > 0) {
            td_copy(o, td[1], td[3], buf, (uint32_t)r, true);
        }
        if ((uint32_t)r == len) {
            td[1] = 0;
        } else {
            const uint32_t first = ((td[1] ^ td[3]) & ~0xfffu) ? 0x1000u - (td[1] & 0xfffu) : len;
            td[1] = (uint32_t)r < first ? td[1] + (uint32_t)r : (td[3] & ~0xfffu) + ((uint32_t)r - first);
        }
    }
    uint32_t cc;
    if (r >= 0 && ((uint32_t)r == len || (pid == PID_IN && (td[0] & TD_R)))) {
        td[0] |= TD_T1;
        td[0] ^= TD_T0;
        cc = CC_NOERROR;
        ed[2] &= ~ED_C;
        if (td[0] & TD_T0) {
            ed[2] |= ED_C;
        }
    } else {
        if (r >= 0) {
            cc = CC_DATAUNDERRUN;
        } else if (r == NC_USB_STALL) {
            cc = CC_STALL;
        } else {
            cc = CC_DEVICENOTRESPONDING;
        }
        ed[2] |= ED_H;
    }
    td[0] = (td[0] & ~(TD_EC_MASK | (0xfu << TD_CC_SHIFT))) | (cc << TD_CC_SHIFT);

    /* Retire: off the ED, onto the done queue. */
    ed[2] = (td[2] & PTR_MASK) | (ed[2] & (ED_H | ED_C));
    td[2] = o->done;
    o->done = td_pa;
    const int di = (int)TD_DI(td[0]);
    if (di < o->done_count) {
        o->done_count = di;
    }
    if (cc != CC_NOERROR) {
        o->done_count = 0; /* errors are reported at once */
    }
    wr_words(o, td_pa, td, 4);
    wr_words(o, ed_pa + 8, &ed[2], 1);
    return !(ed[2] & ED_H);
}

/* Walk an ED list; returns whether any ED had TDs queued. */
static bool service_list(Ohci *o, uint32_t head, bool periodic)
{
    bool active = false;
    unsigned guard = 0;
    for (uint32_t pa = head & PTR_MASK; pa && guard < 256; guard++) {
        uint32_t ed[4];
        if (!rd_words(o, pa, ed, 4)) {
            break;
        }
        if (!(ed[0] & (ED_K | ED_F))) {
            unsigned tds = 0;
            while (!(ed[2] & ED_H) && (ed[2] & PTR_MASK) != (ed[1] & PTR_MASK) && tds < 64) {
                active = true;
                tds++;
                if (!service_td(o, pa, ed)) {
                    break;
                }
                if (periodic) {
                    break; /* one packet per interrupt endpoint per frame */
                }
            }
        }
        pa = ed[3] & PTR_MASK;
    }
    return active;
}

static void frame(Ohci *o)
{
    o->frames++;
    o->frame_number = (uint16_t)(o->frame_number + 1);
    uint8_t *hcca = phys(o, o->hcca, 256);
    if (hcca) {
        nc_wr16(hcca + 0x80, o->frame_number);
        nc_wr16(hcca + 0x82, 0);
    }
    if (!(o->frame_number & 0x7fff)) {
        o->intr_status |= INT_FNO;
    }

    if ((o->ctl & CTL_PLE) && hcca) {
        service_list(o, nc_rd32(hcca + 4 * (o->frame_number & 31)), true);
    }
    if ((o->ctl & CTL_CLE) && (o->status & CS_CLF)) {
        if (!service_list(o, o->ctrl_head, false)) {
            o->ctrl_cur = 0;
            o->status &= ~CS_CLF;
        }
    }
    if ((o->ctl & CTL_BLE) && (o->status & CS_BLF)) {
        if (!service_list(o, o->bulk_head, false)) {
            o->bulk_cur = 0;
            o->status &= ~CS_BLF;
        }
    }

    /* The done queue goes out when its delay runs down and the last one was taken. */
    if (o->done && o->done_count == 0 && !(o->intr_status & INT_WDH) && hcca) {
        uint32_t head = o->done;
        if (o->intr_status & o->intr & ~INT_WDH) {
            head |= 1;
        }
        nc_wr32(hcca + 0x84, head);
        o->done = 0;
        o->done_count = 7;
        o->intr_status |= INT_WDH;
    }
    if (o->done_count != 7 && o->done_count != 0) {
        o->done_count--;
    }
    o->intr_status |= INT_SF;
    update_irq(o);
}

/* ------------------------------------------------------------ registers */

uint32_t usb_read(uint32_t off)
{
    Ohci *o = &g_ohci;
    uint32_t v = 0;
    switch (off & ~3u) {
    case 0x00: v = 0x10; break;
    case 0x04: v = o->ctl; break;
    case 0x08: v = o->status; break;
    case 0x0C: v = o->intr_status; break;
    case 0x10:
    case 0x14: v = o->intr; break;
    case 0x18: v = o->hcca; break;
    case 0x1C: v = o->per_cur; break;
    case 0x20: v = o->ctrl_head; break;
    case 0x24: v = o->ctrl_cur; break;
    case 0x28: v = o->bulk_head; break;
    case 0x2C: v = o->bulk_cur; break;
    case 0x30: v = o->done; break;
    case 0x34: v = o->fit << 31 | o->fsmps << 16 | o->fi; break;
    case 0x38: {
        /* Time left in the current 1 ms frame, in bit times. */
        const uint64_t into = (o->now_ns - o->frame_t0_ns) % 1000000u;
        v = (uint32_t)((o->fi * (1000000u - into)) / 1000000u) & 0x3fff;
        break;
    }
    case 0x3C: v = o->frame_number; break;
    case 0x40: v = o->pstart; break;
    case 0x44: v = o->lst; break;
    case 0x48: v = o->rhdesc_a; break;
    case 0x4C: v = o->rhdesc_b; break;
    case 0x50: v = o->rhstatus; break;
    default:
        if (off >= 0x54 && off < 0x54 + 4 * ROOT_PORTS) {
            v = o->port[(off - 0x54) / 4];
        }
        break;
    }
    return v;
}

void usb_write(uint32_t off, uint32_t v)
{
    Ohci *o = &g_ohci;
    switch (off) {
    case 0x04: {
        const uint32_t old = o->ctl & CTL_HCFS;
        o->ctl = v;
        if ((v & CTL_HCFS) != old) {
            nc_log("hakuX-native-usb", "ohci: state %u -> %u", old >> 6, (v & CTL_HCFS) >> 6);
            if ((v & CTL_HCFS) == HCFS_OPERATIONAL) {
                o->frame_t0_ns = o->now_ns;
                o->next_frame_ns = o->now_ns + 1000000u;
            }
        }
        break;
    }
    case 0x08:
        if (v & CS_HCR) {
            soft_reset(o);
        }
        o->status |= v & (CS_CLF | CS_BLF | CS_OCR);
        break;
    case 0x0C:
        o->intr_status &= ~v;
        update_irq(o);
        break;
    case 0x10:
        o->intr |= v;
        update_irq(o);
        break;
    case 0x14:
        o->intr &= ~v;
        update_irq(o);
        break;
    case 0x18: o->hcca = v & 0xffffff00u; break;
    case 0x20: o->ctrl_head = v & PTR_MASK; break;
    case 0x24: o->ctrl_cur = v & PTR_MASK; break;
    case 0x28: o->bulk_head = v & PTR_MASK; break;
    case 0x2C: o->bulk_cur = v & PTR_MASK; break;
    case 0x34:
        o->fi = v & 0x3fff;
        o->fsmps = (v >> 16) & 0x7fff;
        o->fit = v >> 31;
        break;
    case 0x40: o->pstart = v & 0x3fff; break;
    case 0x44: o->lst = v & 0xfff; break;
    case 0x48: o->rhdesc_a = (o->rhdesc_a & ~RHA_RW_MASK) | (v & RHA_RW_MASK); break;
    case 0x4C: o->rhdesc_b = v; break;
    case 0x50:
        if (v & RHS_OCIC) {
            o->rhstatus &= ~RHS_OCIC;
        }
        if (v & RHS_DRWE) {
            o->rhstatus |= RHS_DRWE;
        }
        if (v & RHS_CRWE) {
            o->rhstatus &= ~RHS_DRWE;
        }
        break;
    default:
        if (off >= 0x54 && off < 0x54 + 4 * ROOT_PORTS) {
            port_write(o, (off - 0x54) / 4, v);
        }
        break;
    }
}

void usb_init(uint8_t *ram, uint32_t ram_size)
{
    Ohci *o = &g_ohci;
    for (unsigned i = 0; i < ROOT_PORTS; i++) {
        o->dev[i] = NULL; /* los dispositivos de antes quedan (pocos bytes) */
    }
    memset(o, 0, sizeof(*o));
    memset(&g_stats, 0, sizeof(g_stats));
    o->ram = ram;
    o->ram_size = ram_size;
    static bool made;
    if (!made) {
        for (unsigned p = 0; p < 4; p++) {
            g_pad_hub[p] = nc_usb_hub_new(3);
            nc_usb_hub_attach(g_pad_hub[p], 1, nc_xid_gamepad_new(p));
        }
        made = true;
    }
    /* el jugador 1: su hub en el puerto raíz 3 */
    o->dev[k_port_map[0] - 1] = g_pad_hub[0];
    soft_reset(o);
    o->ctl = 0;
    roothub_reset(o);
    g_ready = true;
}

/* Los cuadros de 1 ms hasta el tiempo virtual now (a lo más 100 de golpe). */
void usb_tick(uint64_t now)
{
    Ohci *o = &g_ohci;
    o->now_ns = now;
    if (!o->ram || (o->ctl & CTL_HCFS) != HCFS_OPERATIONAL) {
        return;
    }
    if (now > o->next_frame_ns + 100000000u) {
        o->next_frame_ns = now - 100000000u;
    }
    while (o->next_frame_ns <= now) {
        frame(o);
        o->next_frame_ns += 1000000u;
    }
}

bool usb_next_frame(uint64_t *ns)
{
    Ohci *o = &g_ohci;
    if (!o->ram || (o->ctl & CTL_HCFS) != HCFS_OPERATIONAL) {
        return false;
    }
    *ns = o->next_frame_ns;
    return true;
}

bool usb_irq(void) { return g_ohci.irq; }

const UsbStats *usb_stats(void)
{
    g_stats.frames = g_ohci.frames;
    g_stats.tds = g_ohci.tds;
    g_stats.polls = g_polls;
    g_stats.operational = (g_ohci.ctl & CTL_HCFS) == HCFS_OPERATIONAL;
    return &g_stats;
}
