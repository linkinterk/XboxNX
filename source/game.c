/*
 * XboxNX — análisis de un juego.
 */
#include "game.h"

#include <stdlib.h>
#include <string.h>

static void root_cb(void *ctx, const char *name, uint32_t size, bool is_dir)
{
    Game *g = ctx;
    if (g->nroot >= GAME_MAX_ROOT) {
        return;
    }
    GameRootEntry *e = &g->root[g->nroot++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = 0;
    e->size = size;
    e->is_dir = is_dir;
}

static void count_thunks(Game *g)
{
    uint32_t off, avail;
    if (!xbe_va_to_file(&g->xbe, g->xbe.kernel_thunk, &off, &avail)) {
        return;
    }
    /* El kernel de Xbox tiene 366 exportaciones; la tabla termina en 0. */
    uint32_t n = avail / 4;
    if (n > 512) {
        n = 512;
    }
    uint8_t *buf = malloc(n * 4u);
    if (!buf) {
        return;
    }
    if (xdisc_read(&g->disc, &g->xbe_file, off, buf, n * 4u)) {
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *p = buf + i * 4;
            uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            if (!v) {
                break;
            }
            g->kernel_imports++;
            uint16_t ord = (uint16_t)(v & 0xFFFF);
            if ((v & 0x80000000u) && ord > g->max_ordinal) {
                g->max_ordinal = ord;
            }
        }
    }
    free(buf);
}

bool game_open(Game *g, const char *path)
{
    memset(g, 0, sizeof(*g));
    g->xbe_rc = XBE_ERR_SHORT;
    if (!xdisc_open(&g->disc, path)) {
        strcpy(g->error, "No es una imagen de Xbox (no se encontro XDVDFS)");
        return false;
    }
    g->disc_ok = true;

    XDiscFile root = xdisc_root(&g->disc);
    xdisc_list(&g->disc, &root, root_cb, g);

    if (!xdisc_find(&g->disc, "default.xbe", &g->xbe_file)) {
        strcpy(g->error, "La imagen no tiene default.xbe");
        return false;
    }
    g->xbe_found = true;

    /* Primero 4 KiB para saber el tamaño de las cabeceras. */
    uint8_t first[0x1000];
    uint32_t len = g->xbe_file.size < sizeof(first) ? g->xbe_file.size : (uint32_t)sizeof(first);
    if (!xdisc_read(&g->disc, &g->xbe_file, 0, first, len)) {
        strcpy(g->error, "No se pudo leer default.xbe");
        return false;
    }
    uint32_t hsize = len >= 0x10C ? (uint32_t)first[0x108] | ((uint32_t)first[0x109] << 8) |
                                        ((uint32_t)first[0x10A] << 16) | ((uint32_t)first[0x10B] << 24)
                                  : 0;
    if (hsize < len || hsize > 16u * 1024 * 1024 || hsize > g->xbe_file.size) {
        hsize = len;
    }
    uint8_t *hdr = malloc(hsize);
    if (!hdr) {
        strcpy(g->error, "Sin memoria");
        return false;
    }
    if (!xdisc_read(&g->disc, &g->xbe_file, 0, hdr, hsize)) {
        free(hdr);
        strcpy(g->error, "No se pudieron leer las cabeceras del XBE");
        return false;
    }
    g->xbe_rc = xbe_parse(hdr, hsize, &g->xbe);
    free(hdr);
    if (g->xbe_rc != XBE_OK) {
        snprintf(g->error, sizeof(g->error), "XBE invalido: %s", xbe_error(g->xbe_rc));
        return false;
    }
    count_thunks(g);
    return true;
}

void game_close(Game *g)
{
    xdisc_close(&g->disc);
}

unsigned game_xdk_build(const Game *g)
{
    const XbeLib *l = xbe_find_lib(&g->xbe, "XAPILIB");
    if (!l) {
        l = xbe_find_lib(&g->xbe, "XAPILIBP");
    }
    return l ? l->build : 0;
}
