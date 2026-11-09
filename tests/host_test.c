/*
 * XboxNX — prueba en PC del lector (disc.c, xbe.c, game.c).
 * Uso: host_test prueba.iso [esperado_kind]
 */
#include <stdio.h>
#include <string.h>

#include "../source/game.h"

static int g_fail;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c);    \
            g_fail++;                                               \
        }                                                           \
    } while (0)

static Game g;

/* DXT1 de un bloque: color0 rojo puro, color1 azul puro, índices 0,1,2,3 por fila. */
static void test_dxt1(void)
{
    uint8_t b[0x20 + 8] = {0};
    memcpy(b, "XPR0", 4);
    b[8] = 0x20;
    uint32_t fmt = (2u << 24) | (2u << 20) | (0x0Cu << 8);
    memcpy(b + 0x18, &fmt, 4);
    uint16_t c0 = 0xF800, c1 = 0x001F;
    memcpy(b + 0x20, &c0, 2);
    memcpy(b + 0x22, &c1, 2);
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        idx |= (uint32_t)(i & 3) << (2 * i);
    }
    memcpy(b + 0x24, &idx, 4);
    XImage im;
    CHECK(xpr_decode(b, sizeof(b), &im) == 0);
    if (!im.px) {
        return;
    }
    const uint8_t *p0 = im.px, *p1 = im.px + 4, *p2 = im.px + 8, *p3 = im.px + 12;
    CHECK(p0[0] == 255 && p0[2] == 0);           /* rojo */
    CHECK(p1[0] == 0 && p1[2] == 255);           /* azul */
    CHECK(p2[0] == 170 && p2[2] == 85);          /* 2/3 rojo + 1/3 azul */
    CHECK(p3[0] == 85 && p3[2] == 170);
    CHECK(p0[3] == 255 && p3[3] == 255);
    ximage_free(&im);
    /* Basura no revienta. */
    CHECK(xpr_decode(b, 0x10, &im) < 0);
    b[0x19] = 0x55;
    CHECK(xpr_decode(b, sizeof(b), &im) < 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    bool ok = game_open(&g, argv[1]);
    if (!ok) {
        printf("error: %s\n", g.error);
    }
    CHECK(ok);
    if (argc > 2) {
        CHECK(strcmp(xdisc_kind(&g.disc), argv[2]) == 0);
    }
    printf("imagen: %s, juego: %s, title id %08X\n", xdisc_kind(&g.disc), g.xbe.title_name,
           (unsigned)g.xbe.title_id);
    CHECK(strcmp(g.xbe.title_name, "Juego de Prueba") == 0);
    CHECK(g.xbe.title_id == 0x58580001);
    CHECK(g.xbe.region == 5);
    CHECK(g.xbe.version == 3);
    CHECK(g.xbe.base == 0x10000);
    CHECK(g.xbe.entry == 0x11000);
    CHECK(g.xbe.kernel_thunk == 0x12000);
    CHECK(!g.xbe.debug_keys);
    CHECK(g.xbe.nsections == 3);
    CHECK(strcmp(g.xbe.sections[0].name, ".text") == 0);
    CHECK(strcmp(g.xbe.sections[1].name, ".rdata") == 0);
    CHECK(g.xbe.nlibs == 2);
    CHECK(game_xdk_build(&g) == 5849);
    CHECK(g.xbe.libs[1].debug_build);
    CHECK(g.kernel_imports == 4);
    CHECK(g.max_ordinal == 366);
    CHECK(g.nroot == 2);

    /* Imagen del juego: 4x4, píxel (x, y) = R x*40, G y*40, B 7. */
    CHECK(g.icon.px && g.icon.w == 4 && g.icon.h == 4);
    if (g.icon.px) {
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                const uint8_t *p = g.icon.px + (y * 4 + x) * 4;
                CHECK(p[0] == x * 40 && p[1] == y * 40 && p[2] == 7 && p[3] == 255);
            }
        }
    }
    test_dxt1();

    /* Un archivo dentro de una carpeta, con barra al revés y mayúsculas. */
    XDiscFile f;
    CHECK(xdisc_find(&g.disc, "\\MEDIA\\Leeme.TXT", &f));
    char buf[64] = {0};
    CHECK(f.size < sizeof(buf) && xdisc_read(&g.disc, &f, 0, buf, f.size));
    CHECK(strcmp(buf, "hola desde la iso de prueba\n") == 0);
    CHECK(!xdisc_find(&g.disc, "media/no_existe.bin", &f));
    /* Leer fuera del archivo falla. */
    CHECK(xdisc_find(&g.disc, "media/leeme.txt", &f));
    CHECK(!xdisc_read(&g.disc, &f, f.size, buf, 1));

    game_close(&g);

    /* Algo que no es una ISO de Xbox. */
    CHECK(!game_open(&g, argv[0]));
    CHECK(!g.disc_ok);
    game_close(&g);

    printf(g_fail ? "%d fallos\n" : "todo bien\n", g_fail);
    return g_fail ? 1 : 0;
}
