/*
 * XboxNX — etapa 4c: el dibujo del NV2A con la GPU de la Switch (OpenGL 3.3
 * core sobre mesa/nouveau, con EGL).
 *
 * nv2a_sw.c sigue siendo el que lleva el estado de Kelvin, lee los vértices
 * y corre el vertex shader del juego en el CPU; aquí se rasteriza: los
 * triángulos, las texturas, los register combiners (un solo shader que los
 * interpreta con uniforms, sin compilar uno por estado), la mezcla y el Z.
 * Cada superficie de color del juego es una textura de GL (con su FBO); se
 * copia a la RAM del Xbox al hacer flip (para la pantalla) y cuando el CPU
 * la necesita. Si una superficie se usa como textura, se usa directo.
 *
 * Sin GL (o si falla), nv2a_sw.c dibuja por software como antes.
 */
#ifndef XBOXNX_GPU_H
#define XBOXNX_GPU_H

#include <stdbool.h>
#include <stdint.h>

/* Floats por vértice ya transformado (el OutVert de nv2a_sw.c):
 * pos[4], col0[4], col1[4], tex[4][4], fog. */
#define GPU_VERT_FLOATS 29

typedef struct GpuTex {
    bool on;
    unsigned mode;          /* modo del texture shader (1 2D, 3 cubo, 4 pass-through...) */
    uint32_t addr;          /* dirección física (para reconocer superficies) */
    uint32_t fmt, w, h;
    bool cube, linear, bilinear;
    unsigned au, av;        /* modos de dirección (1 wrap, 2 mirror, otros clamp) */
    const uint8_t *data;    /* para el hash del contenido */
    uint32_t bytes;
    const uint8_t *pal;     /* paleta (P8) para el hash */
    uint32_t pal_bytes;
    /* decodificar la cara face (0..5) a w*h ARGB (0xAARRGGBB); ctx: una copia
     * de lo que necesita (el dibujo puede correr en el hilo de la GPU, cuando
     * el estado de nv2a_sw ya es otro) */
    void (*decode)(const void *ctx, int face, uint32_t *out);
    uint64_t ctx[14];
} GpuTex;

typedef struct GpuDraw {
    unsigned prim;          /* 5 triángulos, 6 tira, 7 abanico, 8 quads, 9 tira de quads, 10 polígono */
    unsigned nverts;
    const float *verts;     /* nverts * GPU_VERT_FLOATS */
    /* destino */
    uint32_t color_pa, pitch;
    unsigned fmt;           /* 4..8: 32 bits (8: con alfa) */
    int w, h;               /* tamaño de la superficie (pitch/4 x y1) */
    uint32_t zeta_pa, zpitch;
    unsigned zfmt;          /* 1 Z16, 2 Z24S8; 0 sin Z */
    float zmax;
    int x0, y0, x1, y1;     /* recorte */
    /* estado */
    bool blend, dtest, dmask, cull, atest;
    uint32_t sfac, dfac, beq, bcolor, dfunc, cull_face, front_face, cmask, afunc, aref;
    uint32_t cctl, c_icw[8], a_icw[8], c_ocw[8], a_ocw[8], factor0[8], factor1[8], fcw0, fcw1, fog_color;
    GpuTex tex[4];
    /* 4c: el vertex shader en el GPU. Si vs_in no es NULL, verts no se usa:
     * vs_in son las entradas de cada vértice (nverts x 16 x 4 floats), vs_mask
     * las que usa el programa, vs_consts las 192 constantes, vs_key el hash
     * del programa y vs_src su GLSL (vsh_glsl). */
    const float *vs_in;
    uint16_t vs_mask;
    const float *vs_consts;
    uint64_t vs_key;
    const char *vs_src;
    /* etapa 5: o en vez de vs_in, los vértices ya empaquetados (solo los
     * atributos de vs_mask, vs_pk_n vértices) y, si vs_idx, los índices
     * (vs_nidx, ya en triángulos para los quads) */
    const float *vs_pk;
    unsigned vs_pk_n;
    const uint32_t *vs_idx;
    unsigned vs_nidx;
    bool query; /* contar los píxeles que pasan Z (prueba de visibilidad) */
} GpuDraw;

/* nwin: el NWindow de la Switch (NULL en la PC: sin ventana). */
bool gpu_init(void *nwin);
/* Opcional: recibe cada paso de gpu_init (para un registro en la SD). */
extern void (*gpu_log)(const char *paso);
void gpu_set_info(const char *t);
bool gpu_ready(void);
/* Activar/desactivar el dibujo por GPU (con GL listo). */
void gpu_enable(bool on);
bool gpu_enabled(void);
const char *gpu_info(void);

/* Dibujar; false: no se pudo (lo hace el software). */
bool gpu_draw(const GpuDraw *d);
/* Borrar el rectángulo [x0,x1)x[y0,y1) de la superficie de color (mask: bits
 * ARGB 0xF) o de Z/stencil (flags 1 Z, 2 stencil). false: no tiene superficie
 * en la GPU (que lo haga la RAM). */
