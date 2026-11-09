/*
 * XboxNX — pantallas (estilo de la app de hakuX: tema oscuro y verde).
 * Portable: la Switch dibuja con esto y la PC genera vistas previas.
 */
#ifndef XBOXNX_UI_H
#define XBOXNX_UI_H

#include "game.h"
#include "loader.h"
#include "probe.h"
#include "runner.h"

#define UI_W 1280
#define UI_H 720
#define UI_COLS 2
#define UI_ROWS_VISIBLE 3

typedef struct UiCard {
    char file[256];
    char title[64];
    uint32_t title_id;
    uint32_t region;
    uint32_t version;
    unsigned long long size;
    const char *kind;   /* "XISO", "Disco completo XGD2"... */
    XImage icon;
    bool ok;            /* se pudo leer el XBE */
    char error[128];
} UiCard;

typedef enum { UI_LOADING, UI_LIBRARY, UI_DETAIL, UI_PROBE, UI_MEMORY, UI_RUN, UI_STAGES } UiScreen;

typedef struct UiState {
    UiScreen screen;
    const char *version;
    /* biblioteca */
    const UiCard *cards;
    int ncards;
    int sel;
    int first_row;      /* primera fila visible */
    /* cargando */
    int load_done, load_total;
    const char *load_name;
    /* detalle */
    const Game *game;
    int page;           /* 0 General, 1 Secciones, 2 Librerías, 3 Archivos */
    int scroll;
    /* etapa 2a: el juego en la memoria del Xbox */
    const LoadReport *load;
    int load_scroll;
    /* etapa 2c: ejecución en el intérprete */
    const Runner *run;
    int run_scroll;          /* -1: seguir la última llamada */
    int stage_scroll;        /* la tabla de etapas */
    int stage_back;          /* la pantalla a la que vuelve */
    bool run_screen;         /* X: ver la pantalla del Xbox en vez del registro */
    /* pruebas */
    const ProbeResult *probe;
    bool exc_tried, exc_ok;
    uint32_t exc_desc;
    uint64_t exc_far;
} UiState;

#define UI_PAGES 4

void ui_draw(const UiState *s);
/* Filas visibles por página de la pantalla de detalle (para el scroll). */
int ui_detail_rows(void);
/* Filas visibles de la lista de importaciones de la etapa 2a. */
int ui_memory_rows(void);
/* Filas visibles del registro de llamadas de la etapa 2c. */
int ui_run_rows(void);
/* La tabla de etapas: cuántas hay y cuántas filas se ven. */
int ui_stage_count(void);
int ui_stage_rows(void);
/* Índice de la tarjeta bajo (x, y) en la biblioteca, o -1 (pantalla táctil). */
int ui_card_at(const UiState *s, int x, int y);

/* Llena una tarjeta a partir de un juego ya abierto (copia la imagen). */
void ui_card_from_game(UiCard *c, const Game *g, const char *file, unsigned long long size, bool ok);
void ui_card_free(UiCard *c);

#endif
