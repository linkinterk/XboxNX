/*
 * XboxNX — decodificador de texturas XPR0 (la imagen del juego que trae el
 * XBE en $$XTIMAGE / $$XSIMAGE).
 *
 * Portado de hakuX (android/.../data/XprImage.kt, GPL-3.0-or-later).
 */
#ifndef XBOXNX_XPR_H
#define XBOXNX_XPR_H

#include <stddef.h>
#include <stdint.h>

/* Imagen RGBA8 (bytes R, G, B, A en ese orden). */
typedef struct XImage {
    int w, h;
    uint8_t *px;
} XImage;

/* Decodifica; devuelve 0 si se pudo (img->px con malloc), <0 si no. */
int xpr_decode(const uint8_t *b, size_t len, XImage *img);
void ximage_free(XImage *img);

#endif
