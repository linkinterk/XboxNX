/*
 * XboxNX — pantallas.
 *
 * Colores del tema oscuro de hakuX (ui/theme/Theme.kt): fondo casi negro,
 * superficies grises y el verde de la época de Xbox aclarado para leerse.
 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"

#define C_BG        HEX(0x121212)
#define C_SURF      HEX(0x1F1F1F)
#define C_SURF_HI   HEX(0x262626)
#define C_SURF_TOP  HEX(0x2F2F2F)
#define C_VARIANT   HEX(0x2B2B2B)
#define C_ON        HEX(0xE6E6E6)
#define C_ON_VAR    HEX(0xB4B8BC)
#define C_OUTLINE   HEX(0x3A3A3A)
#define C_GREEN     HEX(0x81C784)
#define C_GREEN_DK  HEX(0x1B5E20)
#define C_GREEN_MID HEX(0x2E7D32)
#define C_ON_GREEN  HEX(0x003A0B)
#define C_GREEN_BG  HEX(0x23392A)
#define C_ERROR     HEX(0xEF9A9A)
#define C_ERROR_BG  HEX(0x3B2222)
#define C_WARN      HEX(0xFFD54F)
#define C_WARN_BG   HEX(0x3A3320)

/* rejilla de la biblioteca */
#define GRID_X   40
#define GRID_Y   104
#define CARD_W   590
#define CARD_H   150
#define GAP      20
#define TOPBAR_H 84
#define BOTBAR_Y 656

/* ---- piezas ---- */

static void app_logo(int x, int y, int s)
{
    gfx_rrect(x, y, s, s, s / 5, C_GREEN_MID);
    gfx_rrect(x, y + s / 2, s, s - s / 2, s / 5, C_GREEN_DK);
    gfx_rect(x, y + s / 2, s, s / 5, C_GREEN_DK);
    gfx_rrect(x, y, s, s / 2 + 2, s / 5, WITH_ALPHA(C_GREEN, 70));
    int fs = s * 46 / 100;
    gfx_text_center(x, s, y + (s - fs) / 2 - s / 30, fs, HEX(0xFFFFFF), "NX");
}

/* Botón del control: círculo claro con la letra. */
static int button_hint(int x, int y, const char *btn, const char *label)
{
    int r = 15;
    gfx_circle(x + r, y + r, r, C_ON);
    gfx_text_center(x, 2 * r, y + 4, 21, C_BG, btn);
    int w = gfx_text(x + 2 * r + 10, y + 3, 22, C_ON, label);
    return 2 * r + 10 + w + 34;
}

typedef struct Hint {
    const char *btn, *label;
} Hint;

static void bottom_bar(const Hint *h, int n)
{
    gfx_rect(0, BOTBAR_Y, UI_W, 1, C_OUTLINE);
    /* de derecha a izquierda, como en la Switch */
    int widths[8], total = 0;
    for (int i = 0; i < n; i++) {
        widths[i] = 30 + 10 + gfx_text_width(22, h[i].label) + 34;
        total += widths[i];
    }
    int x = UI_W - 40 - total + 34;
    for (int i = 0; i < n; i++) {
        x += button_hint(x, BOTBAR_Y + 18, h[i].btn, h[i].label);
    }
}

static void top_bar(const UiState *s, const char *subtitle)
{
    gfx_vgradient(0, 0, UI_W, TOPBAR_H, HEX(0x1A1A1A), C_BG);
    app_logo(40, 18, 48);
    gfx_text(104, 16, 30, C_ON, "XboxNX");
    if (subtitle) {
        gfx_text(104, 52, 18, C_ON_VAR, subtitle);
    }
    char chip[48];
    snprintf(chip, sizeof(chip), "Etapa 1  ·  v%s", s->version);
    int w = gfx_text_width(18, chip) + 32;
    gfx_rrect(UI_W - 40 - w, 26, w, 34, 17, C_GREEN_BG);
    gfx_text(UI_W - 40 - w + 16, 33, 18, C_GREEN, chip);
}

static void chip(int x, int y, const char *text, uint32_t bg, uint32_t fg)
{
    int w = gfx_text_width(16, text) + 20;
    gfx_rrect(x, y, w, 26, 13, bg);
    gfx_text(x + 10, y + 3, 16, fg, text);
}

