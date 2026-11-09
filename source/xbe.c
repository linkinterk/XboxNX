/*
 * XboxNX — lector de cabeceras XBE.
 *
 * Adaptado de hakuX (hw/xbox/hle/core/xbe.c, GPL-3.0-or-later).
 */
#include "xbe.h"

#include <stdio.h>
#include <string.h>

/* Offsets de la cabecera (xemu-xbe.h, struct xbe_header, empacada). */
#define XO_MAGIC            0x000
#define XO_BASE             0x104
#define XO_SIZEOF_HEADERS   0x108
#define XO_SIZEOF_IMAGE     0x10C
#define XO_TIMEDATE         0x114
#define XO_CERT_ADDR        0x118
#define XO_NSECTIONS        0x11C
#define XO_SECTIONS_ADDR    0x120
#define XO_ENTRY            0x128
#define XO_KERNEL_THUNK     0x158
#define XO_NLIBS            0x160
#define XO_LIBS_ADDR        0x164
#define XO_MIN_LEN          0x178

#define SEC_HDR_SIZE        56
#define LIB_SIZE            16
/* Certificado */
#define CERT_TITLE_ID       0x08
#define CERT_TITLE_NAME     0x0C   /* 40 caracteres UTF-16LE */
#define CERT_REGION         0xA0
#define CERT_VERSION        0xAC
#define CERT_MIN_LEN        0xB0

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* VA de cabecera [va, va+len) a offset en hdr[]. */
static bool hva(uint32_t base, uint32_t hdr_len, uint32_t va, uint32_t len, uint32_t *off)
{
    if (va < base) {
        return false;
    }
    uint32_t o = va - base;
    if (o > hdr_len || len > hdr_len - o) {
        return false;
    }
    *off = o;
    return true;
}

static void copy_name(char *dst, uint32_t cap, const uint8_t *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i + 1 < cap && i < max && src[i]; i++) {
        dst[i] = (char)src[i];
    }
    dst[i] = '\0';
}

int xbe_parse(const uint8_t *hdr, uint32_t hdr_len, XbeInfo *out)
{
    memset(out, 0, sizeof(*out));
    if (hdr_len < XO_MIN_LEN) {
        return XBE_ERR_SHORT;
    }
    if (rd32(hdr + XO_MAGIC) != XBE_MAGIC) {
        return XBE_ERR_MAGIC;
    }

    out->base = rd32(hdr + XO_BASE);
    out->sizeof_headers = rd32(hdr + XO_SIZEOF_HEADERS);
    out->sizeof_image = rd32(hdr + XO_SIZEOF_IMAGE);
    out->timedate = rd32(hdr + XO_TIMEDATE);

    uint32_t enc_entry = rd32(hdr + XO_ENTRY);
    uint32_t enc_thunk = rd32(hdr + XO_KERNEL_THUNK);
    uint32_t lo = out->base;
    uint32_t hi = out->base + out->sizeof_image;
    uint32_t e_retail = enc_entry ^ XBE_ENTRY_KEY_RETAIL;
    uint32_t e_debug = enc_entry ^ XBE_ENTRY_KEY_DEBUG;
    if (e_retail >= lo && e_retail < hi) {
        out->entry = e_retail;
        out->kernel_thunk = enc_thunk ^ XBE_THUNK_KEY_RETAIL;
    } else if (e_debug >= lo && e_debug < hi) {
        out->entry = e_debug;
        out->kernel_thunk = enc_thunk ^ XBE_THUNK_KEY_DEBUG;
        out->debug_keys = true;
    } else {
        return XBE_ERR_ENTRY;
    }

    uint32_t off;
    uint32_t cert = rd32(hdr + XO_CERT_ADDR);
    if (hva(out->base, hdr_len, cert, CERT_MIN_LEN, &off)) {
        const uint8_t *c = hdr + off;
        out->title_id = rd32(c + CERT_TITLE_ID);
        unsigned n = 0;
        for (unsigned i = 0; i < 40; i++) {
            uint16_t ch = rd16(c + CERT_TITLE_NAME + i * 2);
            if (!ch) {
                break;
            }
            out->title_name[n++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
        }
        out->title_name[n] = 0;
        out->region = rd32(c + CERT_REGION);
        out->version = rd32(c + CERT_VERSION);
    }

    uint32_t nsec = rd32(hdr + XO_NSECTIONS);
    uint32_t sec_addr = rd32(hdr + XO_SECTIONS_ADDR);
    if (nsec > XBE_MAX_SECTIONS || !hva(out->base, hdr_len, sec_addr, nsec * SEC_HDR_SIZE, &off)) {
        return XBE_ERR_RANGE;
    }
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *s = hdr + off + i * SEC_HDR_SIZE;
        XbeSection *d = &out->sections[i];
        d->flags = rd32(s + 0x00);
        d->va = rd32(s + 0x04);
        d->vsize = rd32(s + 0x08);
        d->raw_off = rd32(s + 0x0C);
        d->raw_size = rd32(s + 0x10);
        uint32_t noff;
        if (hva(out->base, hdr_len, rd32(s + 0x14), 1, &noff)) {
            copy_name(d->name, sizeof(d->name), hdr + noff, hdr_len - noff);
        }
    }
    out->nsections = nsec;

    uint32_t nlib = rd32(hdr + XO_NLIBS);
    uint32_t lib_addr = rd32(hdr + XO_LIBS_ADDR);
    if (nlib > XBE_MAX_LIBS || !hva(out->base, hdr_len, lib_addr, nlib * LIB_SIZE, &off)) {
        return XBE_ERR_RANGE;
    }
    for (uint32_t i = 0; i < nlib; i++) {
        const uint8_t *l = hdr + off + i * LIB_SIZE;
        XbeLib *d = &out->libs[i];
        copy_name(d->name, sizeof(d->name), l, 8);
        d->major = rd16(l + 8);
        d->minor = rd16(l + 10);
        d->build = rd16(l + 12);
        uint16_t flags = rd16(l + 14);
        d->qfe = flags & 0x1FFF;
        d->debug_build = (flags & 0x8000) != 0;
    }
    out->nlibs = nlib;
    return XBE_OK;
}

