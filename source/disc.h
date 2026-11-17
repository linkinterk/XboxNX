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
    int fd;                /* el de f: se lee directo, sin stdio */
    /* v0.18.87: imágenes partidas (juego.1.iso + juego.2.iso..., para tarjetas
     * FAT32, que no admiten archivos de más de 4 GB): los trozos, uno tras otro */
    FILE *more[7];
    int nmore;
    uint64_t size[8];      /* de cada trozo (0: el de f) */
    uint64_t part_off;     /* donde empieza la partición del juego */
    uint32_t root_sector;
    uint32_t root_size;
} XDisc;

typedef struct XDiscFile {
    uint32_t sector;
    uint32_t size;
    uint8_t attr;          /* 0x10: carpeta */
} XDiscFile;

/* Abre una XISO o un volcado completo (offsets XGD1/XGD2/XGD3); si el nombre
 * termina en .1.iso / .1.xiso, también sus trozos .2, .3... */
/* ¿Es el trozo 2 o posterior de una imagen partida? (no va en la biblioteca) */
bool xdisc_is_tail_part(const char *name);
bool xdisc_open(XDisc *d, const char *path);
void xdisc_close(XDisc *d);

/* Busca "default.xbe" o "media\\x.bin" (cualquier barra, sin distinguir
 * mayúsculas). */
bool xdisc_find(const XDisc *d, const char *path, XDiscFile *out);

/* Lee len bytes en el offset off de un archivo. */
bool xdisc_read(const XDisc *d, const XDiscFile *f, uint64_t off, void *buf, size_t len);

/* v0.18.69 (la caché de lecturas): dónde empieza un archivo en la imagen,
 * leer de la imagen por offset absoluto, y su tamaño (0: no se sabe). */
uint64_t xdisc_abs(const XDisc *d, const XDiscFile *f);
bool xdisc_read_abs(const XDisc *d, uint64_t abs, void *buf, size_t len);
uint64_t xdisc_image_size(const XDisc *d);

/* Recorre una carpeta (en orden del árbol). Devuelve cuántas entradas vio, o
 * -1 si no se pudo leer. */
int xdisc_list(const XDisc *d, const XDiscFile *dir,
               void (*fn)(void *ctx, const char *name, uint32_t size, bool is_dir), void *ctx);

/* La carpeta raíz como XDiscFile. */
XDiscFile xdisc_root(const XDisc *d);

/* Tipo de imagen según el offset de la partición. */
const char *xdisc_kind(const XDisc *d);

#endif