static int chip_w(const char *text) { return gfx_text_width(16, text) + 20; }

static void fmt_size(unsigned long long b, char *out, size_t cap)
{
    if (b >= (1ull << 30)) {
        snprintf(out, cap, "%.2f GB", (double)b / (1ull << 30));
    } else if (b >= (1ull << 20)) {
        snprintf(out, cap, "%.1f MB", (double)b / (1ull << 20));
    } else {
        snprintf(out, cap, "%llu KB", b >> 10);
    }
}

static void region_text(uint32_t r, char *out, size_t cap)
{
    out[0] = 0;
    if (r & 1) {
        strncat(out, "NA ", cap - strlen(out) - 1);
    }
    if (r & 2) {
        strncat(out, "JP ", cap - strlen(out) - 1);
    }
    if (r & 4) {
        strncat(out, "PAL ", cap - strlen(out) - 1);
    }
    size_t n = strlen(out);
    if (n && out[n - 1] == ' ') {
        out[n - 1] = 0;
    }
    if (!out[0]) {
        snprintf(out, cap, "—");
    }
}

/* Miniatura: la imagen del juego, o sus iniciales sobre gris. */
static void thumb(const XImage *img, const char *title, int x, int y, int s, int r)
{
    if (img && img->px) {
        gfx_rrect(x, y, s, s, r, HEX(0x000000));
        gfx_image(img, x, y, s, s, r);
        return;
    }
    gfx_rrect(x, y, s, s, r, C_VARIANT);
    char ini[3] = {0};
    int n = 0;
    for (const char *p = title; *p && n < 2; p++) {
        if ((p == title || p[-1] == ' ') && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                                             (*p >= '0' && *p <= '9'))) {
            ini[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        }
    }
    if (!n) {
        ini[0] = '?';
    }
    int fs = s * 2 / 5;
    gfx_text_center(x, s, y + (s - fs) / 2 - fs / 12, fs, C_ON_VAR, ini);
}

/* Insignia de estado: OK / FALLA / PENDIENTE. */
static int badge(int x, int y, int state)
{
    static const char *txt[] = {"OK", "FALLA", "PENDIENTE", "?"};
    uint32_t bg = state == 0 ? C_GREEN_BG : state == 1 ? C_ERROR_BG : C_WARN_BG;
    uint32_t fg = state == 0 ? C_GREEN : state == 1 ? C_ERROR : C_WARN;
    int w = 130;
    gfx_rrect(x, y, w, 30, 15, bg);
    gfx_text_center(x, w, y + 4, 18, fg, txt[state]);
    return w;
}

/* ---- cargando ---- */

static void draw_loading(const UiState *s)
{
    top_bar(s, "Emulador de Xbox original para Nintendo Switch");
    app_logo(UI_W / 2 - 60, 220, 120);
    gfx_text_center(0, UI_W, 370, 30, C_ON, "Leyendo tus juegos...");
    int bw = 520, bx = (UI_W - bw) / 2, by = 430;
    gfx_rrect(bx, by, bw, 12, 6, C_SURF_HI);
    if (s->load_total > 0) {
        int fw = bw * s->load_done / s->load_total;
        if (fw > 0) {
            gfx_rrect(bx, by, fw < 12 ? 12 : fw, 12, 6, C_GREEN);
        }
    }
    if (s->load_name) {
        char line[300];
        snprintf(line, sizeof(line), "%d de %d  ·  %s", s->load_done + 1, s->load_total, s->load_name);
        gfx_text_center(0, UI_W, 462, 20, C_ON_VAR, line);
    }
}

/* ---- biblioteca ---- */

static void card_rect(int i, int first_row, int *x, int *y)
{
    int row = i / UI_COLS - first_row, col = i % UI_COLS;
    *x = GRID_X + col * (CARD_W + GAP);
    *y = GRID_Y + row * (CARD_H + GAP);
}