bool gpu_clear_color(uint32_t pa, uint32_t pitch, int h, int x0, int y0, int x1, int y1, uint32_t argb, unsigned mask);
bool gpu_clear_zeta(uint32_t pa, uint32_t zpitch, int h, int x0, int y0, int x1, int y1, uint32_t value, unsigned zfmt,
                    unsigned flags);
/* Copiar a la RAM la superficie que empieza en pa (la que se va a mostrar). */
void gpu_flush_surface(uint32_t pa);
/* Etapa 5: igual pero sin esperar a la GPU (la RAM se pone al día después:
 * gpu_readback_poll o cuando algo necesite esa RAM). */
void gpu_flush_surface_async(uint32_t pa);
void gpu_readback_poll(void);
/* Las pruebas de visibilidad: la suma de los píxeles que pasaron desde el
 * último reset (espera a la GPU), y empezar de nuevo. */
uint32_t gpu_query_collect(void);
void gpu_query_reset(void);
/* Copiar a la RAM todas las superficies (antes de que el CPU lea). */
void gpu_flush_all(void);
/* La RAM de la superficie cambió por otro lado: volver a subirla. */
void gpu_invalidate(uint32_t pa, uint32_t len);
/* Fin de cuadro (limpia lo que no se usó en un rato). */
void gpu_end_frame(void);

/* La pantalla de la app con GL (1280x720 RGBA): true si se mostró. */
bool gpu_present_ui(const uint32_t *rgba, int w, int h);

typedef struct GpuStats {
    unsigned draws, fallbacks, uploads, flushes, surfaces, textures;
    unsigned vs_draws, vs_progs, vs_bad, vs_cached, vs_saved, vs_const_vec;
    unsigned vs_known, vs_pre; /* vertex shaders guardados en la SD de otras veces y cuántos ya se compilaron por adelantado */
    unsigned vs_deferred; /* dibujos que esperaron un cuadro a que su programa se compile (los hizo la CPU) */
    bool sso;             /* programas por etapa: el fragment shader se compila una sola vez */
    uint64_t ns_link;     /* tiempo compilando programas */
    uint64_t ns_rp;       /* tiempo dentro de glReadPixels al terminar cada cuadro */
    unsigned clears_c, clears_z, clear_bind, clear_new;
    /* etapa 5: la GPU en su hilo */
    bool mt;
    unsigned mt_draws, mt_lost, syncs;
    uint64_t ns_busy;  /* el hilo de la GPU ejecutando órdenes */
    uint64_t ns_enq;   /* el juego copiando dibujos a la cola */
    uint64_t ns_qwait; /* el juego esperando lugar en la cola (la GPU no da abasto) */
    uint64_t ns_sync;  /* el juego esperando una respuesta del hilo */
    uint64_t q_max;    /* lo más lleno que estuvo la cola (bytes) */ /* borrados de color / Z, cuántos cambiaron de FBO, cuántos crearon superficie */
    int rp_mode;          /* 0 probando, 1 BGRA, 2 RGBA (cambiando R y B en la CPU) */
    unsigned rp_us[2];    /* µs por lectura con cada forma, en la prueba */
    unsigned indexed; /* etapa 5: dibujos con índices (cada vértice una vez) */
    unsigned queries; /* informes de visibilidad leídos */
    bool bin_ok;      /* el driver da binarios de programas (la caché de shaders en la SD funciona) */ /* 4c: dibujos con el vertex shader en el GPU, programas, los que no compilaron */
    uint64_t upload_bytes;
    uint64_t ns_draw, ns_tex, ns_read, ns_vbuf, ns_query, ns_call; /* etapa 5: tiempo en gpu_draw (todo), en decodificar+subir texturas y en leer cuadros a la RAM */
} GpuStats;
const GpuStats *gpu_stats(void);
/* Cuánto tiempo de programas nuevos se compila por cuadro antes de dejar los
 * demás para el siguiente (replay lo quita para comparar imágenes). */
void gpu_set_link_budget(uint64_t ns);
/* El vertex program con esa clave: 0 no lo tiene, 1 listo, 2 no compila. */
int gpu_vs_state(uint64_t key);
/* Deja listo el programa (lo compila con src si falta y hay tiempo en este
 * cuadro). false: este dibujo lo transforma la CPU. */
bool gpu_vs_prepare(uint64_t key, const char *src);
/* Al salir de la app. */
void gpu_exit(void);
/* Etapa 5: pasar el dibujo a un hilo propio (con gpu_init hecho). false: sigue en este. */
bool gpu_thread_start(void);
/* Para el muestreo: qué hace el hilo de la GPU (-2 no hay hilo, -1 espera órdenes, si no la zona de prof.h). */
int gpu_thread_zone(void);

/* Olvidar todas las superficies y texturas (otra ejecución, o la RAM cambió por otro lado). */
void gpu_reset(void);
/* La RAM del Xbox (para subir y bajar superficies). */
void gpu_set_ram(uint8_t *ram, uint32_t size);

#endif
