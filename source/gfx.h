/*
 * XboxNX — dibujo por software sobre un búfer RGBA (1280x720 en la Switch).
 * Portable: en la PC se usa para generar vistas previas.
 */
#ifndef XBOXNX_GFX_H
#define XBOXNX_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xpr.h"

/* Color como en memoria del framebuffer RGBA_8888: bytes R, G, B, A. */
#define RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define HEX(rgb) RGBA(((rgb) >> 16) & 0xFF, ((rgb) >> 8) & 0xFF, (rgb) & 0xFF, 0xFF)
#define WITH_ALPHA(c, a) (((c) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))

typedef struct Gfx {
    uint32_t *px;
    int w, h, stride; /* stride en píxeles */
} Gfx;

void gfx_target(uint32_t *px, int w, int h, int stride);
const Gfx *gfx_get(void);

void gfx_clear(uint32_t c);
void gfx_rect(int x, int y, int w, int h, uint32_t c);
void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c);
void gfx_rrect_outline(int x, int y, int w, int h, int r, int thick, uint32_t c);
void gfx_circle(int cx, int cy, int r, uint32_t c);
/* Imagen escalada (bilineal) con esquinas redondeadas (r = 0: sin redondear). */
void gfx_image(const XImage *img, int x, int y, int w, int h, int r);

/* Fuente TrueType (en la Switch, la fuente del sistema). */
bool gfx_font_init(const uint8_t *ttf, size_t len);
/* Texto UTF-8; y es la parte de arriba de la línea. Devuelve el ancho. */
int gfx_text(int x, int y, int size, uint32_t c, const char *s);
int gfx_text_width(int size, const char *s);
/* Igual pero corta con "..." si no cabe en maxw. */
int gfx_text_fit(int x, int y, int size, int maxw, uint32_t c, const char *s);
/* Centrado horizontal en [x, x+w). */
void gfx_text_center(int x, int w, int y, int size, uint32_t c, const char *s);

#endif