int ui_card_at(const UiState *s, int px, int py)
{
    if (s->screen != UI_LIBRARY) {
        return -1;
    }
    for (int i = s->first_row * UI_COLS; i < s->ncards && i < (s->first_row + UI_ROWS_VISIBLE) * UI_COLS; i++) {
        int x, y;
        card_rect(i, s->first_row, &x, &y);
        if (px >= x && px < x + CARD_W && py >= y && py < y + CARD_H) {
            return i;
        }
    }
    return -1;
}

static void draw_card(const UiCard *c, int x, int y, bool sel)
{
    gfx_rrect(x, y, CARD_W, CARD_H, 18, sel ? C_SURF_TOP : C_SURF);
    if (sel) {
        gfx_rrect_outline(x - 4, y - 4, CARD_W + 8, CARD_H + 8, 22, 4, C_GREEN);
    }
    int ts = CARD_H - 30;
    thumb(&c->icon, c->title[0] ? c->title : c->file, x + 15, y + 15, ts, 14);
    int tx = x + 15 + ts + 20, tw = CARD_W - (tx - x) - 20;
    const char *title = c->title[0] ? c->title : c->file;
    gfx_text_fit(tx, y + 18, 27, tw, C_ON, title);
    if (c->ok) {
        char reg[24], line[96];
        region_text(c->region, reg, sizeof(reg));
        snprintf(line, sizeof(line), "%08X  ·  %s  ·  v%u", (unsigned)c->title_id, reg, (unsigned)c->version);
        gfx_text_fit(tx, y + 56, 19, tw, C_ON_VAR, line);
    } else {
        gfx_text_fit(tx, y + 56, 19, tw, C_ERROR, c->error[0] ? c->error : "No se pudo leer");
    }
    char sz[24];
    fmt_size(c->size, sz, sizeof(sz));
    int cx = tx;
    if (c->kind) {
        chip(cx, y + CARD_H - 44, c->kind, C_GREEN_BG, C_GREEN);
        cx += chip_w(c->kind) + 8;
    }
    chip(cx, y + CARD_H - 44, sz, C_VARIANT, C_ON_VAR);
}

static void draw_library(const UiState *s)
{
    char sub[64];
    snprintf(sub, sizeof(sub), "%d %s en sd:/xbox/", s->ncards, s->ncards == 1 ? "juego" : "juegos");
    top_bar(s, s->ncards ? sub : "Emulador de Xbox original para Nintendo Switch");

    if (s->ncards == 0) {
        int px = (UI_W - 760) / 2, py = 170;
        gfx_rrect(px, py, 760, 380, 24, C_SURF);
        gfx_circle(UI_W / 2, py + 90, 48, C_GREEN_BG);
        gfx_text_center(px, 760, py + 62, 46, C_GREEN, "+");
        gfx_text_center(px, 760, py + 160, 30, C_ON, "No hay juegos todavía");
        gfx_text_center(px, 760, py + 210, 21, C_ON_VAR, "Copia tu ISO (.iso o .xiso, de un disco tuyo) a");
        gfx_text_center(px, 760, py + 240, 21, C_GREEN, "sd:/xbox/");
        gfx_text_center(px, 760, py + 290, 18, C_ON_VAR, "Si pesa más de 4 GB, la tarjeta SD tiene que estar en exFAT.");
        Hint h[] = {{"Y", "Buscar"}, {"X", "Pruebas"}, {"+", "Salir"}};
        bottom_bar(h, 3);
        return;
    }

    for (int i = s->first_row * UI_COLS; i < s->ncards && i < (s->first_row + UI_ROWS_VISIBLE) * UI_COLS; i++) {
        int x, y;
        card_rect(i, s->first_row, &x, &y);
        draw_card(&s->cards[i], x, y, i == s->sel);
    }
    /* barra de desplazamiento */
    int rows = (s->ncards + UI_COLS - 1) / UI_COLS;
    if (rows > UI_ROWS_VISIBLE) {
        int th = 3 * CARD_H + 2 * GAP;
        int bh = th * UI_ROWS_VISIBLE / rows, by = GRID_Y + (th - bh) * s->first_row / (rows - UI_ROWS_VISIBLE);
        gfx_rrect(UI_W - 18, GRID_Y, 6, th, 3, C_SURF_HI);
        gfx_rrect(UI_W - 18, by, 6, bh, 3, C_GREEN);
    }
    Hint h[] = {{"A", "Abrir"}, {"Y", "Buscar"}, {"X", "Pruebas"}, {"+", "Salir"}};
    bottom_bar(h, 4);
}

