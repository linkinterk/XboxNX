/*
 * XboxNX — análisis de un juego: abre la imagen, busca default.xbe, lee sus
 * cabeceras y la tabla de importaciones del kernel. Portable (Switch y PC).
 */
#ifndef XBOXNX_GAME_H
#define XBOXNX_GAME_H

#include "disc.h"
#include "xbe.h"
#include "xpr.h"

#define GAME_MAX_ROOT 128

typedef struct GameRootEntry {
    char name[64];
    uint32_t size;
    bool is_dir;
} GameRootEntry;

typedef struct Game {
    XDisc disc;
    bool disc_ok;
    XDiscFile xbe_file;
    bool xbe_found;
    int xbe_rc;               /* XBE_OK si se pudo leer */
    XbeInfo xbe;
    unsigned kernel_imports;  /* entradas de la tabla de thunks */
    uint16_t max_ordinal;     /* el ordinal más alto que pide */
    unsigned nroot;
    GameRootEntry root[GAME_MAX_ROOT];
    XImage icon;              /* imagen del juego ($$XTIMAGE), px NULL si no hay */
    char error[128];
} Game;

/* Abre y analiza. Devuelve true si llegó a leer el XBE; si no, error explica. */
bool game_open(Game *g, const char *path);
void game_close(Game *g);

/* Versión del XDK con la que se compiló (de XAPILIB), o 0. */
unsigned game_xdk_build(const Game *g);

#endif
