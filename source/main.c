/*
 * XboxNX — etapa 1: cargador.
 *
 * Lista las imágenes de Xbox en sdmc:/xbox/, abre la elegida, lee el
 * default.xbe y muestra lo que el emulador va a necesitar. Además corre
 * pruebas de hardware para la etapa 2.
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
#include "probe.h"

#define APP_VER   "0.1.0"
#define GAMES_DIR "sdmc:/xbox"
#define MAX_FILES 256
#define ROWS      36

/* colores ANSI de la consola de libnx */
#define C_RESET "\x1b[0m"
#define C_TITLE "\x1b[36;1m"
#define C_SEL   "\x1b[30;47m"
#define C_OK    "\x1b[32;1m"
#define C_BAD   "\x1b[31;1m"
#define C_WARN  "\x1b[33;1m"
#define C_DIM   "\x1b[37m"

typedef struct FileEntry {
    char name[256];
    unsigned long long size;
} FileEntry;

static FileEntry g_files[MAX_FILES];
static int g_nfiles;
static Game g_game;
static ProbeResult g_probe;
static bool g_probe_done;

static bool has_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".iso") || !strcasecmp(dot, ".xiso"));
}

static int cmp_files(const void *a, const void *b)
{
    return strcasecmp(((const FileEntry *)a)->name, ((const FileEntry *)b)->name);
}

static void scan_games(void)
{
    g_nfiles = 0;
    mkdir(GAMES_DIR, 0777);
    DIR *dir = opendir(GAMES_DIR);
    if (!dir) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) && g_nfiles < MAX_FILES) {
        if (!has_ext(de->d_name)) {
            continue;
        }
        FileEntry *f = &g_files[g_nfiles];
        snprintf(f->name, sizeof(f->name), "%s", de->d_name);
        char path[512];
        snprintf(path, sizeof(path), GAMES_DIR "/%s", de->d_name);
        struct stat st;
        f->size = stat(path, &st) == 0 ? (unsigned long long)st.st_size : 0;
        g_nfiles++;
    }
    closedir(dir);
    qsort(g_files, (size_t)g_nfiles, sizeof(g_files[0]), cmp_files);
}

static void header(const char *where)
{
    printf("\x1b[2J\x1b[1;1H");
    printf(C_TITLE "XboxNX %s" C_RESET " - etapa 1: cargador    " C_DIM "%s" C_RESET "\n", APP_VER, where);
    printf("--------------------------------------------------------------------------------\n");
}

static void fmt_size(unsigned long long b, char *out, size_t cap)
{
    if (b >= (1ull << 30)) {
        snprintf(out, cap, "%.2f GiB", (double)b / (1ull << 30));
    } else if (b >= (1ull << 20)) {
        snprintf(out, cap, "%.1f MiB", (double)b / (1ull << 20));
    } else {
        snprintf(out, cap, "%llu KiB", b >> 10);
    }
}

/* ---- pantalla: lista de juegos ---- */

static void draw_list(int sel, int top)
{
    header("Juegos en sdmc:/xbox/");
    if (g_nfiles == 0) {
        printf("\nNo hay imagenes .iso / .xiso en " C_WARN "sdmc:/xbox/" C_RESET "\n\n");
        printf("Copia tu ISO (de un disco tuyo) a esa carpeta y pulsa Y para buscar otra vez.\n");
        printf("Si la ISO pesa mas de 4 GiB, la tarjeta SD tiene que estar en exFAT.\n");
    }
    for (int i = top; i < g_nfiles && i < top + ROWS; i++) {
        char sz[32];
        fmt_size(g_files[i].size, sz, sizeof(sz));
        printf("%s %-62.62s %12s " C_RESET "\n", i == sel ? C_SEL : "", g_files[i].name, sz);
    }
    printf("\x1b[44;1H" C_DIM "A: abrir   Y: buscar otra vez   X: pruebas de hardware   +: salir" C_RESET);
}