/* ---- detalle ---- */

static const char *k_pages[UI_PAGES] = {"General", "Secciones", "Librerías", "Archivos"};

#define PANEL_X 440
#define PANEL_Y 168
#define PANEL_W (UI_W - PANEL_X - 40)
#define PANEL_H (BOTBAR_Y - PANEL_Y - 20)
#define ROW_H   38

int ui_detail_rows(void) { return (PANEL_H - 70) / ROW_H; }

static void kv(int *y, const char *k, const char *v, uint32_t vc)
{
    gfx_text(PANEL_X + 28, *y, 20, C_ON_VAR, k);
    gfx_text_fit(PANEL_X + 250, *y, 20, PANEL_W - 280, vc, v);
    *y += 33;
}

static void page_general(const UiState *s)
{
    const Game *g = s->game;
    const XbeInfo *x = &g->xbe;
    char b[96];
    int y = PANEL_Y + 24;
    snprintf(b, sizeof(b), "%s  (partición en +0x%llX)", xdisc_kind(&g->disc), (unsigned long long)g->disc.part_off);
    kv(&y, "Imagen", b, C_ON);
    snprintf(b, sizeof(b), "%u bytes", (unsigned)g->xbe_file.size);
    kv(&y, "default.xbe", b, C_ON);
    kv(&y, "Claves", x->debug_keys ? "Debug (kit de desarrollo)" : "Retail", C_ON);
    snprintf(b, sizeof(b), "0x%08X  ·  %u KB en RAM", (unsigned)x->base, (unsigned)(x->sizeof_image >> 10));
    kv(&y, "Base", b, C_ON);
    snprintf(b, sizeof(b), "0x%08X", (unsigned)x->entry);
    kv(&y, "Punto de entrada", b, C_ON);
    snprintf(b, sizeof(b), "%u funciones (ordinal más alto %u de 366)", g->kernel_imports, (unsigned)g->max_ordinal);
    kv(&y, "Kernel", b, C_ON);
    unsigned xdk = game_xdk_build(g);
    if (xdk) {
        snprintf(b, sizeof(b), "%u", xdk);
        kv(&y, "Versión del XDK", b, C_ON);
    }
    snprintf(b, sizeof(b), "%u secciones  ·  %u librerías", x->nsections, x->nlibs);
    kv(&y, "Contenido", b, C_ON);

    y += 10;
    gfx_rect(PANEL_X + 28, y, PANEL_W - 56, 1, C_OUTLINE);
    y += 16;
    gfx_text(PANEL_X + 28, y, 22, C_ON, "Revisión de etapa 1");
    y += 38;
    struct {
        const char *what;
        int st;
    } checks[] = {
        {"Disco leído", g->disc_ok ? 0 : 1},
        {"Ejecutable de Xbox válido", g->xbe_rc == XBE_OK ? 0 : 1},
        {"Cabe en los 64 MB", x->sizeof_image <= 64u * 1024 * 1024 ? 0 : 1},
        {"Tabla del kernel encontrada", g->kernel_imports ? 0 : 2},
    };
    for (unsigned i = 0; i < 4; i++) {
        int col = (int)(i % 2), row = (int)(i / 2);
        int cx = PANEL_X + 28 + col * (PANEL_W - 56) / 2, cy = y + row * 42;
        badge(cx, cy, checks[i].st);
        gfx_text_fit(cx + 144, cy + 3, 20, (PANEL_W - 56) / 2 - 156, C_ON, checks[i].what);
    }
}

static void table_header(int y, const char *const *cols, const int *xs, int n)
{
    gfx_rect(PANEL_X, y - 6, PANEL_W, 40, C_SURF_HI);
    for (int i = 0; i < n; i++) {
        gfx_text(PANEL_X + xs[i], y + 2, 18, C_ON_VAR, cols[i]);
    }
}

