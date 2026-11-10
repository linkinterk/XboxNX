/*
 * XboxNX — etapa 1: cargador, con interfaz gráfica.
 *
 * Biblioteca de juegos de sdmc:/xbox/ en tarjetas (con la imagen que trae
 * cada disco), detalle del juego y pruebas de hardware para la etapa 2.
 *
 * Basado en hakuX (https://github.com/rfandango/hakuX), GPL-3.0-or-later.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>

#include "game.h"
#include "gfx.h"
#include "kernel.h"
#include "loader.h"
#include "runner.h"
#include "probe.h"
#include "ui.h"
#include "usb/usb.h"
#include "prof.h"
#include "apu/apu.h"

#define APP_VER   "0.18.9"
#define GAMES_DIR "sdmc:/xbox"
#define MAX_GAMES 128

static uint32_t g_shadow[UI_W * UI_H];
static Framebuffer g_fb;

static UiCard g_cards[MAX_GAMES];
static int g_ncards;
static Game g_game;       /* el juego abierto en el detalle */
static Game g_scan;       /* para leer cada juego al cargar */
static ProbeResult g_probe;
static bool g_probe_done;
static UiState g_ui;
static Vm g_vm;
static Kernel g_kernel;
static void present(void);
static LoadReport g_load;
static Runner g_run;

static void load_into_memory(void);

/* Etapa 2c: memoria limpia y el hilo de arranque en el intérprete. */
/* El tiempo virtual por instrucción (Y en el registro): 2 ns es un
 * Pentium III de 733 MHz; más, y el juego avanza más por cada instrucción
 * (sus esperas activas terminan antes) aunque crea tener una CPU lenta. */
static unsigned g_nspi = 2;

static void start_run(void)
{
    load_into_memory();
    if (!g_load.ok) {
        return; /* se queda en la pantalla de memoria con el error */
    }
    g_kernel.ns_per_instr = g_nspi;
    runner_boot(&g_run, &g_vm, &g_kernel, &g_load);
    g_ui.run = &g_run;
    g_ui.run_scroll = -1;
    g_ui.screen = UI_RUN;
}

/* Etapa 2a: memoria del Xbox limpia, kernel y el juego cargado. */
static void load_into_memory(void)
{
    g_ui.screen = UI_LOADING;
    g_ui.load_total = 0;
    g_ui.load_name = "Cargando en la memoria del Xbox...";
    present();
    vm_free(&g_vm);
    memset(&g_load, 0, sizeof(g_load));
    u64 t0 = armGetSystemTick();
    if (!vm_init(&g_vm)) {
        snprintf(g_load.error, sizeof(g_load.error), "No hay RAM para los 64 MB del Xbox");
    } else if (!kernel_init(&g_kernel, &g_vm)) {
        snprintf(g_load.error, sizeof(g_load.error), "No se pudo preparar el área del kernel");
    } else {
        /* D: es la ISO; el disco duro del Xbox, carpetas en la SD */
        kio_set_media(&g_kernel, &g_game.disc, "sdmc:/switch/XboxNX/hdd");
        loader_load(&g_vm, &g_kernel, &g_game, &g_load);
    }
    g_load.ms = (unsigned)(armTicksToNs(armGetSystemTick() - t0) / 1000000);
    g_ui.load = &g_load;
    g_ui.load_scroll = 0;
    g_ui.load_name = NULL;
    g_ui.screen = UI_MEMORY;
}

static void present(void)
{
    if (g_ui.screen == UI_RUN) {
        prof_take(g_ui.prof);
    }
    prof_push(PROF_UI);
    ui_draw(&g_ui);
    u32 stride;
    u8 *dst = framebufferBegin(&g_fb, &stride);
    for (int y = 0; y < UI_H; y++) {
        memcpy(dst + (size_t)y * stride, &g_shadow[y * UI_W], UI_W * 4);
    }
    framebufferEnd(&g_fb);
    prof_pop();
}

static bool has_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".iso") || !strcasecmp(dot, ".xiso"));
}

static int cmp_cards(const void *a, const void *b)
{
    const UiCard *x = a, *y = b;
    const char *nx = x->title[0] ? x->title : x->file, *ny = y->title[0] ? y->title : y->file;
    return strcasecmp(nx, ny);
}