/* ---- pantalla: información del juego ---- */

static const char *k_pages[] = {"General", "Secciones", "Librerias", "Archivos"};
#define NPAGES 4

static void page_general(void)
{
    const Game *g = &g_game;
    const XbeInfo *x = &g->xbe;
    char reg[64];
    xbe_region_str(x->region, reg, sizeof(reg));
    char tid[16];
    snprintf(tid, sizeof(tid), "%c%c-%03u", (char)(x->title_id >> 24), (char)(x->title_id >> 16),
             (unsigned)(x->title_id & 0xFFFF));
    printf("Juego:            " C_OK "%s" C_RESET "\n", x->title_name[0] ? x->title_name : "(sin nombre)");
    printf("Title ID:         %08X  (%s)\n", (unsigned)x->title_id, tid);
    printf("Region:           %s\n", reg);
    printf("Version:          %u\n", (unsigned)x->version);
    printf("Imagen:           %s (particion en +0x%llX)\n", xdisc_kind(&g->disc),
           (unsigned long long)g->disc.part_off);
    printf("default.xbe:      %u bytes\n", (unsigned)g->xbe_file.size);
    printf("Claves:           %s\n", x->debug_keys ? "debug (kit de desarrollo)" : "retail");
    printf("Base:             0x%08X\n", (unsigned)x->base);
    printf("Tamano en RAM:    %u KiB\n", (unsigned)(x->sizeof_image >> 10));
    printf("Punto de entrada: 0x%08X\n", (unsigned)x->entry);
    printf("Tabla kernel:     0x%08X\n", (unsigned)x->kernel_thunk);
    printf("Imports kernel:   %u (ordinal mas alto %u de 366)\n", g->kernel_imports, (unsigned)g->max_ordinal);
    unsigned xdk = game_xdk_build(g);
    if (xdk) {
        printf("XDK:              %u\n", xdk);
    }
    printf("Secciones:        %u      Librerias: %u\n\n", x->nsections, x->nlibs);

    bool fits = x->sizeof_image <= 64u * 1024 * 1024;
    printf("Revision de etapa 1:\n");
    printf("  %s Disco leido\n", g->disc_ok ? C_OK "[OK]" C_RESET : C_BAD "[X]" C_RESET);
    printf("  %s Ejecutable de Xbox valido\n", g->xbe_rc == XBE_OK ? C_OK "[OK]" C_RESET : C_BAD "[X]" C_RESET);
    printf("  %s Cabe en los 64 MiB del Xbox\n", fits ? C_OK "[OK]" C_RESET : C_BAD "[X]" C_RESET);
    printf("  %s Tabla del kernel encontrada\n", g->kernel_imports ? C_OK "[OK]" C_RESET : C_WARN "[?]" C_RESET);
    printf("\nEtapa 2 (siguiente): cargar esto en memoria y ejecutar el codigo x86.\n");
}

static void page_sections(int top)
{
    const XbeInfo *x = &g_game.xbe;
    printf(C_DIM "%-9s %-10s %-10s %-10s %-10s %s" C_RESET "\n", "Nombre", "VA", "Tam. RAM", "Offset", "Tam. disco",
           "Flags");
    for (unsigned i = (unsigned)top; i < x->nsections && i < (unsigned)top + ROWS - 2; i++) {
        const XbeSection *s = &x->sections[i];
        printf("%-9s 0x%08X 0x%08X 0x%08X 0x%08X %c%c%c\n", s->name, (unsigned)s->va, (unsigned)s->vsize,
               (unsigned)s->raw_off, (unsigned)s->raw_size, s->flags & XBE_SEC_WRITABLE ? 'W' : '-',
               s->flags & XBE_SEC_EXECUTABLE ? 'X' : '-', s->flags & XBE_SEC_PRELOAD ? 'P' : '-');
    }
}