static void page_sections(const UiState *s)
{
    const XbeInfo *x = &s->game->xbe;
    static const char *cols[] = {"Nombre", "Dirección", "Tamaño", "En disco", "Permisos"};
    static const int xs[] = {28, 190, 340, 450, 560};
    int y = PANEL_Y + 20;
    table_header(y, cols, xs, 5);
    y += 46;
    for (int i = s->scroll; i < (int)x->nsections && i < s->scroll + ui_detail_rows(); i++) {
        const XbeSection *sec = &x->sections[i];
        if ((i - s->scroll) % 2) {
            gfx_rect(PANEL_X, y - 5, PANEL_W, ROW_H, HEX(0x232323));
        }
        char b[32];
        gfx_text_fit(PANEL_X + xs[0], y, 19, 150, C_ON, sec->name);
        snprintf(b, sizeof(b), "0x%08X", (unsigned)sec->va);
        gfx_text(PANEL_X + xs[1], y, 19, C_ON_VAR, b);
        snprintf(b, sizeof(b), "%u KB", (unsigned)((sec->vsize + 1023) >> 10));
        gfx_text(PANEL_X + xs[2], y, 19, C_ON_VAR, b);
        snprintf(b, sizeof(b), "%u KB", (unsigned)((sec->raw_size + 1023) >> 10));
        gfx_text(PANEL_X + xs[3], y, 19, C_ON_VAR, b);
        int cx = PANEL_X + xs[4];
        if (sec->flags & XBE_SEC_EXECUTABLE) {
            chip(cx, y - 2, "código", C_GREEN_BG, C_GREEN);
            cx += chip_w("código") + 6;
        }
        if (sec->flags & XBE_SEC_WRITABLE) {
            chip(cx, y - 2, "escribe", C_VARIANT, C_ON_VAR);
            cx += chip_w("escribe") + 6;
        }
        if (sec->flags & XBE_SEC_PRELOAD) {
            chip(cx, y - 2, "carga", C_VARIANT, C_ON_VAR);
        }
        y += ROW_H;
    }
}

static void page_libs(const UiState *s)
{
    const XbeInfo *x = &s->game->xbe;
    static const char *cols[] = {"Librería", "Versión", "Nota"};
    static const int xs[] = {28, 260, 460};
    int y = PANEL_Y + 20;
    table_header(y, cols, xs, 3);
    y += 46;
    for (int i = s->scroll; i < (int)x->nlibs && i < s->scroll + ui_detail_rows(); i++) {
        const XbeLib *l = &x->libs[i];
        if ((i - s->scroll) % 2) {
            gfx_rect(PANEL_X, y - 5, PANEL_W, ROW_H, HEX(0x232323));
        }
        char b[48];
        gfx_text(PANEL_X + xs[0], y, 19, C_ON, l->name);
        snprintf(b, sizeof(b), "%u.%u.%u%s", l->major, l->minor, l->build, l->debug_build ? " debug" : "");
        gfx_text(PANEL_X + xs[1], y, 19, C_ON_VAR, b);
        if (!strncmp(l->name, "D3D8", 4)) {
            chip(PANEL_X + xs[2], y - 2, "gráficos · etapa 4", C_GREEN_BG, C_GREEN);
        } else if (!strncmp(l->name, "DSOUND", 6)) {
            chip(PANEL_X + xs[2], y - 2, "audio · etapa 3", C_VARIANT, C_ON_VAR);
        } else if (!strncmp(l->name, "XAPILIB", 7)) {
            chip(PANEL_X + xs[2], y - 2, "sistema · etapa 3", C_VARIANT, C_ON_VAR);
        }
        y += ROW_H;
    }
}