static void scan_games(void)
{
    for (int i = 0; i < g_ncards; i++) {
        ui_card_free(&g_cards[i]);
    }
    g_ncards = 0;

    /* primero los nombres, para la barra de progreso */
    static char names[MAX_GAMES][256];
    int n = 0;
    mkdir(GAMES_DIR, 0777);
    DIR *dir = opendir(GAMES_DIR);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) && n < MAX_GAMES) {
            if (has_ext(de->d_name)) {
                snprintf(names[n++], sizeof(names[0]), "%s", de->d_name);
            }
        }
        closedir(dir);
    }

    g_ui.screen = UI_LOADING;
    g_ui.load_total = n;
    for (int i = 0; i < n; i++) {
        g_ui.load_done = i;
        g_ui.load_name = names[i];
        present();
        char path[512];
        snprintf(path, sizeof(path), GAMES_DIR "/%s", names[i]);
        struct stat st;
        unsigned long long size = stat(path, &st) == 0 ? (unsigned long long)st.st_size : 0;
        bool ok = game_open(&g_scan, path);
        ui_card_from_game(&g_cards[g_ncards++], &g_scan, names[i], size, ok);
        game_close(&g_scan);
    }
    qsort(g_cards, (size_t)g_ncards, sizeof(g_cards[0]), cmp_cards);
    g_ui.load_name = NULL;
    g_ui.ncards = g_ncards;
    g_ui.sel = 0;
    g_ui.first_row = 0;
    g_ui.screen = UI_LIBRARY;
}

static void keep_visible(void)
{
    int row = g_ui.sel / UI_COLS;
    if (row < g_ui.first_row) {
        g_ui.first_row = row;
    }
    if (row >= g_ui.first_row + UI_ROWS_VISIBLE) {
        g_ui.first_row = row - UI_ROWS_VISIBLE + 1;
    }
}

static void open_detail(void)
{
    char path[512];
    snprintf(path, sizeof(path), GAMES_DIR "/%s", g_cards[g_ui.sel].file);
    game_close(&g_game);
    game_open(&g_game, path);
    g_ui.game = &g_game;
    g_ui.page = 0;
    g_ui.scroll = 0;
    g_ui.screen = UI_DETAIL;
}

static int detail_items(void)
{
    switch (g_ui.page) {
    case 1: return (int)g_game.xbe.nsections;
    case 2: return (int)g_game.xbe.nlibs;
    case 3: return (int)g_game.nroot;
    default: return 0;
    }
}

/* Los controles de la Switch como el control del Xbox, por posición: B
 * (abajo) es la A del Xbox, A la B, Y la X, X la Y; L y R el blanco y el
 * negro; ZL y ZR los gatillos; + Start y - Back. */
