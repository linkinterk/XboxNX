/*
 * XboxNX — lector de XDVDFS (sistema de archivos del disco de Xbox).
 *
 * Adaptado de hakuX (native/core/xdvdfs.c, GPL-3.0-or-later,
 * https://github.com/rfandango/hakuX): sin la capa de mods, sobre FILE*
 * para que funcione igual en la Switch (newlib) y en la PC (pruebas).
 */
#ifndef XBOXNX_DISC_H
#define XBOXNX_DISC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define XDISC_SECTOR 2048u

typedef struct XDisc {
    FILE *f;
    uint64_t part_off;     /* donde empieza la partición del juego */
    uint32_t root_sector;
    uint32_t root_size;
} XDisc;

typedef struct XDiscFile {
    uint32_t sector;
    uint32_t size;
    uint8_t attr;          /* 0x10: carpeta */
} XDiscFile;

/* Abre una XISO o un volcado completo (offsets XGD1/XGD2/XGD3). */
bool xdisc_open(XDisc *d, const char *path);
void xdisc_close(XDisc *d);

/* Busca "default.xbe" o "media\\x.bin" (cualquier barra, sin distinguir
 * mayúsculas). */
bool xdisc_find(const XDisc *d, const char *path, XDiscFile *out);

/* Lee len bytes en el offset off de un archivo. */
bool xdisc_read(const XDisc *d, const XDiscFile *f, uint64_t off, void *buf, size_t len);

/* Recorre una carpeta (en orden del árbol). Devuelve cuántas entradas vio, o
 * -1 si no se pudo leer. */
int xdisc_list(const XDisc *d, const XDiscFile *dir,
               void (*fn)(void *ctx, const char *name, uint32_t size, bool is_dir), void *ctx);

/* La carpeta raíz como XDiscFile. */
XDiscFile xdisc_root(const XDisc *d);

/* Tipo de imagen según el offset de la partición. */
const char *xdisc_kind(const XDisc *d);

#endif