static void page_files(const UiState *s)
{
    const Game *g = s->game;
    static const char *cols[] = {"Raíz del disco", "Tamaño"};
    static const int xs[] = {28, 600};
    int y = PANEL_Y + 20;
    table_header(y, cols, xs, 2);
    y += 46;
    for (int i = s->scroll; i < (int)g->nroot && i < s->scroll + ui_detail_rows(); i++) {
        const GameRootEntry *e = &g->root[i];
        if ((i - s->scroll) % 2) {
            gfx_rect(PANEL_X, y - 5, PANEL_W, ROW_H, HEX(0x232323));
        }
        if (e->is_dir) {
            gfx_rrect(PANEL_X + 28, y + 2, 22, 17, 4, C_GREEN);
            gfx_rrect(PANEL_X + 28, y - 1, 11, 8, 3, C_GREEN);
            gfx_text_fit(PANEL_X + 62, y, 19, 520, C_ON, e->name);
            gfx_text(PANEL_X + xs[1], y, 19, C_ON_VAR, "carpeta");
        } else {
            char b[24];
            fmt_size(e->size, b, sizeof(b));
            gfx_text_fit(PANEL_X + 62, y, 19, 520, C_ON, e->name);
            gfx_text(PANEL_X + xs[1], y, 19, C_ON_VAR, b);
        }
        y += ROW_H;
    }
}

static void draw_detail(const UiState *s)
{
    const Game *g = s->game;
    const UiCard *c = &s->cards[s->sel];
    top_bar(s, c->file);

    /* panel izquierdo: imagen y datos */
    gfx_rrect(40, 104, 376, BOTBAR_Y - 124, 22, C_SURF);
    thumb(&c->icon, c->title[0] ? c->title : c->file, 88, 132, 280, 20);
    const char *title = c->title[0] ? c->title : c->file;
    gfx_text_fit(64, 432, 28, 328, C_ON, title);
    if (c->ok) {
        char b[64], reg[24];
        const XbeInfo *x = &g->xbe;
        snprintf(b, sizeof(b), "%c%c-%03u  ·  %08X", (char)(x->title_id >> 24), (char)(x->title_id >> 16),
                 (unsigned)(x->title_id & 0xFFFF), (unsigned)x->title_id);
        gfx_text_fit(64, 474, 19, 328, C_ON_VAR, b);
        region_text(x->region, reg, sizeof(reg));
        snprintf(b, sizeof(b), "Región %s  ·  versión %u", reg, (unsigned)x->version);
        gfx_text_fit(64, 502, 19, 328, C_ON_VAR, b);
        chip(64, 548, xdisc_kind(&g->disc), C_GREEN_BG, C_GREEN);
        chip(64, 584, "Etapa 2: ejecutar el juego (pendiente)", C_VARIANT, C_ON_VAR);
    }

    /* pestañas */
    int tx = PANEL_X;
    int r = 15;
    gfx_circle(tx + r, 116 + r, r, C_ON);
    gfx_text_center(tx, 2 * r, 120, 20, C_BG, "L");
    tx += 2 * r + 14;
    for (int i = 0; i < UI_PAGES; i++) {
        int w = gfx_text_width(21, k_pages[i]) + 36;
        bool on = i == s->page;
        gfx_rrect(tx, 110, w, 42, 21, on ? C_GREEN : C_SURF);
        gfx_text(tx + 18, 119, 21, on ? C_ON_GREEN : C_ON_VAR, k_pages[i]);
        tx += w + 10;
    }
    gfx_circle(tx + 4 + r, 116 + r, r, C_ON);
    gfx_text_center(tx + 4, 2 * r, 120, 20, C_BG, "R");

    gfx_rrect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 22, C_SURF);
    if (!c->ok) {
        gfx_text(PANEL_X + 28, PANEL_Y + 28, 22, C_ERROR, g->error[0] ? g->error : c->error);
        if (s->page == 3 && g->disc_ok) {
            page_files(s);
        }
    } else {
        switch (s->page) {
        case 0: page_general(s); break;
        case 1: page_sections(s); break;
        case 2: page_libs(s); break;
        default: page_files(s); break;
        }
    }
    Hint h[] = {{"L", "Pestaña"}, {"R", "Pestaña"}, {"B", "Volver"}, {"+", "Salir"}};
    bottom_bar(h, 4);
}

/* ---- pruebas de hardware ---- */

static void probe_row(int *y, int st, const char *what, const char *detail)
{
    gfx_rrect(80, *y, UI_W - 160, 70, 18, C_SURF);
    badge(104, *y + 20, st);
    gfx_text(258, *y + 10, 22, C_ON, what);
    if (detail) {
        gfx_text_fit(258, *y + 40, 18, UI_W - 160 - 200, C_ON_VAR, detail);
    }
    *y += 82;
}

