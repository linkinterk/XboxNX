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
    CHECK(g.xbe.nsections == 2);
    CHECK(strcmp(g.xbe.sections[0].name, ".text") == 0);
    CHECK(strcmp(g.xbe.sections[1].name, ".rdata") == 0);
    CHECK(g.xbe.nlibs == 2);
    CHECK(game_xdk_build(&g) == 5849);
    CHECK(g.xbe.libs[1].debug_build);
    CHECK(g.kernel_imports == 4);
    CHECK(g.max_ordinal == 366);
    CHECK(g.nroot == 2);

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