static void page_libs(int top)
{
    const XbeInfo *x = &g_game.xbe;
    printf(C_DIM "%-10s %s" C_RESET "\n", "Libreria", "Version");
    for (unsigned i = (unsigned)top; i < x->nlibs && i < (unsigned)top + ROWS - 2; i++) {
        const XbeLib *l = &x->libs[i];
        printf("%-10s %u.%u.%u%s%s\n", l->name, l->major, l->minor, l->build, l->qfe ? " QFE" : "",
               l->debug_build ? " (debug)" : "");
    }
    printf("\nD3D8 es la libreria grafica: la etapa 4 traduce lo que manda a la GPU.\n");
}

static void page_files(int top)
{
    const Game *g = &g_game;
    printf(C_DIM "Raiz del disco (%u entradas)" C_RESET "\n", g->nroot);
    for (unsigned i = (unsigned)top; i < g->nroot && i < (unsigned)top + ROWS - 2; i++) {
        const GameRootEntry *e = &g->root[i];
        char sz[32];
        fmt_size(e->size, sz, sizeof(sz));
        printf("%s%-50.50s %12s" C_RESET "\n", e->is_dir ? C_TITLE : "", e->name, e->is_dir ? "<carpeta>" : sz);
    }
}

static void draw_game(const char *file, int page, int top)
{
    char where[96];
    snprintf(where, sizeof(where), "%.60s", file);
    header(where);
    for (int i = 0; i < NPAGES; i++) {
        printf("%s %s " C_RESET " ", i == page ? C_SEL : C_DIM, k_pages[i]);
    }
    printf("\n\n");
    if (g_game.xbe_rc != XBE_OK) {
        printf(C_BAD "%s" C_RESET "\n", g_game.error);
        if (g_game.disc_ok && page == 3) {
            page_files(top);
        }
    } else {
        switch (page) {
        case 0: page_general(); break;
        case 1: page_sections(top); break;
        case 2: page_libs(top); break;
        case 3: page_files(top); break;
        }
    }
    printf("\x1b[44;1H" C_DIM "L/R: pagina   Arriba/Abajo: desplazar   B: volver   +: salir" C_RESET);
}

/* ---- pantalla: pruebas de hardware ---- */

static void draw_probe(bool exc_tried, bool exc_ok, uint32_t exc_desc, uint64_t exc_far)
{
    header("Pruebas de hardware para la etapa 2");
    const ProbeResult *r = &g_probe;
    char a[32], b[32], c[32];
    fmt_size(r->mem_total, a, sizeof(a));
    fmt_size(r->mem_used, b, sizeof(b));
    fmt_size(r->biggest_alloc, c, sizeof(c));

    printf("Modo:      %s\n", r->applet_mode
                                  ? C_WARN "Applet (abierto desde el Album) - poca RAM" C_RESET
                                  : C_OK "Aplicacion completa (title takeover)" C_RESET);
    printf("RAM:       total %s, en uso %s, bloque mas grande %s\n", a, b, c);
    bool ram_ok = r->biggest_alloc >= (1024ull << 20);
    printf("  %s Al menos 1 GiB libre para el emulador\n", ram_ok ? C_OK "[OK]" C_RESET : C_BAD "[X]" C_RESET);
    if (!ram_ok && r->applet_mode) {
        printf("    Abre XboxNX manteniendo R sobre un juego (title takeover) para tener ~3 GiB.\n");
    }
    printf("  %s JIT (generar y ejecutar codigo ARM64): ", r->jit_ok ? C_OK "[OK]" C_RESET : C_BAD "[X]" C_RESET);
    if (r->jit_ok) {
        printf("devolvio %d\n", r->jit_value);
    } else {
        printf("error 0x%X\n", (unsigned)r->jit_rc);
    }
    printf("  %s Ventana de 4 GiB para la memoria del Xbox\n", r->va4g_ok ? C_OK "[OK]" C_RESET : C_BAD "[X]" C_RESET);
    if (!exc_tried) {
        printf("  " C_WARN "[ ]" C_RESET " Atrapar accesos invalidos: pulsa A para probar\n");
        printf("      (si la Switch no lo permite, la app se cierra: avisame si pasa)\n");
    } else {
        printf("  %s Atrapar accesos invalidos", exc_ok ? C_OK "[OK]" C_RESET : C_BAD "[X]" C_RESET);
        if (exc_ok) {
            printf(" (tipo 0x%X, direccion 0x%llX)", (unsigned)exc_desc, (unsigned long long)exc_far);
        }
        printf("\n");
    }
    printf("\nToma una foto de esta pantalla y mandamela: con esto se decide la etapa 2.\n");
    printf("\x1b[44;1H" C_DIM "A: probar excepciones   B: volver   +: salir" C_RESET);
}

