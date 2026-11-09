/*
 * XboxNX (de hakuX, GPL-3) — el control del Xbox (Duke): un dispositivo XID
 * (clase 0x58) con su reporte de 20 bytes en el IN 2 por interrupción y la
 * vibración en el OUT 2. La entrada llega de nc_pad_set (los controles de
 * la Switch).
 */
#include "usb/usb.h"

#include <stdlib.h>

#define XID_PLAYERS 4

typedef struct Xid {
    NcUsbDev dev;
    unsigned player;
} Xid;

static const uint8_t k_dev_desc[18] = {
    18, 1, 0x10, 0x01, /* USB 1.1 */
    0, 0, 0,           /* class per interface */
    0x40,
    0x5e, 0x04, 0x02, 0x02, /* Microsoft, controller (0x0202) */
    0x00, 0x01, 1, 2, 3, 1,
};

static const uint8_t k_cfg_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0x58, 0x42, 0, 0,  /* XID interface */
    7, 5, 0x82, 0x03, 0x20, 0, 4,     /* report: interrupt IN 2 */
    7, 5, 0x02, 0x03, 0x20, 0, 4,     /* rumble: interrupt OUT 2 */
};

static const uint8_t k_xid_desc[16] = {
    16, 0x42, 0x00, 0x01, /* XID 1.00 */
    1, 1,                 /* gamepad */
    20, 6,                /* input, output report sizes */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static const char *const k_strings[] = {"hakuX", "Microsoft Xbox Controller", "1"};

static NcPadState g_pad[XID_PLAYERS];
static uint16_t g_rumble[XID_PLAYERS][2];
unsigned g_polls; /* para las estadísticas de ohci.c */

static void make_report(unsigned player, uint8_t r[20])
{
    NcPadState p = g_pad[player];

    r[0] = 0;  /* report id */
    r[1] = 20; /* length */
    nc_wr16(r + 2, p.buttons);
    memcpy(r + 4, p.analog, 8);
    nc_wr16(r + 12, (uint16_t)p.lx);
    nc_wr16(r + 14, (uint16_t)p.ly);
    nc_wr16(r + 16, (uint16_t)p.rx);
    nc_wr16(r + 18, (uint16_t)p.ry);
}

static void take_rumble(unsigned player, const uint8_t *r, int len)
{
    if (len < 6) {
        return;
    }
    g_rumble[player][0] = nc_rd16(r + 2);
    g_rumble[player][1] = nc_rd16(r + 4);
}

static int xid_control(NcUsbDev *d, const uint8_t *s, uint8_t *data, int len)
{
    Xid *x = (Xid *)d;
    const uint8_t type = s[0], req = s[1];
    const uint16_t value = nc_rd16(s + 2);
    switch ((type << 8) | req) {
    case 0xA101: /* HID GET_REPORT */
        if (value != 0x0100) {
            return NC_USB_STALL;
        }
        {
            uint8_t r[20];
            make_report(x->player, r);
            memcpy(data, r, MIN(len, 20));
            return MIN(len, 20);
        }
    case 0x2109: /* HID SET_REPORT */
        if (value != 0x0200) {
            return NC_USB_STALL;
        }
        take_rumble(x->player, data, len);
        return 0;
    case 0xC106: /* XID GET_DESCRIPTOR */
        if (value != 0x4200) {
            return NC_USB_STALL;
        }
        memcpy(data, k_xid_desc, MIN(len, (int)sizeof(k_xid_desc)));
        return MIN(len, (int)sizeof(k_xid_desc));
    case 0xC101: /* XID GET_CAPABILITIES: every bit and axis is live */
        if (value == 0x0100) {
            memset(data, 0xff, MIN(len, 20));
            data[0] = 0;
            if (len > 1) {
                data[1] = 20;
            }
            return MIN(len, 20);
        }
        if (value == 0x0200) {
            memset(data, 0xff, MIN(len, 6));
            data[0] = 0;
            if (len > 1) {
                data[1] = 6;
            }
            return MIN(len, 6);
        }
        return NC_USB_STALL;
    default:
        nc_log("hakuX-native-usb", "xid: unhandled request %02x %02x value %04x", type, req, value);
        return NC_USB_STALL;
    }
}

static int xid_data(NcUsbDev *d, bool in, int ep, uint8_t *buf, int len)
{
    Xid *x = (Xid *)d;
    if (ep != 2) {
        return NC_USB_STALL;
    }
    if (in) {
        g_polls++;
        uint8_t r[20];
        make_report(x->player, r);
        {   /* button changes as the title reads them (a few a second at most) */
            static uint16_t s_last[XID_PLAYERS];
            static unsigned s_logged;
            const uint16_t b = (uint16_t)(r[2] | r[3] << 8);
            if (b != s_last[x->player] && s_logged < 200) {
                s_logged++;
                nc_log("hakuX-native-usb", "gamepad %u: buttons %04x -> %04x (A %02x)", x->player,
                       s_last[x->player], b, r[4]);
            }
            s_last[x->player] = b;
        }
        const int n = MIN(len, 20);
        memcpy(buf, r, n);
        return n;
    }
    take_rumble(x->player, buf, len);
    return len;
}

NcUsbDev *nc_xid_gamepad_new(unsigned player)
{
    Xid *x = calloc(1, sizeof(Xid));
    x->player = player % XID_PLAYERS;
    x->dev.name = "gamepad";
    x->dev.dev_desc = k_dev_desc;
    x->dev.dev_desc_len = sizeof(k_dev_desc);
    x->dev.cfg_desc = k_cfg_desc;
    x->dev.cfg_desc_len = sizeof(k_cfg_desc);
    x->dev.strings = k_strings;
    x->dev.nstrings = 3;
    x->dev.control = xid_control;
    x->dev.data = xid_data;
    x->dev.pending_addr = -1;
    return &x->dev;
}

void nc_pad_set(unsigned player, const NcPadState *s)
{
    if (player < XID_PLAYERS) {
        g_pad[player] = *s;
    }
}

void nc_pad_rumble(unsigned player, uint16_t *left, uint16_t *right)
{
    if (player < XID_PLAYERS) {
        *left = g_rumble[player][0];
        *right = g_rumble[player][1];
    }
}
