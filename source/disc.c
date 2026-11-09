/*
 * XboxNX — lector de XDVDFS.
 *
 * Adaptado de hakuX (native/core/xdvdfs.c, GPL-3.0-or-later).
 *
 * Descriptor de volumen en el sector 32 de la partición del juego:
 * "MICROSOFT*XBOX*MEDIA", sector y tamaño de la raíz. Una carpeta es un
 * árbol binario de entradas:
 *   u16 izquierda, u16 derecha (offsets en dwords; 0 = ninguno; 0xFFFF
 *                               rellena el final de un sector)
 *   u32 sector, u32 tamaño
 *   u8 atributos, u8 largo del nombre, nombre
 */
#define _FILE_OFFSET_BITS 64
#include "disc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static const char k_magic[] = "MICROSOFT*XBOX*MEDIA";
/* XISO extraída, luego volcados completos XGD1 / XGD2 / XGD3. */
static const uint64_t k_part_offsets[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool read_at(FILE *f, void *buf, size_t len, uint64_t off)
{
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) {
        return false;
    }
    return fread(buf, 1, len, f) == len;
}

bool xdisc_open(XDisc *d, const char *path)
{
    memset(d, 0, sizeof(*d));
    d->f = fopen(path, "rb");
    if (!d->f) {
        return false;
    }
    setvbuf(d->f, NULL, _IOFBF, 64 * 1024);
    for (size_t i = 0; i < sizeof(k_part_offsets) / sizeof(k_part_offsets[0]); i++) {
        uint8_t vd[XDISC_SECTOR];
        uint64_t off = k_part_offsets[i];
        if (!read_at(d->f, vd, sizeof(vd), off + 32ull * XDISC_SECTOR)) {
            continue;
        }
        if (memcmp(vd, k_magic, 20) == 0 && memcmp(vd + 0x7EC, k_magic, 20) == 0) {
            d->part_off = off;
            d->root_sector = rd32(vd + 20);
            d->root_size = rd32(vd + 24);
            return true;
        }
    }
    fclose(d->f);
    d->f = NULL;
    return false;
}

void xdisc_close(XDisc *d)
{
    if (d->f) {
        fclose(d->f);
    }
    d->f = NULL;
}

XDiscFile xdisc_root(const XDisc *d)
{
    XDiscFile r = {d->root_sector, d->root_size, 0x10};
    return r;
}

const char *xdisc_kind(const XDisc *d)
{
    switch (d->part_off) {
    case 0: return "XISO";
    case 0x18300000ull: return "Disco completo XGD1";
    case 0xFD90000ull: return "Disco completo XGD2";
    case 0x2080000ull: return "Disco completo XGD3";
    default: return "?";
    }
}

static bool name_eq(const uint8_t *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) {
        return false;
    }
    for (size_t i = 0; i < alen; i++) {
        if (toupper(a[i]) != toupper((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

/* Recorrido en profundidad de todo el árbol. */
static bool dir_search(const uint8_t *dir, uint32_t size, uint32_t off, const char *name,
                       size_t namelen, XDiscFile *out, unsigned depth)
{
    if (off + 14 > size || depth > 64) {
        return false;
    }
    const uint8_t *e = dir + off;
    uint16_t left = rd16(e), right = rd16(e + 2);
    if (left == 0xFFFF && right == 0xFFFF && e[12] == 0xFF) {
        return false; /* relleno */
    }
    uint8_t nlen = e[13];
    if (off + 14 + nlen <= size && name_eq(e + 14, nlen, name, namelen)) {
        out->sector = rd32(e + 4);
        out->size = rd32(e + 8);
        out->attr = e[12];
        return true;
    }
    return (left && left != 0xFFFF && dir_search(dir, size, left * 4u, name, namelen, out, depth + 1)) ||
           (right && right != 0xFFFF && dir_search(dir, size, right * 4u, name, namelen, out, depth + 1));
}

static uint8_t *read_dir(const XDisc *d, const XDiscFile *dir)
{
    if (!(dir->attr & 0x10) || !dir->size || dir->size > 16u * 1024 * 1024) {
        return NULL;
    }
    uint8_t *buf = malloc(dir->size);
    if (!buf) {
        return NULL;
    }
    if (!read_at(d->f, buf, dir->size, d->part_off + (uint64_t)dir->sector * XDISC_SECTOR)) {
        free(buf);
        return NULL;
    }
    return buf;
}

bool xdisc_find(const XDisc *d, const char *path, XDiscFile *out)
{
    XDiscFile cur = xdisc_root(d);
    const char *p = path;
    while (*p) {
        while (*p == '\\' || *p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *end = p;
        while (*end && *end != '\\' && *end != '/') {
            end++;
        }
        uint8_t *dir = read_dir(d, &cur);
        if (!dir) {
            return false;
        }
        bool ok = dir_search(dir, cur.size, 0, p, (size_t)(end - p), &cur, 0);
        free(dir);
        if (!ok) {
            return false;
        }
        p = end;
    }
    *out = cur;
    return true;
}

bool xdisc_read(const XDisc *d, const XDiscFile *f, uint64_t off, void *buf, size_t len)
{
    if (off > f->size || len > f->size - off) {
        return false;
    }
    return read_at(d->f, buf, len, d->part_off + (uint64_t)f->sector * XDISC_SECTOR + off);
}

static int walk(const uint8_t *dir, uint32_t size, uint32_t off, unsigned depth,
                void (*fn)(void *, const char *, uint32_t, bool), void *ctx)
{
    if (off + 14 > size || depth > 64) {
        return 0;
    }
    const uint8_t *e = dir + off;
    uint16_t left = rd16(e), right = rd16(e + 2);
    if (left == 0xFFFF && right == 0xFFFF && e[12] == 0xFF) {
        return 0;
    }
    int n = 0;
    if (left && left != 0xFFFF) {
        n += walk(dir, size, left * 4u, depth + 1, fn, ctx);
    }
    uint8_t nlen = e[13];
    if (off + 14 + nlen <= size) {
        char name[256];
        memcpy(name, e + 14, nlen);
        name[nlen] = 0;
        fn(ctx, name, rd32(e + 8), (e[12] & 0x10) != 0);
        n++;
    }
    if (right && right != 0xFFFF) {
        n += walk(dir, size, right * 4u, depth + 1, fn, ctx);
    }
    return n;
}

int xdisc_list(const XDisc *d, const XDiscFile *dir,
               void (*fn)(void *ctx, const char *name, uint32_t size, bool is_dir), void *ctx)
{
    uint8_t *buf = read_dir(d, dir);
    if (!buf) {
        return -1;
    }
    int n = walk(buf, dir->size, 0, 0, fn, ctx);
    free(buf);
    return n;
}