/* ---- bucle principal ---- */

enum { SCR_LIST, SCR_GAME, SCR_PROBE };

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    scan_games();
    int screen = SCR_LIST, sel = 0, top = 0, page = 0, scroll = 0;
    bool exc_tried = false, exc_ok = false;
    uint32_t exc_desc = 0;
    uint64_t exc_far = 0;
    bool dirty = true;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 k = padGetButtonsDown(&pad);
        if (k & HidNpadButton_Plus) {
            break;
        }

        if (screen == SCR_LIST) {
            if ((k & HidNpadButton_AnyDown) && sel + 1 < g_nfiles) {
                sel++;
                dirty = true;
            }
            if ((k & HidNpadButton_AnyUp) && sel > 0) {
                sel--;
                dirty = true;
            }
            if (sel < top) {
                top = sel;
            }
            if (sel >= top + ROWS) {
                top = sel - ROWS + 1;
            }
            if (k & HidNpadButton_Y) {
                scan_games();
                sel = top = 0;
                dirty = true;
            }
            if ((k & HidNpadButton_A) && g_nfiles > 0) {
                header("Abriendo...");
                printf("Leyendo %s ...\n", g_files[sel].name);
                consoleUpdate(NULL);
                char path[512];
                snprintf(path, sizeof(path), GAMES_DIR "/%s", g_files[sel].name);
                game_close(&g_game);
                game_open(&g_game, path);
                screen = SCR_GAME;
                page = scroll = 0;
                dirty = true;
            } else if (k & HidNpadButton_X) {
                if (!g_probe_done) {
                    header("Probando...");
                    consoleUpdate(NULL);
                    probe_run(&g_probe);
                    g_probe_done = true;
                }
                screen = SCR_PROBE;
                dirty = true;
            }
        } else if (screen == SCR_GAME) {
            if (k & HidNpadButton_R) {
                page = (page + 1) % NPAGES;
                scroll = 0;
                dirty = true;
            }
            if (k & HidNpadButton_L) {
                page = (page + NPAGES - 1) % NPAGES;
                scroll = 0;
                dirty = true;
            }
            if (k & HidNpadButton_AnyDown) {
                scroll++;
                dirty = true;
            }
            if ((k & HidNpadButton_AnyUp) && scroll > 0) {
                scroll--;
                dirty = true;
            }
            if (k & HidNpadButton_B) {
                screen = SCR_LIST;
                dirty = true;
            }
        } else {
            if ((k & HidNpadButton_A) && !exc_tried) {
                exc_ok = probe_exception(&exc_desc, &exc_far);
                exc_tried = true;
                dirty = true;
            }
            if (k & HidNpadButton_B) {
                screen = SCR_LIST;
                dirty = true;
            }
        }

        if (dirty) {
            switch (screen) {
            case SCR_LIST: draw_list(sel, top); break;
            case SCR_GAME: draw_game(g_files[sel].name, page, scroll); break;
            default: draw_probe(exc_tried, exc_ok, exc_desc, exc_far); break;
            }
            consoleUpdate(NULL);
            dirty = false;
        }
    }

    game_close(&g_game);
    consoleExit(NULL);
    return 0;
}