static void pad_to_xbox(PadState *pad, u64 h, NcPadState *ps)
{
    memset(ps, 0, sizeof(*ps));
    if (h & HidNpadButton_Up) ps->buttons |= NC_PAD_DPAD_UP;
    if (h & HidNpadButton_Down) ps->buttons |= NC_PAD_DPAD_DOWN;
    if (h & HidNpadButton_Left) ps->buttons |= NC_PAD_DPAD_LEFT;
    if (h & HidNpadButton_Right) ps->buttons |= NC_PAD_DPAD_RIGHT;
    if (h & HidNpadButton_Plus) ps->buttons |= NC_PAD_START;
    if (h & HidNpadButton_Minus) ps->buttons |= NC_PAD_BACK;
    if (h & HidNpadButton_StickL) ps->buttons |= NC_PAD_LTHUMB;
    if (h & HidNpadButton_StickR) ps->buttons |= NC_PAD_RTHUMB;
    ps->analog[0] = (h & HidNpadButton_B) ? 0xFF : 0;  /* A */
    ps->analog[1] = (h & HidNpadButton_A) ? 0xFF : 0;  /* B */
    ps->analog[2] = (h & HidNpadButton_Y) ? 0xFF : 0;  /* X */
    ps->analog[3] = (h & HidNpadButton_X) ? 0xFF : 0;  /* Y */
    ps->analog[4] = (h & HidNpadButton_R) ? 0xFF : 0;  /* negro */
    ps->analog[5] = (h & HidNpadButton_L) ? 0xFF : 0;  /* blanco */
    ps->analog[6] = (h & HidNpadButton_ZL) ? 0xFF : 0; /* gatillo izquierdo */
    ps->analog[7] = (h & HidNpadButton_ZR) ? 0xFF : 0; /* gatillo derecho */
    HidAnalogStickState l = padGetStickPos(pad, 0), r = padGetStickPos(pad, 1);
    ps->lx = (int16_t)l.x;
    ps->ly = (int16_t)l.y;
    ps->rx = (int16_t)r.x;
    ps->ry = (int16_t)r.y;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    framebufferCreate(&g_fb, nwindowGetDefault(), UI_W, UI_H, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&g_fb);
    gfx_target(g_shadow, UI_W, UI_H, UI_W);

    /* La fuente del sistema de la Switch. */
    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        PlFontData font;
        if (R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) {
            gfx_font_init(font.address, font.size);
        }
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    apu_set_threaded(true); /* el audio en el núcleo 1 */
    s32 prev_touches = 0;

    g_ui.version = APP_VER;
    g_ui.cards = g_cards;
    g_ui.probe = &g_probe;
    scan_games();
    bool dirty = true;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 k = padGetButtonsDown(&pad);
        bool playing = g_ui.screen == UI_RUN && g_ui.run_screen;
        if ((k & HidNpadButton_Plus) && !playing) {
            break;
        }

        /* toque: posición del dedo al apoyarlo */
        int tx = -1, ty = -1;
        HidTouchScreenState ts = {0};
        if (hidGetTouchScreenStates(&ts, 1)) {
            if (ts.count > 0 && prev_touches == 0) {
                tx = (int)ts.touches[0].x;
                ty = (int)ts.touches[0].y;
            }
            prev_touches = ts.count;
        }

        switch (g_ui.screen) {
        case UI_LIBRARY: {
            int n = g_ncards;
            int sel = g_ui.sel;
            if (n > 0) {
                if (k & HidNpadButton_AnyRight) {
                    sel = sel + 1 < n ? sel + 1 : sel;
                }
                if (k & HidNpadButton_AnyLeft) {
                    sel = sel > 0 ? sel - 1 : sel;
                }
                if (k & HidNpadButton_AnyDown) {
                    sel = sel + UI_COLS < n ? sel + UI_COLS : sel;
                }
                if (k & HidNpadButton_AnyUp) {
                    sel = sel - UI_COLS >= 0 ? sel - UI_COLS : sel;
                }
            }
            if (sel != g_ui.sel) {
                g_ui.sel = sel;
                keep_visible();
                dirty = true;
            }
            int hit = tx >= 0 ? ui_card_at(&g_ui, tx, ty) : -1;
            if (hit >= 0 && hit != g_ui.sel) {
                g_ui.sel = hit;
                dirty = true;
            } else if ((hit >= 0 || (k & HidNpadButton_A)) && n > 0) {
                open_detail();
                dirty = true;
            } else if (k & HidNpadButton_Y) {
                scan_games();
                dirty = true;
            } else if (k & HidNpadButton_Minus) {
                g_ui.stage_back = UI_LIBRARY;
                g_ui.screen = UI_STAGES;
                dirty = true;
            } else if (k & HidNpadButton_X) {
                if (!g_probe_done) {
                    probe_run(&g_probe);
                    g_probe_done = true;
                }
                g_ui.screen = UI_PROBE;
                dirty = true;
            }
            break;
        }
        case UI_DETAIL: {
            if (k & (HidNpadButton_R | HidNpadButton_ZR)) {
                g_ui.page = (g_ui.page + 1) % UI_PAGES;
                g_ui.scroll = 0;
                dirty = true;
            }
            if (k & (HidNpadButton_L | HidNpadButton_ZL)) {
                g_ui.page = (g_ui.page + UI_PAGES - 1) % UI_PAGES;
                g_ui.scroll = 0;
                dirty = true;
            }
            int max = detail_items() - ui_detail_rows();
            if ((k & HidNpadButton_AnyDown) && g_ui.scroll < max) {
                g_ui.scroll++;
                dirty = true;
            }
            if ((k & HidNpadButton_AnyUp) && g_ui.scroll > 0) {
                g_ui.scroll--;
                dirty = true;
            }
            if ((k & HidNpadButton_Y) && g_cards[g_ui.sel].ok) {
                load_into_memory();
                dirty = true;
            } else if (k & HidNpadButton_B) {
                g_ui.screen = UI_LIBRARY;
                dirty = true;
            }
            break;
        }
        case UI_MEMORY: {
            int max = (int)g_load.nimports - ui_memory_rows();
            if ((k & HidNpadButton_AnyDown) && g_ui.load_scroll < max) {
                g_ui.load_scroll++;
                dirty = true;
            }
            if ((k & HidNpadButton_AnyUp) && g_ui.load_scroll > 0) {
                g_ui.load_scroll--;
                dirty = true;
            }
            if (k & (HidNpadButton_R | HidNpadButton_ZR)) {
                g_ui.load_scroll = g_ui.load_scroll + ui_memory_rows() < max ? g_ui.load_scroll + ui_memory_rows()
                                                                              : (max > 0 ? max : 0);
                dirty = true;
            }
            if (k & (HidNpadButton_L | HidNpadButton_ZL)) {
                g_ui.load_scroll = g_ui.load_scroll > ui_memory_rows() ? g_ui.load_scroll - ui_memory_rows() : 0;
                dirty = true;
            }
            if ((k & HidNpadButton_A) && g_load.ok) {
                start_run();
                dirty = true;
            } else if (k & HidNpadButton_B) {
                g_ui.screen = UI_DETAIL;
                dirty = true;
            }
            break;
        }
        case UI_RUN: {
            /* En la pantalla del Xbox los botones son del juego (L3 + R3
             * juntos vuelven al registro); en el registro, del emulador. */
            if (g_ui.run_screen) {
                u64 held = padGetButtons(&pad);
                NcPadState ps = {0};
                if (((held & HidNpadButton_StickL) && (held & HidNpadButton_StickR)) || tx >= 0) {
                    g_ui.run_screen = false;
                    dirty = true;
                } else {
                    pad_to_xbox(&pad, held, &ps);
                }
                nc_pad_set(0, &ps);
                k = 0;
            } else {
                NcPadState none = {0};
                nc_pad_set(0, &none);
                if (k & HidNpadButton_X) {
                    g_ui.run_screen = true;
                    dirty = true;
                    k = 0;
                }
                if (k & HidNpadButton_Y) {
                    g_nspi = g_nspi >= 8 ? 1 : g_nspi * 2;
                    g_kernel.ns_per_instr = g_nspi;
                    dirty = true;
                }
            }
            if (g_run.state == RUN_ACTIVE) {
                /* intérprete entre dibujos de la pantalla (dibujarla cuesta):
                 * ~60 ms viendo la del Xbox, ~150 ms en el registro */
                u64 budget = g_ui.run_screen ? 60000000ull : 150000000ull;
                u64 t0 = armGetSystemTick(), spent = 0;
                do {
                    runner_slice(&g_run, 100000);
                    spent = armTicksToNs(armGetSystemTick() - t0);
                } while (g_run.state == RUN_ACTIVE && spent < budget);
                g_run.ns += spent;
                dirty = true;
                if (k & HidNpadButton_B) {
                    g_run.state = RUN_CPU_STOP;
                    snprintf(g_run.end, sizeof(g_run.end), "Detenido por ti");
                }
            } else if (k & HidNpadButton_A) {
                start_run();
                dirty = true;
            } else if (k & HidNpadButton_B) {
                g_ui.screen = UI_MEMORY;
                dirty = true;
            }
            int rows = ui_run_rows();
            int max = (int)runner_log_len(&g_run) - rows;
            int cur = g_ui.run_scroll >= 0 ? g_ui.run_scroll : (max > 0 ? max : 0);
            if (k & (HidNpadButton_AnyUp | HidNpadButton_L | HidNpadButton_ZL)) {
                int step = (k & HidNpadButton_AnyUp) ? 1 : rows;
                g_ui.run_scroll = cur > step ? cur - step : 0;
                dirty = true;
            }
            if (k & (HidNpadButton_AnyDown | HidNpadButton_R | HidNpadButton_ZR)) {
                int step = (k & HidNpadButton_AnyDown) ? 1 : rows;
                g_ui.run_scroll = cur + step >= max ? -1 : cur + step;
                dirty = true;
            }
            break;
        }
        case UI_STAGES: {
            int max = ui_stage_count() - ui_stage_rows();
            if ((k & HidNpadButton_AnyDown) && g_ui.stage_scroll < max) {
                g_ui.stage_scroll++;
                dirty = true;
            }
            if ((k & HidNpadButton_AnyUp) && g_ui.stage_scroll > 0) {
                g_ui.stage_scroll--;
                dirty = true;
            }
            if (k & HidNpadButton_B) {
                g_ui.screen = (UiScreen)g_ui.stage_back;
                dirty = true;
            }
            break;
        }
        case UI_PROBE:
            if ((k & HidNpadButton_A) && !g_ui.exc_tried) {
                g_ui.exc_ok = probe_exception(&g_ui.exc_desc, &g_ui.exc_far);
                g_ui.exc_tried = true;
                dirty = true;
            }
            if (k & HidNpadButton_B) {
                g_ui.screen = UI_LIBRARY;
                dirty = true;
            }
            break;
        default:
            break;
        }

        if (dirty) {
            present();
            dirty = false;
        }
    }

    vm_free(&g_vm);
    game_close(&g_game);
    for (int i = 0; i < g_ncards; i++) {
        ui_card_free(&g_cards[i]);
    }
    framebufferClose(&g_fb);
    plExit();
    return 0;
}