const char *xbe_error(int rc)
{
    switch (rc) {
    case XBE_OK: return "ok";
    case XBE_ERR_SHORT: return "cabecera muy corta";
    case XBE_ERR_MAGIC: return "no es un XBE (falta XBEH)";
    case XBE_ERR_RANGE: return "secciones o librerias fuera de rango";
    case XBE_ERR_ENTRY: return "punto de entrada invalido";
    default: return "error desconocido";
    }
}

const XbeLib *xbe_find_lib(const XbeInfo *x, const char *name)
{
    for (unsigned i = 0; i < x->nlibs; i++) {
        if (strcmp(x->libs[i].name, name) == 0) {
            return &x->libs[i];
        }
    }
    return NULL;
}

bool xbe_va_to_file(const XbeInfo *x, uint32_t va, uint32_t *file_off, uint32_t *avail)
{
    if (va >= x->base && va - x->base < x->sizeof_headers) {
        *file_off = va - x->base;
        *avail = x->sizeof_headers - *file_off;
        return true;
    }
    for (unsigned i = 0; i < x->nsections; i++) {
        const XbeSection *s = &x->sections[i];
        if (va >= s->va && va - s->va < s->raw_size) {
            *file_off = s->raw_off + (va - s->va);
            *avail = s->raw_size - (va - s->va);
            return true;
        }
    }
    return false;
}

void xbe_region_str(uint32_t region, char *out, unsigned cap)
{
    out[0] = 0;
    if (region & 0x1) {
        strncat(out, "NA ", cap - strlen(out) - 1);
    }
    if (region & 0x2) {
        strncat(out, "JP ", cap - strlen(out) - 1);
    }
    if (region & 0x4) {
        strncat(out, "EU/Resto ", cap - strlen(out) - 1);
    }
    if (region & 0x80000000u) {
        strncat(out, "Fabrica ", cap - strlen(out) - 1);
    }
    if (!out[0]) {
        snprintf(out, cap, "0x%08X", (unsigned)region);
    }
}