static void draw_probe(const UiState *s)
{
    top_bar(s, "Pruebas de hardware para la etapa 2");
    const ProbeResult *r = s->probe;
    char b[160];
    int y = 110;
    char tot[24], big[24];
    fmt_size(r->mem_total, tot, sizeof(tot));
    fmt_size(r->biggest_alloc, big, sizeof(big));

    snprintf(b, sizeof(b), "%s", r->applet_mode ? "Applet (desde el Álbum): poca RAM. Abre manteniendo R sobre un juego."
                                                : "Aplicación completa (title takeover)");
    probe_row(&y, r->applet_mode ? 2 : 0, "Modo de arranque", b);
    snprintf(b, sizeof(b), "Total %s, bloque más grande %s (hace falta al menos 1 GB)", tot, big);
    probe_row(&y, r->biggest_alloc >= (1024ull << 20) ? 0 : 1, "Memoria RAM", b);
    if (r->jit_ok) {
        snprintf(b, sizeof(b), "Generó código ARM64 y lo ejecutó: devolvió %d", r->jit_value);
    } else {
        snprintf(b, sizeof(b), "No se pudo: error 0x%X", (unsigned)r->jit_rc);
    }
    probe_row(&y, r->jit_ok ? 0 : 1, "JIT (traductor x86 a ARM64)", b);
    probe_row(&y, r->va4g_ok ? 0 : 1, "Ventana de 4 GB para la memoria del Xbox",
              r->va4g_ok ? "Hay espacio de direcciones libre" : "No hay una ventana libre de 4 GB");
    if (!s->exc_tried) {
        probe_row(&y, 2, "Atrapar accesos inválidos",
                  "Pulsa A para probar. Si la app se cierra, ese también es un resultado: avísame.");
    } else if (s->exc_ok) {
        snprintf(b, sizeof(b), "Atrapado y recuperado (tipo 0x%X, dirección 0x%llX)", (unsigned)s->exc_desc,
                 (unsigned long long)s->exc_far);
        probe_row(&y, 0, "Atrapar accesos inválidos", b);
    } else {
        probe_row(&y, 1, "Atrapar accesos inválidos", "El acceso inválido no se atrapó");
    }
    gfx_text_center(0, UI_W, y + 4, 20, C_ON_VAR, "Toma una foto de esta pantalla y mándamela: con esto se decide la etapa 2.");
    Hint h1[] = {{"A", "Probar excepciones"}, {"B", "Volver"}, {"+", "Salir"}};
    Hint h2[] = {{"B", "Volver"}, {"+", "Salir"}};
    if (s->exc_tried) {
        bottom_bar(h2, 2);
    } else {
        bottom_bar(h1, 3);
    }
}

void ui_draw(const UiState *s)
{
    gfx_clear(C_BG);
    switch (s->screen) {
    case UI_LOADING: draw_loading(s); break;
    case UI_LIBRARY: draw_library(s); break;
    case UI_DETAIL: draw_detail(s); break;
    case UI_PROBE: draw_probe(s); break;
    }
}

/* ---- tarjetas ---- */

void ui_card_from_game(UiCard *c, const Game *g, const char *file, unsigned long long size, bool ok)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->file, sizeof(c->file), "%s", file);
    c->size = size;
    c->ok = ok;
    c->kind = g->disc_ok ? xdisc_kind(&g->disc) : NULL;
    if (ok) {
        snprintf(c->title, sizeof(c->title), "%s", g->xbe.title_name);
        c->title_id = g->xbe.title_id;
        c->region = g->xbe.region;
        c->version = g->xbe.version;
    } else {
        snprintf(c->error, sizeof(c->error), "%s", g->error);
    }
    if (g->icon.px) {
        size_t n = (size_t)g->icon.w * (size_t)g->icon.h * 4;
        c->icon.px = malloc(n);
        if (c->icon.px) {
            memcpy(c->icon.px, g->icon.px, n);
            c->icon.w = g->icon.w;
            c->icon.h = g->icon.h;
        }
    }
}

void ui_card_free(UiCard *c)
{
    ximage_free(&c->icon);
}
