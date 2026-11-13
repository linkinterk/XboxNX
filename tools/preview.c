/*
 * XboxNX — vistas previas de las pantallas en la PC (PNG).
 * Uso: preview fuente.ttf prueba.iso carpeta_salida
 * Usa juegos de ejemplo inventados y la ISO de prueba de tests/mkxiso.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/gfx.h"
#include "../source/ui.h"
#include "../source/hw.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static uint32_t fb[UI_W * UI_H];

static void save(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    stbi_write_png(path, UI_W, UI_H, 4, fb, UI_W * 4);
    printf("%s\n", path);
}

/* Imagen inventada: degradado con un círculo, para ver cómo queda una carátula. */
static void fake_icon(XImage *im, int seed)
{
    im->w = im->h = 128;
    im->px = malloc(128 * 128 * 4);
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            uint8_t *p = im->px + (y * 128 + x) * 4;
            int dx = x - 64, dy = y - 70, d = dx * dx + dy * dy;
            int ring = d < 40 * 40 && d > 28 * 28;
            p[0] = (uint8_t)(ring ? 240 : (seed * 70 + x) % 256 / 2);
            p[1] = (uint8_t)(ring ? 240 : (seed * 30 + y) % 256 / 2 + 40);
            p[2] = (uint8_t)(ring ? 240 : (seed * 110 + x + y) % 256 / 2 + 60);
            p[3] = 255;
        }
    }
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

static Game g_game;

int main(int argc, char **argv)
{
    if (argc < 4) {
        return 2;
    }
    size_t flen;
    unsigned char *ttf = read_file(argv[1], &flen);
    if (!ttf || !gfx_font_init(ttf, flen)) {
        fprintf(stderr, "fuente?\n");
        return 1;
    }
    gfx_target(fb, UI_W, UI_H, UI_W);

    static UiCard cards[5];
    const char *titles[] = {"Juego de Prueba", "Aventura Espacial", "Carreras Extremas 3", "Liga de Futbol 2005",
                            "Un juego con un nombre larguisimo que no cabe en la tarjeta"};
    for (int i = 0; i < 5; i++) {
        UiCard *c = &cards[i];
        snprintf(c->file, sizeof(c->file), "%s.iso", titles[i]);
        snprintf(c->title, sizeof(c->title), "%s", titles[i]);
        c->title_id = 0x58580001u + (uint32_t)i;
        c->region = i % 2 ? 1 : 5;
        c->version = (uint32_t)i + 1;
        c->size = (unsigned long long)(i + 2) * 1100 * 1024 * 1024;
        c->kind = i == 2 ? "Disco completo XGD2" : "XISO";
        c->ok = i != 3;
        if (!c->ok) {
            snprintf(c->error, sizeof(c->error), "La imagen no tiene default.xbe");
        }
        if (i != 1 && i != 3) {
            fake_icon(&c->icon, i);
        }
    }

    UiState s = {0};
    s.version = "0.18.35";
    s.cards = cards;

    s.screen = UI_LOADING;
    s.load_done = 2;
    s.load_total = 5;
    s.load_name = "Carreras Extremas 3.iso";
    ui_draw(&s);
    save(argv[3], "1_cargando");

    s.screen = UI_LIBRARY;
    s.ncards = 5;
    s.sel = 0;
    ui_draw(&s);
    save(argv[3], "2_biblioteca");

    s.ncards = 0;
    ui_draw(&s);
    save(argv[3], "3_vacia");
    s.screen = UI_STAGES;
    ui_draw(&s);
    save(argv[3], "3b_etapas");
    s.screen = UI_LIBRARY;
    s.ncards = 5;

    if (!game_open(&g_game, argv[2])) {
        fprintf(stderr, "iso: %s\n", g_game.error);
        return 1;
    }
    ui_card_free(&cards[0]);
    ui_card_from_game(&cards[0], &g_game, "Juego de Prueba.iso", 2200ull * 1024 * 1024, true);
    s.screen = UI_DETAIL;
    s.game = &g_game;
    s.sel = 0;
    const char *names[] = {"4_detalle_general", "5_detalle_secciones", "6_detalle_librerias", "7_detalle_archivos"};
    for (int p = 0; p < UI_PAGES; p++) {
        s.page = p;
        ui_draw(&s);
        save(argv[3], names[p]);
    }

    /* etapa 2a */
    static Vm vm;
    static Kernel k;
    static LoadReport lr;
    vm_init(&vm);
    kernel_init(&k, &vm);
    loader_load(&vm, &k, &g_game, &lr);
    s.screen = UI_MEMORY;
    s.load = &lr;
    ui_draw(&s);
    save(argv[3], "10_memoria");
    LoadReport bad = lr;
    bad.ok = false;
    snprintf(bad.error, sizeof(bad.error), "Sin memoria para la sección .data");
    s.load = &bad;
    ui_draw(&s);
    save(argv[3], "11_memoria_error");
    /* etapa 2c */
    static Runner run;
    runner_boot(&run, &vm, &k, &lr);
    runner_slice(&run, 100000);
    run.ns = 1000000;
    /* hilos de ejemplo para ver el panel lleno */
    run.state = RUN_ACTIVE;
    for (int i = 0, n = 0; i < K_MAX_THREADS && n < 4; i++) {
        KThread *t = &k.threads[i];
        if (t->state != KT_FREE) {
            continue;
        }
        t->state = n == 3 ? KT_DEAD : n == 2 ? KT_READY : KT_WAITING;
        t->id = (uint32_t)(10 + n) * 4;
        t->priority = 8 + n;
        t->nwait = 1;
        t->wait_obj[0] = 0x80012340;
        t->has_deadline = n == 1;
        t->ctx.r[R_ESP] = run.cpu.r[R_ESP];
        t->exit_status = 1;
        t->instructions = 12345678;
        n++;
    }
    s.screen = UI_RUN;
    s.run = &run;
    s.run_scroll = -1;
    ui_draw(&s);
    save(argv[3], "12_ejecucion");
    /* la pantalla del Xbox con un degradado de ejemplo */
    for (uint32_t y = 0; y < 480; y++) {
        for (uint32_t x = 0; x < 640; x++) {
            uint32_t *p = (uint32_t *)(vm.ram + 0x02000000u + y * 2560 + x * 4);
            *p = 0xFF000000u | (x * 255 / 639) << 16 | (y * 255 / 479) << 8 | 0x40;
        }
    }
    hw_set_display(&k, 0x1E, 2560, 0x02000000u);
    s.run_screen = true;
    ui_draw(&s);
    save(argv[3], "13_pantalla");
    s.run_screen = false;
    vm_free(&vm);

    ProbeResult pr = {0};
    pr.applet_mode = false;
    pr.mem_total = 3285ull << 20;
    pr.mem_used = 12ull << 20;
    pr.biggest_alloc = 3200ull << 20;
    pr.jit_ok = true;
    pr.jit_value = 42;
    pr.va4g_ok = true;
    s.screen = UI_PROBE;
    s.probe = &pr;
    ui_draw(&s);
    save(argv[3], "8_pruebas");
    s.exc_tried = true;
    s.exc_ok = true;
    s.exc_desc = 0x101;
    s.exc_far = 0x10;
    ui_draw(&s);
    save(argv[3], "9_pruebas_hechas");

    game_close(&g_game);
    return 0;
}
