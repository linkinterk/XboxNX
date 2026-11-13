/* XboxNX — etapa 4c: el dibujo del NV2A con OpenGL (ver gpu.h). */
#include "gpu.h"
#include <stdio.h>

#ifndef XBOXNX_NO_GL /* las pruebas en la PC compilan sin GL: ver el final */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "gl_min.h"
#include "prof.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

/* ------------------------------------------------------------ cargador */

#define GL_DEF(name, ret, args) ret(*name) args;
GL_FUNCS(GL_DEF)
#undef GL_DEF

int gl_load(void *(*getproc)(const char *), const char **missing)
{
#define GL_GET(name, ret, args)                     \
    *(void **)&name = getproc(#name);               \
    if (!name) {                                    \
        *missing = #name;                           \
        return 0;                                   \
    }
    GL_FUNCS(GL_GET)
#undef GL_GET
    return 1;
}

static void *egl_proc(const char *n) { return (void *)eglGetProcAddress(n); }

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Opcionales: los binarios de los programas (la caché de shaders en la SD). */
static void (*p_glGetProgramBinary)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
static void (*p_glProgramBinary)(GLuint, GLenum, const void *, GLsizei);
static void (*p_glProgramParameteri)(GLuint, GLenum, GLint);
static void (*p_glGetIntegerv)(GLenum, GLint *);
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 1099511628211ull;
    }
    return h;
}


#define GL_RGB8 0x8051

/* ------------------------------------------------------------ estado */

#define MAX_SURF 24
#define MAX_ZETA 12
#define MAX_FBO 32
#define MAX_TEX 384
#define RING_SIZE (16u << 20) /* el anillo de los vértices (vbo2) */
#define IRING_SIZE (4u << 20) /* el de los índices */

typedef struct Surf {
    uint32_t pa, pitch;
    int w, h;
    unsigned fmt;
    GLuint tex, fbo;   /* fbo: solo color (para leerla) */
    bool dirty;        /* la GPU tiene datos que la RAM no */
    unsigned used;     /* cuadro en que se usó */
    uint8_t params;    /* filtro y repetición puestos al usarla de textura (1 + clave) */
} Surf;

typedef struct Zeta {
    uint32_t pa, zpitch;
    int w, h;
    GLuint rb;
    unsigned used;
    bool pend;        /* borrarlo al ponerle su primer FBO */
    double pend_z;
    int pend_s;
} Zeta;

typedef struct Fbo {
    int surf, zeta;    /* índices; zeta -1 sin Z */
    GLuint fbo;
} Fbo;

typedef struct TexEnt {
    uint32_t addr, fmt, w, h;
    bool cube;
    uint64_t hash;
    GLuint tex;
    unsigned checked, used;
    bool valid;
    uint8_t params; /* filtro y repetición puestos (1 + clave; 0: ninguno) */
} TexEnt;

/* Un programa de GL: el de siempre (vértices ya transformados por la CPU) o
 * uno por cada vertex program del juego (4c, el vertex shader en el GPU).
 * Cada uno guarda sus uniforms (los valores son de cada programa). */
#define U_SLOTS 20
#define MAX_PROG 128
typedef struct Prog {
    GLuint prog;
    uint64_t key;   /* el hash del vertex program (0: el de siempre) */
    bool bad;       /* no compiló: lo hace la CPU */
    unsigned used;
    GLint u_vp, u_tmode, u_tcube, u_tscale, u_cctl, u_fcw0, u_fcw1, u_icw, u_ocw, u_aicw, u_aocw, u_f0, u_f1, u_fog,
        u_atest, u_afunc, u_aref, u_s2d[4], u_scb[4], u_c;
    uint8_t ulen[U_SLOTS];
    uint8_t u[U_SLOTS][128];
    float c[192 * 4];
    bool c_ok;
} Prog;

static struct {
    bool ready, on;
    EGLDisplay dpy;
    EGLSurface win;
    EGLContext ctx;
    char info[160];
    uint8_t *ram;
    uint32_t ram_size;
    GLuint vao, vbo, ibo;
    GLuint ui_prog, ui_tex, ui_vao;
    int ui_w, ui_h;
    Prog progs[MAX_PROG]; /* [0]: el de siempre */
    bool bin_ok;          /* el driver da binarios de programas (caché en la SD) */
    uint64_t drv_hash;    /* el hash del driver y del fragment shader (parte de la clave) */
    /* el vertex shader en el GPU: sus entradas empaquetadas */
    GLuint vao2, vbo2;
    uint16_t vao2_mask;
    size_t ring_pos;      /* etapa 5: vbo2 es un anillo; se escribe sin sincronizar */
    GLuint iring;         /* y otro para los índices */
    size_t iring_pos;
    GLint draw_first;     /* el primer vértice de este dibujo en el anillo */
    GLuint qibo[2];       /* índices fijos de quads y de tiras de quads */
    uint32_t qibo_n[2];   /* para cuántos vértices alcanzan */
    float *vsbuf;
    size_t vsbuf_n;
    Surf surf[MAX_SURF];
    Zeta zeta[MAX_ZETA];
    Fbo fbo[MAX_FBO];
    TexEnt tex[MAX_TEX];
    unsigned frame;
    uint32_t *scratch;  /* decodificar texturas */
    size_t scratch_n;
    uint32_t *idx;
    size_t idx_n;
    GpuStats st;
} G;

const GpuStats *gpu_stats(void) { return &G.st; }
bool gpu_ready(void) { return G.ready; }
void gpu_enable(bool on) { G.on = on && G.ready; }
bool gpu_enabled(void) { return G.on; }
const char *gpu_info(void) { return G.info; }
void gpu_set_info(const char *t) { snprintf(G.info, sizeof(G.info), "%s", t); }

void gpu_set_ram(uint8_t *ram, uint32_t size)
{
    G.ram = ram;
    G.ram_size = size;
}

/* ------------------------------------------------------------ shaders */

static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_pos;\n"
    "layout(location=1) in vec4 a_c0;\n"
    "layout(location=2) in vec4 a_c1;\n"
    "layout(location=3) in vec4 a_t0;\n"
    "layout(location=4) in vec4 a_t1;\n"
    "layout(location=5) in vec4 a_t2;\n"
    "layout(location=6) in vec4 a_t3;\n"
    "uniform vec4 u_vp;\n" /* 2/W, 2/H, 2/zmax */
    "out vec4 v_c0; out vec4 v_c1; out vec4 v_t0; out vec4 v_t1; out vec4 v_t2; out vec4 v_t3;\n"
    "void main() {\n"
    "  float w = a_pos.w;\n"
    "  if (!(abs(w) > 1e-20) || isinf(w)) w = 1.0;\n"
    "  vec3 n = vec3(a_pos.x * u_vp.x - 1.0, a_pos.y * u_vp.y - 1.0, a_pos.z * u_vp.z - 1.0);\n"
    "  gl_Position = vec4(n * w, w);\n"
    "  v_c0 = a_c0; v_c1 = a_c1; v_t0 = a_t0; v_t1 = a_t1; v_t2 = a_t2; v_t3 = a_t3;\n"
    "}\n";

/* Los register combiners interpretados (lo mismo que combine() de nv2a_sw.c). */
static const char *k_fs =
    "#version 330 core\n"
    "in vec4 v_c0; in vec4 v_c1; in vec4 v_t0; in vec4 v_t1; in vec4 v_t2; in vec4 v_t3;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D s2d0; uniform sampler2D s2d1; uniform sampler2D s2d2; uniform sampler2D s2d3;\n"
    "uniform samplerCube scb0; uniform samplerCube scb1; uniform samplerCube scb2; uniform samplerCube scb3;\n"
    "uniform int u_tmode[4];\n"
    "uniform int u_tcube[4];\n"
    "uniform vec2 u_tscale[4];\n"
    "uniform uint u_cctl; uniform uint u_fcw0; uniform uint u_fcw1;\n"
    "uniform uint u_icw[8]; uniform uint u_ocw[8]; uniform uint u_aicw[8]; uniform uint u_aocw[8];\n"
    "uniform vec4 u_f0[8]; uniform vec4 u_f1[8];\n"
    "uniform vec4 u_fog;\n"
    "uniform int u_atest; uniform uint u_afunc; uniform float u_aref;\n"
    "vec4 R[16];\n"
    "float map_in(float x, uint m) {\n"
    "  if (m == 0u) return max(x, 0.0);\n"
    "  if (m == 1u) return 1.0 - clamp(x, 0.0, 1.0);\n"
    "  if (m == 2u) return 2.0 * max(x, 0.0) - 1.0;\n"
    "  if (m == 3u) return -2.0 * max(x, 0.0) + 1.0;\n"
    "  if (m == 4u) return max(x, 0.0) - 0.5;\n"
    "  if (m == 5u) return -max(x, 0.0) + 0.5;\n"
    "  if (m == 6u) return x;\n"
    "  return -x;\n"
    "}\n"
    "vec3 in3(uint sel) {\n"
    "  vec4 r = R[int(sel & 15u)]; uint m = (sel >> 5) & 7u;\n"
    "  vec3 s = ((sel >> 4) & 1u) != 0u ? vec3(r.a) : r.rgb;\n"
    "  return vec3(map_in(s.r, m), map_in(s.g, m), map_in(s.b, m));\n"
    "}\n"
    "float in1(uint sel) {\n"
    "  vec4 r = R[int(sel & 15u)];\n"
    "  return map_in(((sel >> 4) & 1u) != 0u ? r.a : r.b, (sel >> 5) & 7u);\n"
    "}\n"
    "float op(float x, uint o) {\n"
    "  if (o == 1u) x -= 0.5; else if (o == 2u) x *= 2.0; else if (o == 3u) x = (x - 0.5) * 2.0;\n"
    "  else if (o == 4u) x *= 4.0; else if (o == 6u) x *= 0.5;\n"
    "  return clamp(x, -1.0, 1.0);\n"
    "}\n"
    "vec3 op3(vec3 x, uint o) { return vec3(op(x.r, o), op(x.g, o), op(x.b, o)); }\n"
    "bool cmpf(uint f, float a, float b) {\n"
    "  if (f == 0x200u) return false; if (f == 0x201u) return a < b; if (f == 0x202u) return a == b;\n"
    "  if (f == 0x203u) return a <= b; if (f == 0x204u) return a > b; if (f == 0x205u) return a != b;\n"
    "  if (f == 0x206u) return a >= b; return true;\n"
    "}\n"
    "vec4 stage(int md, int cube, vec2 sc, vec4 tc, sampler2D s2, samplerCube scb) {\n"
    "  if (md == 4) return clamp(tc, 0.0, 1.0);\n"
    "  if (md == 3 && cube != 0) return texture(scb, tc.xyz);\n"
    "  vec2 uv = tc.xy; if (md == 1 && tc.w != 0.0 && tc.w != 1.0) uv /= tc.w;\n"
    "  return texture(s2, uv * sc);\n"
    "}\n"
    "void main() {\n"
    "  for (int i = 0; i < 16; i++) R[i] = vec4(0.0);\n"
    "  R[4] = clamp(v_c0, 0.0, 1.0); R[5] = clamp(v_c1, 0.0, 1.0);\n"
    "  R[3] = vec4(u_fog.rgb, 1.0);\n"
    "  if (u_tmode[0] != 0) R[8] = stage(u_tmode[0], u_tcube[0], u_tscale[0], v_t0, s2d0, scb0);\n"
    "  if (u_tmode[1] != 0) R[9] = stage(u_tmode[1], u_tcube[1], u_tscale[1], v_t1, s2d1, scb1);\n"
    "  if (u_tmode[2] != 0) R[10] = stage(u_tmode[2], u_tcube[2], u_tscale[2], v_t2, s2d2, scb2);\n"
    "  if (u_tmode[3] != 0) R[11] = stage(u_tmode[3], u_tcube[3], u_tscale[3], v_t3, s2d3, scb3);\n"
    "  R[12].a = R[8].a;\n"
    "  vec4 c;\n"
    "  uint n = u_cctl & 0xFFu;\n"
    "  if (n != 0u) {\n"
    "    if (n > 8u) n = 8u;\n"
    "    bool per = ((u_cctl >> 12) & 1u) != 0u; bool msb = ((u_cctl >> 8) & 1u) != 0u;\n"
    "    for (uint s = 0u; s < n; s++) {\n"
    "      uint k = per ? s : 0u;\n"
    "      R[1] = u_f0[k]; R[2] = u_f1[k];\n"
    "      uint icw = u_icw[s], ocw = u_ocw[s];\n"
    "      vec3 a = in3(icw >> 24), b = in3((icw >> 16) & 0xFFu), cc = in3((icw >> 8) & 0xFFu), d = in3(icw & 0xFFu);\n"
    "      bool abdot = ((ocw >> 13) & 1u) != 0u, cddot = ((ocw >> 12) & 1u) != 0u, mux = ((ocw >> 14) & 1u) != 0u;\n"
    "      uint o = (ocw >> 15) & 7u;\n"
    "      float r0a = R[12].a;\n"
    "      bool sel_cd = msb ? r0a >= 0.5 : ((int(r0a * 255.0) & 1) != 0);\n"
    "      vec3 pab = abdot ? vec3(dot(a, b)) : a * b; vec3 pcd = cddot ? vec3(dot(cc, d)) : cc * d;\n"
    "      vec3 ab = op3(pab, o), cd = op3(pcd, o);\n"
    "      vec3 sum = op3(mux ? (sel_cd ? pcd : pab) : pab + pcd, o);\n"
    "      uint aicw = u_aicw[s], aocw = u_aocw[s];\n"
    "      float aa = in1(aicw >> 24), ba = in1((aicw >> 16) & 0xFFu), ca = in1((aicw >> 8) & 0xFFu), da = in1(aicw & 0xFFu);\n"
    "      uint ao = (aocw >> 15) & 7u;\n"
    "      float aab = op(aa * ba, ao), acd = op(ca * da, ao);\n"
    "      float asum = op(((aocw >> 14) & 1u) != 0u ? (sel_cd ? ca * da : aa * ba) : aa * ba + ca * da, ao);\n"
    "      uint dcd_r = ocw & 15u, dab_r = (ocw >> 4) & 15u, dsum_r = (ocw >> 8) & 15u;\n"
    "      if (dcd_r != 0u) { R[dcd_r].rgb = cd; if (((ocw >> 19) & 1u) != 0u) R[dcd_r].a = cd.b; }\n"
    "      if (dab_r != 0u) { R[dab_r].rgb = ab; if (((ocw >> 18) & 1u) != 0u) R[dab_r].a = ab.b; }\n"
    "      if (dsum_r != 0u) R[dsum_r].rgb = sum;\n"
    "      uint acd_r = aocw & 15u, aab_r = (aocw >> 4) & 15u, asum_r = (aocw >> 8) & 15u;\n"
    "      if (acd_r != 0u) R[acd_r].a = acd;\n"
    "      if (aab_r != 0u) R[aab_r].a = aab;\n"
    "      if (asum_r != 0u) R[asum_r].a = asum;\n"
    "    }\n"
    "    R[14].rgb = clamp(R[5].rgb, 0.0, 1.0) + clamp(R[12].rgb, 0.0, 1.0);\n"
    "    vec3 e = in3(u_fcw1 >> 24), f = in3((u_fcw1 >> 16) & 0xFFu);\n"
    "    R[15].rgb = e * f;\n"
    "    vec3 A = clamp(in3((u_fcw0 >> 24) & 0x3Fu), 0.0, 1.0);\n"
    "    vec3 B = clamp(in3((u_fcw0 >> 16) & 0x3Fu), 0.0, 1.0);\n"
    "    vec3 C = clamp(in3((u_fcw0 >> 8) & 0x3Fu), 0.0, 1.0);\n"
    "    vec3 D = clamp(in3(u_fcw0 & 0x3Fu), 0.0, 1.0);\n"
    "    float G = in1((u_fcw1 >> 8) & 0x3Fu);\n"
    "    c = vec4(clamp(A * B + (vec3(1.0) - A) * C + D, 0.0, 1.0), clamp(G, 0.0, 1.0));\n"
    "  } else {\n"
    "    c = u_tmode[0] != 0 ? R[8] * R[4] : R[4];\n"
    "  }\n"
    "  if (u_atest != 0 && !cmpf(u_afunc, floor(c.a * 255.0 + 0.5), u_aref)) discard;\n"
    "  o_color = c;\n"
    "}\n";

static const char *k_ui_vs =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);\n"
    "  v_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";
static const char *k_ui_fs =
    "#version 330 core\n"
    "in vec2 v_uv; out vec4 o; uniform sampler2D t;\n"
    "void main() { o = vec4(texture(t, v_uv).rgb, 1.0); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[600];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        snprintf(G.info, sizeof(G.info), "shader: %.150s", log);
        fprintf(stderr, "XboxNX GL shader:\n%s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link2(const char *vs, const char *fs, bool keep_bin)
{
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    if (keep_bin && G.bin_ok) {
        p_glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(v);
    glDeleteShader(f);
    if (!ok) {
        char log[600];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        snprintf(G.info, sizeof(G.info), "link: %.150s", log);
        fprintf(stderr, "XboxNX GL link:\n%s\n", log);
        return 0;
    }
    return p;
}

static GLuint link(const char *vs, const char *fs) { return link2(vs, fs, false); }
static GLuint link_vs(const char *vs) { return link2(vs, k_fs, true); }

/* Las posiciones de los uniforms de un programa y sus samplers fijos. */
static void prog_locs(Prog *P)
{
#define U(f) P->f = glGetUniformLocation(P->prog, #f)
    U(u_vp); U(u_tmode); U(u_tcube); U(u_tscale); U(u_cctl); U(u_fcw0); U(u_fcw1); U(u_icw); U(u_ocw);
    U(u_aicw); U(u_aocw); U(u_f0); U(u_f1); U(u_fog); U(u_atest); U(u_afunc); U(u_aref);
#undef U
    P->u_c = glGetUniformLocation(P->prog, "c");
    glUseProgram(P->prog);
    static const char *s2[4] = {"s2d0", "s2d1", "s2d2", "s2d3"}, *sc[4] = {"scb0", "scb1", "scb2", "scb3"};
    for (int i = 0; i < 4; i++) {
        P->u_s2d[i] = glGetUniformLocation(P->prog, s2[i]);
        P->u_scb[i] = glGetUniformLocation(P->prog, sc[i]);
        glUniform1i(P->u_s2d[i], i);
        glUniform1i(P->u_scb[i], 4 + i);
    }
    memset(P->ulen, 0, sizeof(P->ulen));
    P->c_ok = false;
}

/* ------------------------------------------------------------ EGL */

void (*gpu_log)(const char *paso);
#define PASO(t) (gpu_log ? gpu_log(t) : (void)0)

static bool init_inner(void *nwin);

/* Si algo falla a medias, se suelta todo: la ventana queda libre para el
 * framebuffer de siempre. */
static void teardown(void)
{
    if (G.dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (G.ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(G.dpy, G.ctx);
        }
        if (G.win != EGL_NO_SURFACE) {
            eglDestroySurface(G.dpy, G.win);
        }
        eglTerminate(G.dpy);
    }
    G.dpy = EGL_NO_DISPLAY;
    G.ctx = EGL_NO_CONTEXT;
    G.win = EGL_NO_SURFACE;
    G.ready = G.on = false;
}

bool gpu_init(void *nwin)
{
    if (G.ready) {
        return true;
    }
    G.dpy = EGL_NO_DISPLAY;
    G.ctx = EGL_NO_CONTEXT;
    G.win = EGL_NO_SURFACE;
    if (init_inner(nwin)) {
        return true;
    }
    PASO("falló, se suelta EGL");
    teardown();
    return false;
}

static bool init_inner(void *nwin)
{
    PASO("eglGetDisplay");
#ifdef __SWITCH__
    G.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    G.dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
#endif
    PASO("eglInitialize");
    if (G.dpy == EGL_NO_DISPLAY || !eglInitialize(G.dpy, NULL, NULL)) {
        snprintf(G.info, sizeof(G.info), "EGL: sin pantalla");
        return false;
    }
    PASO("eglBindAPI");
    if (!eglBindAPI(EGL_OPENGL_API)) {
        snprintf(G.info, sizeof(G.info), "EGL: sin OpenGL");
        return false;
    }
    /* Sin ventana (la pantalla es del framebuffer de la app): se dibuja solo
     * en FBOs y se copia a la RAM del Xbox. Contexto sin superficie
     * (EGL_KHR_surfaceless_context) o, si no, un pbuffer pequeño. */
    EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                         EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, nwin ? EGL_WINDOW_BIT : 0, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    PASO("eglChooseConfig");
    if (!eglChooseConfig(G.dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        snprintf(G.info, sizeof(G.info), "EGL: sin configuración");
        return false;
    }
    G.win = EGL_NO_SURFACE;
    if (nwin) {
        PASO("eglCreateWindowSurface");
        G.win = eglCreateWindowSurface(G.dpy, cfg, (EGLNativeWindowType)nwin, NULL);
        if (G.win == EGL_NO_SURFACE) {
            snprintf(G.info, sizeof(G.info), "EGL: sin superficie (0x%X)", (unsigned)eglGetError());
            return false;
        }
    }
    EGLint ctx_attr[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                         EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE};
    PASO("eglCreateContext 3.3");
    G.ctx = eglCreateContext(G.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (G.ctx == EGL_NO_CONTEXT) {
        snprintf(G.info, sizeof(G.info), "EGL: sin contexto 3.3 (0x%X)", (unsigned)eglGetError());
        return false;
    }
    PASO("eglMakeCurrent");
    if (!eglMakeCurrent(G.dpy, G.win, G.win, G.ctx)) {
        if (nwin) {
            snprintf(G.info, sizeof(G.info), "EGL: no se pudo activar (0x%X)", (unsigned)eglGetError());
            return false;
        }
        PASO("pbuffer");
        EGLint pcfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                              EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
        EGLConfig pcfg;
        EGLint pn = 0, pb_attr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        if (eglChooseConfig(G.dpy, pcfg_attr, &pcfg, 1, &pn) && pn > 0) {
            G.win = eglCreatePbufferSurface(G.dpy, pcfg, pb_attr);
        }
        if (G.win == EGL_NO_SURFACE || !eglMakeCurrent(G.dpy, G.win, G.win, G.ctx)) {
            snprintf(G.info, sizeof(G.info), "EGL: no se pudo activar sin ventana (0x%X)", (unsigned)eglGetError());
            return false;
        }
    }
    const char *miss = "?";
    PASO("cargar funciones GL");
    if (!gl_load(egl_proc, &miss)) {
        snprintf(G.info, sizeof(G.info), "GL: falta %s", miss);
        return false;
    }
    if (nwin) {
        eglSwapInterval(G.dpy, 0);
    }
    *(void **)&p_glGetProgramBinary = egl_proc("glGetProgramBinary");
    *(void **)&p_glProgramBinary = egl_proc("glProgramBinary");
    *(void **)&p_glProgramParameteri = egl_proc("glProgramParameteri");
    *(void **)&p_glGetIntegerv = egl_proc("glGetIntegerv");
    {
        GLint nf = 0;
        if (p_glGetIntegerv) {
            p_glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nf);
        }
        G.bin_ok = nf > 0 && p_glGetProgramBinary && p_glProgramBinary && p_glProgramParameteri;
        const char *r = (const char *)glGetString(GL_RENDERER), *v = (const char *)glGetString(GL_VERSION);
        uint64_t h = 1469598103934665603ull;
        h = fnv(h, r ? r : "", r ? strlen(r) : 0);
        h = fnv(h, v ? v : "", v ? strlen(v) : 0);
        h = fnv(h, k_fs, strlen(k_fs));
        h = fnv(h, "vsh1", 4); /* subir si cambia vsh_glsl */
        G.drv_hash = h;
    }
    PASO("shaders");
    G.progs[0].prog = link(k_vs, k_fs);
    G.ui_prog = link(k_ui_vs, k_ui_fs);
    if (!G.progs[0].prog || !G.ui_prog) {
        return false;
    }
    prog_locs(&G.progs[0]);
    glUseProgram(G.ui_prog);
    glUniform1i(glGetUniformLocation(G.ui_prog, "t"), 0);
    PASO("buffers");
    glGenVertexArrays(1, &G.vao);
    glGenVertexArrays(1, &G.ui_vao);
    glGenBuffers(1, &G.vbo);
    glGenBuffers(1, &G.ibo);
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.ibo);
    const GLsizei stride = GPU_VERT_FLOATS * 4;
    static const int off[7] = {0, 4, 8, 12, 16, 20, 24};
    for (int i = 0; i < 7; i++) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribPointer((GLuint)i, 4, GL_FLOAT, GL_FALSE, stride, (const void *)(uintptr_t)(off[i] * 4));
    }
    glBindVertexArray(0);
    glGenVertexArrays(1, &G.vao2);
    glGenBuffers(1, &G.vbo2);
    G.vao2_mask = 0;
    snprintf(G.info, sizeof(G.info), "%s · %s", (const char *)glGetString(GL_RENDERER),
             (const char *)glGetString(GL_VERSION));
    G.ready = true;
    G.on = true;
    return true;
}

/* ------------------------------------------------------------ superficies */

/* ---- caché de estado de gpu_draw: en la Switch cada llamada a mesa cuesta,
 * así que solo se manda lo que cambió desde el dibujo anterior. Cualquier
 * otra función que toque el estado pone SC.valid = false. ---- */
static struct {
    bool valid;
    GLuint fbo, prog, vao;
    int vp[4], sc[4];
    int caps; /* bits: 1 scissor, 2 depth clamp, 4 depth test, 8 cull, 16 blend */
    int known_caps;
    GLenum dfunc, cullf, frontf, bsrc, bdst, beq;
    int dmask;
    unsigned cmask;
    float bcol[4];
} SC;

static void sc_check(void)
{
    if (!SC.valid) {
        memset(&SC, 0, sizeof(SC));
        SC.fbo = SC.prog = SC.vao = 0xFFFFFFFFu;
        SC.dfunc = SC.cullf = SC.frontf = SC.bsrc = SC.bdst = SC.beq = 0xFFFFFFFFu;
        SC.dmask = -1;
        SC.cmask = 0xFFFFFFFFu;
        SC.vp[2] = SC.sc[2] = -1;
        SC.bcol[0] = -1;
        SC.valid = true;
    }
}
static void sc_cap(GLenum cap, int bit, bool on)
{
    if ((SC.known_caps & bit) && ((SC.caps & bit) != 0) == on) {
        return;
    }
    if (on) {
        glEnable(cap);
        SC.caps |= bit;
    } else {
        glDisable(cap);
        SC.caps &= ~bit;
    }
    SC.known_caps |= bit;
}
/* true: hay que mandar el uniform (y se guarda) */
static bool sc_u(Prog *P, int slot, const void *v, size_t n)
{
    if (P->ulen[slot] == n && !memcmp(P->u[slot], v, n)) {
        return false;
    }
    memcpy(P->u[slot], v, n);
    P->ulen[slot] = (uint8_t)n;
    return true;
}

/* ---- la caché de shaders en la SD: un archivo por programa, con el binario
 * que da el driver. La clave incluye el driver y el fragment shader, así que
 * si cambian (otra versión) los archivos viejos simplemente no se usan. */
#ifdef __SWITCH__
#define SHADER_DIR "sdmc:/switch/XboxNX/shaders"
#endif

static const char *shader_dir(void)
{
#ifdef SHADER_DIR
    return SHADER_DIR;
#else
    return getenv("XBOXNX_SHADERS"); /* en la PC (replay): opcional */
#endif
}

static void cache_path(char *o, size_t n, uint64_t key)
{
    snprintf(o, n, "%s/%016llx.bin", shader_dir(), (unsigned long long)(key ^ G.drv_hash));
}

static GLuint cache_load(uint64_t key)
{
    if (!G.bin_ok || !shader_dir()) {
        return 0;
    }
    char path[300];
    cache_path(path, sizeof(path), key);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    uint32_t hdr[2]; /* formato, tamaño */
    GLuint p = 0;
    if (fread(hdr, 4, 2, f) == 2 && hdr[1] > 0 && hdr[1] < (8u << 20)) {
        void *buf = malloc(hdr[1]);
        if (buf && fread(buf, 1, hdr[1], f) == hdr[1]) {
            p = glCreateProgram();
            p_glProgramBinary(p, hdr[0], buf, (GLsizei)hdr[1]);
            GLint ok = 0;
            glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok) {
                glDeleteProgram(p);
                p = 0;
            }
        }
        free(buf);
    }
    fclose(f);
    return p;
}

static void cache_save(uint64_t key, GLuint p)
{
    if (!G.bin_ok || !shader_dir()) {
        return;
    }
    GLint len = 0;
    glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || len >= (8 << 20)) {
        return;
    }
    void *buf = malloc((size_t)len);
    if (!buf) {
        return;
    }
    GLenum fmt = 0;
    GLsizei got = 0;
    p_glGetProgramBinary(p, len, &got, &fmt, buf);
    if (got > 0) {
        mkdir(shader_dir(), 0777);
        char path[300];
        cache_path(path, sizeof(path), key);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t hdr[2] = {fmt, (uint32_t)got};
            fwrite(hdr, 4, 2, f);
            fwrite(buf, 1, (size_t)got, f);
            fclose(f);
            G.st.vs_saved++;
        }
    }
    free(buf);
}

/* El programa del vertex program de este dibujo (compilado una vez). NULL: no se pudo. */
static Prog *vs_prog(const GpuDraw *d)
{
    int free_i = -1, old = -1;
    for (int i = 1; i < MAX_PROG; i++) {
        Prog *P = &G.progs[i];
        if (P->key == d->vs_key && (P->prog || P->bad)) {
            P->used = G.frame;
            return P->bad ? NULL : P;
        }
        if (!P->prog && !P->bad) {
            if (free_i < 0) {
                free_i = i;
            }
        } else if (old < 0 || P->used < G.progs[old].used) {
            old = i;
        }
    }
    if (!d->vs_src) {
        return NULL;
    }
    int i = free_i >= 0 ? free_i : old;
    Prog *P = &G.progs[i];
    if (P->prog) {
        glDeleteProgram(P->prog);
    }
    memset(P, 0, sizeof(*P));
    P->key = d->vs_key;
    P->used = G.frame;
    P->prog = cache_load(d->vs_key);
    if (P->prog) {
        G.st.vs_cached++;
    } else {
        char keep[sizeof(G.info)];
        memcpy(keep, G.info, sizeof(keep));
        P->prog = link_vs(d->vs_src);
        memcpy(G.info, keep, sizeof(keep));
        if (P->prog) {
            cache_save(d->vs_key, P->prog);
        }
    }
    SC.valid = false; /* link / prog_locs cambian el programa puesto */
    if (!P->prog) {
        P->bad = true;
        G.st.vs_bad++;
        return NULL;
    }
    prog_locs(P);
    G.st.vs_progs++;
    return P;
}

/* Las entradas que usa el programa, juntas, al buffer del VAO 2. */
static bool vs_upload(const GpuDraw *d)
{
    unsigned k = (unsigned)__builtin_popcount(d->vs_mask);
    if (!k) {
        k = 1;
    }
    size_t nv = d->vs_pk ? d->vs_pk_n : d->nverts;
    size_t need = nv * k * 4;
    size_t stride = (size_t)k * 16, bytes = need * sizeof(float);
    if (SC.vao != G.vao2) {
        glBindVertexArray(G.vao2);
        SC.vao = G.vao2;
    }
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo2);
    /* etapa 5: el anillo (antes glBufferData con cada dibujo: memoria nueva
     * cada vez). La posición múltiplo del tamaño del vértice: así el dibujo
     * empieza en el vértice pos / stride. Al dar la vuelta, memoria nueva
     * para el anillo entero (lo de antes puede estar todavía en uso). */
    float *o = NULL;
    if (bytes <= RING_SIZE) {
        size_t pos = (G.ring_pos + stride - 1) / stride * stride;
        if (!G.ring_pos || pos + bytes > RING_SIZE) {
            glBufferData(GL_ARRAY_BUFFER, RING_SIZE, NULL, GL_STREAM_DRAW);
            pos = 0;
        }
        o = glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)pos, (GLsizeiptr)bytes,
                             GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
        if (o) {
            G.draw_first = (GLint)(pos / stride);
            G.ring_pos = pos + bytes;
            if (!G.ring_pos) {
                G.ring_pos = 1;
            }
        }
    }
    bool mapped = o != NULL;
    if (!mapped) {
        if (need > G.vsbuf_n) {
            free(G.vsbuf);
            G.vsbuf = malloc(need * sizeof(float));
            G.vsbuf_n = G.vsbuf ? need : 0;
            if (!G.vsbuf) {
                return false;
            }
        }
        o = G.vsbuf;
    }
    float *o0 = o;
    if (d->vs_pk) { /* ya empaquetados (nv2a_sw) */
        memcpy(o, d->vs_pk, bytes);
    }
    for (unsigned v = 0; !d->vs_pk && v < d->nverts; v++) {
        const float *in = d->vs_in + (size_t)v * 64;
        for (unsigned a = 0; a < 16; a++) {
            if (d->vs_mask & (1u << a)) {
                memcpy(o, in + a * 4, 16);
                o += 4;
            }
        }
    }
    if (mapped) {
        glUnmapBuffer(GL_ARRAY_BUFFER);
    }
    if (G.vao2_mask != d->vs_mask) {
        unsigned at = 0;
        for (unsigned a = 0; a < 16; a++) {
            if (d->vs_mask & (1u << a)) {
                glEnableVertexAttribArray(a);
                glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, (GLsizei)(k * 16), (const void *)(uintptr_t)(at * 16));
                at++;
            } else {
                glDisableVertexAttribArray(a);
            }
        }
        G.vao2_mask = d->vs_mask;
    }
    if (!mapped) {
        G.ring_pos = 0; /* el anillo se rehace la próxima vez */
        G.draw_first = 0;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, o0, GL_STREAM_DRAW);
    }
    return true;
}

/* Índices fijos para quads (8) y tiras de quads (9) hasta n vértices: se hacen
 * una vez (antes se subían con cada dibujo). */
static bool quad_ibo(int which, uint32_t n)
{
    if (!G.qibo[which]) {
        glGenBuffers(1, &G.qibo[which]);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.qibo[which]);
    if (G.qibo_n[which] >= n) {
        return true;
    }
    uint32_t cap = 4096;
    while (cap < n) {
        cap *= 2;
    }
    size_t need = (size_t)cap * 3;
    uint32_t *idx = malloc(need * 4);
    if (!idx) {
        return false;
    }
    size_t k = 0;
    if (which == 0) {
        for (uint32_t i = 0; i + 3 < cap; i += 4) {
            uint32_t q[6] = {i, i + 1, i + 2, i, i + 2, i + 3};
            memcpy(&idx[k], q, sizeof(q));
            k += 6;
        }
    } else {
        for (uint32_t i = 0; i + 3 < cap; i += 2) {
            uint32_t q[6] = {i, i + 1, i + 3, i, i + 3, i + 2};
            memcpy(&idx[k], q, sizeof(q));
            k += 6;
        }
    }
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(k * 4), idx, GL_STATIC_DRAW);
    free(idx);
    G.qibo_n[which] = cap;
    return true;
}

static int surf_find(uint32_t pa)
{
    for (int i = 0; i < MAX_SURF; i++) {
        if (G.surf[i].tex && G.surf[i].pa == pa) {
            return i;
        }
    }
    return -1;
}

/* Etapa 5: qué textura tiene cada unidad (para no volver a ponerla en cada
 * dibujo: cada glBindTexture hace que el driver revalide las texturas). */
static int g_unit;
static GLuint g_bound[8][2];

static void tex_unit(int u)
{
    if (g_unit != u) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        g_unit = u;
    }
}

static void tex_bind(GLenum target, GLuint tex)
{
    int k = target == GL_TEXTURE_CUBE_MAP ? 1 : 0;
    if (g_bound[g_unit][k] != tex) {
        glBindTexture(target, tex);
        g_bound[g_unit][k] = tex;
    }
}

static void tex_del(GLuint *tex)
{
    for (int u = 0; u < 8; u++) {
        for (int k = 0; k < 2; k++) {
            if (g_bound[u][k] == *tex) {
                g_bound[u][k] = 0;
            }
        }
    }
    glDeleteTextures(1, tex);
}

/* Etapa 5: el cuadro terminado se lee a un PBO sin esperar a la GPU (antes
 * glReadPixels a la RAM paraba todo hasta que la GPU acababa el cuadro); se
 * copia a la RAM cuando la GPU terminó (o cuando alguien necesita la RAM). */
static struct {
    GLuint pbo;
    size_t cap;
    GLsync fence;
    bool on;
    uint32_t pa, bytes;
} RB;

static void rb_finish_(bool wait);

static void rb_finish(bool wait)
{
    if (!RB.on) {
        return;
    }
    uint64_t t0 = now_ns();
    ZONE_SET(Z_READ);
    rb_finish_(wait);
    ZONE_END();
    G.st.ns_read += now_ns() - t0;
}

static void rb_finish_(bool wait)
{
    GLenum r = glClientWaitSync(RB.fence, GL_SYNC_FLUSH_COMMANDS_BIT, wait ? 2000000000ull : 0);
    if (!wait && r == GL_TIMEOUT_EXPIRED) {
        return;
    }
    glDeleteSync(RB.fence);
    RB.on = false;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, RB.pbo);
    const void *p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)RB.bytes, GL_MAP_READ_BIT);
    if (p && G.ram && (uint64_t)RB.pa + RB.bytes <= G.ram_size) {
        memcpy(G.ram + RB.pa, p, RB.bytes);
    }
    if (p) {
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

/* Lo que la GPU ya terminó, a la RAM, sin esperar (antes de mostrar la pantalla). */
void gpu_readback_poll(void)
{
    if (G.ready) {
        rb_finish(false);
    }
}

/* La superficie i a la RAM sin esperar: al PBO, y a la RAM luego. */
static void surf_flush_async(int i)
{
    Surf *s = &G.surf[i];
    if (!s->dirty || !G.ram) {
        return;
    }
    rb_finish(true); /* la anterior primero */
    uint64_t end = (uint64_t)s->pa + (uint64_t)s->pitch * (uint64_t)s->h;
    if (end > G.ram_size) {
        s->dirty = false;
        return;
    }
    size_t bytes = (size_t)s->pitch * (size_t)s->h;
    if (!RB.pbo) {
        glGenBuffers(1, &RB.pbo);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, RB.pbo);
    if (RB.cap < bytes) {
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_READ);
        RB.cap = bytes;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    SC.valid = false;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(s->pitch / 4));
    glReadPixels(0, 0, s->w, s->h, GL_BGRA, GL_UNSIGNED_BYTE, (void *)0);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    RB.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    RB.on = RB.fence != NULL;
    RB.pa = s->pa;
    RB.bytes = (uint32_t)bytes;
    s->dirty = false;
    G.st.flushes++;
}

static void surf_flush(int i)
{
    Surf *s = &G.surf[i];
    if (RB.on && s->pa < RB.pa + RB.bytes && RB.pa < s->pa + s->pitch * (uint32_t)s->h) {
        rb_finish(true); /* lo que venía en camino, antes (si no, lo pisaría después) */
    }
    if (!s->dirty || !G.ram) {
        return;
    }
    uint64_t end = (uint64_t)s->pa + (uint64_t)s->pitch * (uint64_t)s->h;
    if (end > G.ram_size) {
        s->dirty = false;
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    SC.valid = false; /* la caché de estado de gpu_draw ya no vale */
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(s->pitch / 4));
    glReadPixels(0, 0, s->w, s->h, GL_BGRA, GL_UNSIGNED_BYTE, G.ram + s->pa);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    s->dirty = false;
    G.st.flushes++;
}

static void surf_upload(Surf *s)
{
    uint64_t end = (uint64_t)s->pa + (uint64_t)s->pitch * (uint64_t)s->h;
    tex_bind(GL_TEXTURE_2D, s->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(s->pitch / 4));
    glTexImage2D(GL_TEXTURE_2D, 0, s->fmt == 8 ? GL_RGBA8 : GL_RGB8, s->w, s->h, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                 G.ram && end <= G.ram_size ? G.ram + s->pa : NULL);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    G.st.uploads++;
}

static void surf_free(int i)
{
    Surf *s = &G.surf[i];
    surf_flush(i);
    for (int f = 0; f < MAX_FBO; f++) {
        if (G.fbo[f].fbo && G.fbo[f].surf == i) {
            glDeleteFramebuffers(1, &G.fbo[f].fbo);
            G.fbo[f].fbo = 0;
        }
    }
    glDeleteFramebuffers(1, &s->fbo);
    tex_del(&s->tex);
    memset(s, 0, sizeof(*s));
}

/* La superficie de color en pa (la crea o la agranda). */
static int surf_get(uint32_t pa, uint32_t pitch, int h, unsigned fmt)
{
    int w = (int)(pitch / 4);
    int i = surf_find(pa);
    if (i >= 0) {
        Surf *s = &G.surf[i];
        if (s->pitch == pitch && s->h >= h && s->fmt == fmt) {
            s->used = G.frame;
            return i;
        }
        surf_free(i); /* otro tamaño: a la RAM y de nuevo */
    }
    int best = -1;
    for (i = 0; i < MAX_SURF; i++) {
        if (!G.surf[i].tex) {
            best = i;
            break;
        }
        if (best < 0 || G.surf[i].used < G.surf[best].used) {
            best = i;
        }
    }
    if (G.surf[best].tex) {
        surf_free(best);
    }
    Surf *s = &G.surf[best];
    s->pa = pa;
    s->pitch = pitch;
    s->w = w;
    s->h = h;
    s->fmt = fmt;
    s->used = G.frame;
    glGenTextures(1, &s->tex);
    tex_bind(GL_TEXTURE_2D, s->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    surf_upload(s);
    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    SC.valid = false; /* la caché de estado de gpu_draw ya no vale */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    G.st.surfaces++;
    return best;
}

static int zeta_get(uint32_t pa, uint32_t zpitch, int w, int h)
{
    int best = -1;
    for (int i = 0; i < MAX_ZETA; i++) {
        Zeta *z = &G.zeta[i];
        if (z->rb && z->pa == pa) {
            if (z->w >= w && z->h >= h) {
                z->used = G.frame;
                return i;
            }
            best = i; /* más grande: de nuevo */
            break;
        }
    }
    if (best < 0) {
        for (int i = 0; i < MAX_ZETA; i++) {
            if (!G.zeta[i].rb) {
                best = i;
                break;
            }
            if (best < 0 || G.zeta[i].used < G.zeta[best].used) {
                best = i;
            }
        }
    }
    Zeta *z = &G.zeta[best];
    if (z->rb) {
        for (int f = 0; f < MAX_FBO; f++) {
            if (G.fbo[f].fbo && G.fbo[f].zeta == best) {
                glDeleteFramebuffers(1, &G.fbo[f].fbo);
                G.fbo[f].fbo = 0;
            }
        }
        glDeleteRenderbuffers(1, &z->rb);
    }
    z->pa = pa;
    z->zpitch = zpitch;
    z->w = w;
    z->h = h;
    z->used = G.frame;
    glGenRenderbuffers(1, &z->rb);
    glBindRenderbuffer(GL_RENDERBUFFER, z->rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    z->pend = true;
    z->pend_z = 1.0;
    z->pend_s = 0;
    return best;
}

static GLuint fbo_get(int si, int zi)
{
    for (int f = 0; f < MAX_FBO; f++) {
        if (G.fbo[f].fbo && G.fbo[f].surf == si && G.fbo[f].zeta == zi) {
            return G.fbo[f].fbo;
        }
    }
    if (zi < 0) {
        return G.surf[si].fbo;
    }
    int slot = -1;
    for (int f = 0; f < MAX_FBO; f++) {
        if (!G.fbo[f].fbo) {
            slot = f;
            break;
        }
    }
    if (slot < 0) {
        slot = (int)(G.frame % MAX_FBO);
        glDeleteFramebuffers(1, &G.fbo[slot].fbo);
    }
    Fbo *o = &G.fbo[slot];
    o->surf = si;
    o->zeta = zi;
    glGenFramebuffers(1, &o->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, o->fbo);
    SC.valid = false; /* la caché de estado de gpu_draw ya no vale */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.surf[si].tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, G.zeta[zi].rb);
    Zeta *z = &G.zeta[zi];
    if (z->pend) {
        z->pend = false;
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearDepth(z->pend_z);
        glClearStencil(z->pend_s);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    return o->fbo;
}

/* Las superficies con datos nuevos que se cruzan con [pa, pa+len): a la RAM. */
static void flush_range(uint32_t pa, uint32_t len)
{
    if (RB.on && pa < RB.pa + RB.bytes && RB.pa < pa + len) {
        rb_finish(true); /* alguien va a leer esa RAM */
    }
    for (int i = 0; i < MAX_SURF; i++) {
        Surf *s = &G.surf[i];
        if (s->tex && s->dirty && pa < s->pa + s->pitch * (uint32_t)s->h && s->pa < pa + len) {
            surf_flush(i);
        }
    }
}

void gpu_flush_surface(uint32_t pa)
{
    if (!G.ready) {
        return;
    }
    int i = surf_find(pa);
    if (i >= 0) {
        surf_flush(i);
    }
}

void gpu_flush_surface_async(uint32_t pa)
{
    if (!G.ready) {
        return;
    }
    int i = surf_find(pa);
    if (i >= 0) {
        surf_flush_async(i);
    }
}

void gpu_flush_all(void)
{
    if (!G.ready) {
        return;
    }
    rb_finish(true);
    for (int i = 0; i < MAX_SURF; i++) {
        if (G.surf[i].tex) {
            surf_flush(i);
        }
    }
}

void gpu_invalidate(uint32_t pa, uint32_t len)
{
    if (!G.ready) {
        return;
    }
    for (int i = 0; i < MAX_SURF; i++) {
        Surf *s = &G.surf[i];
        if (s->tex && pa < s->pa + s->pitch * (uint32_t)s->h && s->pa < pa + len) {
            s->dirty = false; /* la RAM manda */
            surf_upload(s);
        }
    }
    for (int i = 0; i < MAX_TEX; i++) {
        if (G.tex[i].tex && G.tex[i].addr >= pa && G.tex[i].addr < pa + len) {
            G.tex[i].checked = 0;
        }
    }
}

/* ------------------------------------------------------------ texturas */

static uint64_t hash_bytes(const uint8_t *p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull ^ n;
    if (!p) {
        return h;
    }
    /* las grandes, una de cada 8 palabras (basta para notar un cambio) */
    uint32_t step = n > 65536 ? 64 : 8;
    for (uint32_t i = 0; i + 8 <= n; i += step) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

/* el modo de dirección como lo pone wrap_mode: 1 wrap, 2 mirror, 0 clamp */
static unsigned wkey(unsigned a) { return a == 1 ? 1u : a == 2 ? 2u : 0u; }

static void wrap_mode(GLenum target, unsigned au, unsigned av)
{
    GLint ws = au == 1 ? GL_REPEAT : au == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
    GLint wt = av == 1 ? GL_REPEAT : av == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(target, GL_TEXTURE_WRAP_S, ws);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wt);
}

/* La textura de GL de una etapa (subida o tomada de una superficie);
 * *scale: por cuánto multiplicar las coordenadas. */
static GLuint tex_get(const GpuTex *t, float scale[2])
{
    scale[0] = scale[1] = 1;
    if (t->linear) {
        int si = surf_find(t->addr);
        if (si >= 0 && !t->cube) {
            Surf *s = &G.surf[si];
            s->used = G.frame;
            scale[0] = 1.0f / (float)s->w;
            scale[1] = 1.0f / (float)s->h;
            tex_bind(GL_TEXTURE_2D, s->tex);
            uint8_t key = (uint8_t)(1 + ((t->bilinear ? 1 : 0) | wkey(t->au) << 1 | wkey(t->av) << 3));
            if (s->params != key) { /* etapa 5: solo si cambió (cada llamada hace revalidar al driver) */
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t->bilinear ? GL_LINEAR : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->bilinear ? GL_LINEAR : GL_NEAREST);
                wrap_mode(GL_TEXTURE_2D, t->au, t->av);
                s->params = key;
            }
            return s->tex;
        }
        scale[0] = 1.0f / (float)t->w;
        scale[1] = 1.0f / (float)t->h;
    }
    flush_range(t->addr, t->bytes);
    GLenum target = t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    int hit = -1, free_i = -1, old = -1;
    for (int i = 0; i < MAX_TEX; i++) {
        TexEnt *e = &G.tex[i];
        if (!e->tex) {
            if (free_i < 0) {
                free_i = i;
            }
            continue;
        }
        if (e->addr == t->addr && e->fmt == t->fmt && e->w == t->w && e->h == t->h && e->cube == t->cube) {
            hit = i;
            break;
        }
        if (old < 0 || e->used < G.tex[old].used) {
            old = i;
        }
    }
    TexEnt *e;
    if (hit >= 0) {
        e = &G.tex[hit];
    } else {
        int i = free_i >= 0 ? free_i : old;
        e = &G.tex[i];
        if (e->tex) {
            tex_del(&e->tex);
        }
        memset(e, 0, sizeof(*e));
        e->addr = t->addr;
        e->fmt = t->fmt;
        e->w = t->w;
        e->h = t->h;
        e->cube = t->cube;
        glGenTextures(1, &e->tex);
        G.st.textures++;
    }
    e->used = G.frame;
    tex_bind(target, e->tex);
    if (e->checked != G.frame || !e->valid) {
        uint64_t h = hash_bytes(t->data, t->bytes) ^ (hash_bytes(t->pal, t->pal_bytes) * 31);
        e->checked = G.frame;
        if (!e->valid || h != e->hash) {
            uint64_t tt = now_ns();
            int zt = g_zone;
            g_zone = Z_TEX;
            size_t need = (size_t)t->w * t->h;
            if (need > G.scratch_n) {
                free(G.scratch);
                G.scratch = malloc(need * 4);
                G.scratch_n = G.scratch ? need : 0;
            }
            if (!G.scratch) {
                return 0;
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            for (int f = 0; f < (t->cube ? 6 : 1); f++) {
                t->decode(t->stage, f, G.scratch);
                glTexImage2D(t->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f : GL_TEXTURE_2D, 0, GL_RGBA8,
                             (GLsizei)t->w, (GLsizei)t->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, G.scratch);
                G.st.upload_bytes += need * 4;
            }
            e->hash = h;
            e->valid = true;
            G.st.uploads++;
            G.st.ns_tex += now_ns() - tt;
            g_zone = zt;
        }
    }
    uint8_t key = (uint8_t)(1 + ((t->bilinear ? 1 : 0) | wkey(t->au) << 1 | wkey(t->av) << 3 | (t->cube ? 32 : 0)));
    if (e->params != key) { /* etapa 5: solo si cambió */
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, t->bilinear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, t->bilinear ? GL_LINEAR : GL_NEAREST);
        if (t->cube) {
            glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        } else {
            wrap_mode(target, t->au, t->av);
        }
        e->params = key;
    }
    return e->tex;
}

/* ------------------------------------------------------------ dibujo */

static GLenum blend_factor(uint32_t f)
{
    switch (f) {
    case 0: case 1:
    case 0x300: case 0x301: case 0x302: case 0x303: case 0x304: case 0x305: case 0x306: case 0x307: case 0x308:
    case 0x8001: case 0x8002: case 0x8003: case 0x8004:
        return f;
    default:
        return 1;
    }
}

static GLenum blend_eq(uint32_t e)
{
    switch (e) {
    case 0x8006: case 0x8007: case 0x8008: case 0x800A: case 0x800B: return e;
    default: return GL_FUNC_ADD;
    }
}

static void argb_vec(uint32_t v, float o[4])
{
    o[0] = ((v >> 16) & 255) / 255.0f, o[1] = ((v >> 8) & 255) / 255.0f, o[2] = (v & 255) / 255.0f;
    o[3] = (v >> 24) / 255.0f;
}

static bool gpu_draw_(const GpuDraw *d);

/* Las pruebas de visibilidad pendientes (una consulta de GL por dibujo). */
#define MAX_Q 512
static GLuint g_q[MAX_Q];
static unsigned g_qn;

bool gpu_draw(const GpuDraw *d)
{
    ZONE_SET(Z_GL_STATE);
    uint64_t t0 = now_ns();
    bool q = d->query && G.on && g_qn < MAX_Q;
    if (q) {
        if (!g_q[g_qn]) {
            glGenQueries(1, &g_q[g_qn]);
        }
        glBeginQuery(GL_SAMPLES_PASSED, g_q[g_qn]);
    }
    bool ok = gpu_draw_(d);
    if (q) {
        glEndQuery(GL_SAMPLES_PASSED);
        if (ok) {
            g_qn++;
        }
    }
    G.st.ns_draw += now_ns() - t0;
    ZONE_END();
    return ok;
}

uint32_t gpu_query_collect(void)
{
    if (!G.ready) {
        return 0;
    }
    ZONE_SET(Z_QUERY);
    uint64_t sum = 0, t0 = now_ns();
    for (unsigned i = 0; i < g_qn; i++) {
        GLuint n = 0;
        glGetQueryObjectuiv(g_q[i], GL_QUERY_RESULT, &n);
        sum += n;
    }
    g_qn = 0;
    G.st.queries++;
    G.st.ns_query += now_ns() - t0;
    ZONE_END();
    return sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sum;
}

void gpu_query_reset(void) { g_qn = 0; }

static bool gpu_draw_(const GpuDraw *d)
{
    if (!G.on || !G.ram || d->fmt < 4 || d->fmt > 8 || d->w <= 0 || d->h <= 0) {
        G.st.fallbacks++;
        return false;
    }
    if (d->prim < 5 || d->prim > 10 || d->nverts < 3) {
        return true; /* puntos y líneas: todavía no (el software tampoco) */
    }
    Prog *P = &G.progs[0];
    g_zone = Z_GL_PROG;
    if (d->vs_in || d->vs_pk) {
        P = vs_prog(d);
        if (!P) {
            return false; /* lo hace la CPU (y luego este mismo camino con los vértices listos) */
        }
    }
    g_zone = Z_GL_TARGET;
    int si = surf_get(d->color_pa, d->pitch, d->h, d->fmt);
    int zi = -1;
    if (d->zfmt && d->zeta_pa) {
        zi = zeta_get(d->zeta_pa, d->zpitch, G.surf[si].w, G.surf[si].h);
    }
    GLuint fbo = fbo_get(si, zi);
    sc_check(); /* fbo_get / zeta_get pueden haber cambiado el estado */
    Surf *s = &G.surf[si];
    if (SC.fbo != fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        SC.fbo = fbo;
    }
    if (SC.vp[2] != s->w || SC.vp[3] != s->h) {
        glViewport(0, 0, s->w, s->h);
        SC.vp[2] = s->w, SC.vp[3] = s->h;
    }
    sc_cap(GL_SCISSOR_TEST, 1, true);
    int scv[4] = {d->x0, d->y0, d->x1 - d->x0, d->y1 - d->y0};
    if (memcmp(SC.sc, scv, sizeof(scv))) {
        glScissor(scv[0], scv[1], scv[2], scv[3]);
        memcpy(SC.sc, scv, sizeof(scv));
    }
    sc_cap(GL_DEPTH_CLAMP, 2, true);
    g_zone = Z_GL_STATE;
    /* estado */
    if (zi >= 0 && d->dtest) {
        sc_cap(GL_DEPTH_TEST, 4, true);
        GLenum df = d->dfunc >= 0x200 && d->dfunc <= 0x207 ? d->dfunc : 0x207;
        if (SC.dfunc != df) {
            glDepthFunc(df);
            SC.dfunc = df;
        }
        int dm = d->dmask ? 1 : 0;
        if (SC.dmask != dm) {
            glDepthMask(dm ? GL_TRUE : GL_FALSE);
            SC.dmask = dm;
        }
    } else {
        sc_cap(GL_DEPTH_TEST, 4, false);
        if (SC.dmask != 0) {
            glDepthMask(GL_FALSE);
            SC.dmask = 0;
        }
    }
    if (d->cull && (d->cull_face == 0x404 || d->cull_face == 0x405 || d->cull_face == 0x408)) {
        sc_cap(GL_CULL_FACE, 8, true);
        if (SC.cullf != d->cull_face) {
            glCullFace(d->cull_face);
            SC.cullf = d->cull_face;
        }
        /* la y de GL va al revés de la pantalla: el sentido se invierte */
        GLenum ff = d->front_face == 0x900 ? GL_CCW : GL_CW;
        if (SC.frontf != ff) {
            glFrontFace(ff);
            SC.frontf = ff;
        }
    } else {
        sc_cap(GL_CULL_FACE, 8, false);
    }
    if (d->blend) {
        sc_cap(GL_BLEND, 16, true);
        GLenum bs = blend_factor(d->sfac), bd = blend_factor(d->dfac), be = blend_eq(d->beq);
        if (SC.bsrc != bs || SC.bdst != bd) {
            glBlendFuncSeparate(bs, bd, bs, bd);
            SC.bsrc = bs, SC.bdst = bd;
        }
        if (SC.beq != be) {
            glBlendEquation(be);
            SC.beq = be;
        }
        float bc[4];
        argb_vec(d->bcolor, bc);
        if (memcmp(SC.bcol, bc, sizeof(bc))) {
            glBlendColor(bc[0], bc[1], bc[2], bc[3]);
            memcpy(SC.bcol, bc, sizeof(bc));
        }
    } else {
        sc_cap(GL_BLEND, 16, false);
    }
    if (SC.cmask != d->cmask) {
        glColorMask((d->cmask & 0x00010000u) != 0, (d->cmask & 0x00000100u) != 0, (d->cmask & 0x00000001u) != 0,
                    (d->cmask & 0x01000000u) != 0);
        SC.cmask = d->cmask;
    }
    /* texturas */
    if (SC.prog != P->prog) {
        glUseProgram(P->prog);
        SC.prog = P->prog;
    }
    g_zone = Z_GL_TEXS;
    GLint tmode[4] = {0}, tcube[4] = {0};
    float tscale[8];
    for (int i = 0; i < 4; i++) {
        const GpuTex *t = &d->tex[i];
        tscale[2 * i] = tscale[2 * i + 1] = 1;
        if (!t->on) {
            continue;
        }
        tmode[i] = (GLint)t->mode;
        if (t->mode == 4) {
            continue;
        }
        tex_unit(t->cube ? 4 + i : i);
        GLuint tx = tex_get(t, &tscale[2 * i]);
        if (!tx) {
            tmode[i] = 0;
        }
        tcube[i] = t->cube;
    }
    tex_unit(0);
    if (!SC.valid) { /* tex_get pudo copiar una superficie a la RAM */
        sc_check();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        SC.fbo = fbo;
        glUseProgram(P->prog);
        SC.prog = P->prog;
    }
    g_zone = Z_GL_UNIF;
    /* uniforms (solo los que cambiaron) */
    float vp[4] = {2.0f / (float)s->w, 2.0f / (float)s->h, d->zmax > 0 ? 2.0f / d->zmax : 1.0f, 0};
    if (sc_u(P, 0, vp, sizeof(vp))) glUniform4fv(P->u_vp, 1, vp);
    if (sc_u(P, 1, tmode, sizeof(tmode))) glUniform1iv(P->u_tmode, 4, tmode);
    if (sc_u(P, 2, tcube, sizeof(tcube))) glUniform1iv(P->u_tcube, 4, tcube);
    if (sc_u(P, 3, tscale, sizeof(tscale))) glUniform2fv(P->u_tscale, 4, tscale);
    if (sc_u(P, 4, &d->cctl, 4)) glUniform1ui(P->u_cctl, d->cctl);
    if (sc_u(P, 5, &d->fcw0, 4)) glUniform1ui(P->u_fcw0, d->fcw0);
    if (sc_u(P, 6, &d->fcw1, 4)) glUniform1ui(P->u_fcw1, d->fcw1);
    if (sc_u(P, 7, d->c_icw, 32)) glUniform1uiv(P->u_icw, 8, d->c_icw);
    if (sc_u(P, 8, d->c_ocw, 32)) glUniform1uiv(P->u_ocw, 8, d->c_ocw);
    if (sc_u(P, 9, d->a_icw, 32)) glUniform1uiv(P->u_aicw, 8, d->a_icw);
    if (sc_u(P, 10, d->a_ocw, 32)) glUniform1uiv(P->u_aocw, 8, d->a_ocw);
    if (sc_u(P, 11, d->factor0, 32) | sc_u(P, 12, d->factor1, 32)) {
        float f0[32], f1[32];
        for (int i = 0; i < 8; i++) {
            argb_vec(d->factor0[i], &f0[4 * i]);
            argb_vec(d->factor1[i], &f1[4 * i]);
        }
        glUniform4fv(P->u_f0, 8, f0);
        glUniform4fv(P->u_f1, 8, f1);
    }
    if (sc_u(P, 13, &d->fog_color, 4)) {
        float fog[4];
        argb_vec(d->fog_color, fog);
        glUniform4fv(P->u_fog, 1, fog);
    }
    int at = d->atest;
    if (sc_u(P, 14, &at, 4)) glUniform1i(P->u_atest, d->atest);
    if (sc_u(P, 15, &d->afunc, 4)) glUniform1ui(P->u_afunc, d->afunc);
    if (sc_u(P, 16, &d->aref, 4)) glUniform1f(P->u_aref, (float)(d->aref & 0xFF));
    /* vértices */
    if (d->vs_in || d->vs_pk) {
        if (!P->c_ok) {
            glUniform4fv(P->u_c, 192, d->vs_consts);
            memcpy(P->c, d->vs_consts, sizeof(P->c));
            P->c_ok = true;
        } else {
            /* etapa 5: solo las constantes que cambiaron (el juego cambia unas
             * pocas, las matrices, entre dibujo y dibujo) */
            int first = -1, last = -1;
            for (int k = 0; k < 192; k++) {
                if (memcmp(&P->c[k * 4], &d->vs_consts[k * 4], 16)) {
                    if (first < 0) {
                        first = k;
                    }
                    last = k;
                }
            }
            if (first >= 0) {
                glUniform4fv(P->u_c + first, last - first + 1, &d->vs_consts[first * 4]);
                memcpy(&P->c[first * 4], &d->vs_consts[first * 4], (size_t)(last - first + 1) * 16);
                G.st.vs_const_vec += (unsigned)(last - first + 1);
            }
        }
        uint64_t tv = now_ns();
        g_zone = Z_GL_VERT;
        bool up = vs_upload(d);
        g_zone = Z_GL_STATE;
        G.st.ns_vbuf += now_ns() - tv;
        if (!up) {
            return false;
        }
        G.st.vs_draws++;
    } else {
        if (SC.vao != G.vao) {
            glBindVertexArray(G.vao);
            SC.vao = G.vao;
        }
        glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)d->nverts * GPU_VERT_FLOATS * 4, d->verts, GL_STREAM_DRAW);
        G.draw_first = 0;
    }
    unsigned n = d->nverts;
    GLint f0 = G.draw_first;
    g_zone = Z_GL_CALL;
    uint64_t tcall = now_ns();
    if (d->vs_idx && d->vs_nidx) {
        /* etapa 5: con índices (cada vértice una vez): al anillo de índices */
        size_t ib = (size_t)d->vs_nidx * 4;
        if (ib <= IRING_SIZE) {
            if (!G.iring) {
                glGenBuffers(1, &G.iring);
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.iring);
            size_t pos = (G.iring_pos + 3) & ~(size_t)3;
            if (!G.iring_pos || pos + ib > IRING_SIZE) {
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, IRING_SIZE, NULL, GL_STREAM_DRAW);
                pos = 0;
            }
            void *ip = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)pos, (GLsizeiptr)ib,
                                        GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
            if (ip) {
                memcpy(ip, d->vs_idx, ib);
                glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
                G.iring_pos = pos + ib;
                GLenum mode = d->prim == 6 ? GL_TRIANGLE_STRIP : (d->prim == 7 || d->prim == 10) ? GL_TRIANGLE_FAN : GL_TRIANGLES;
                glDrawElementsBaseVertex(mode, (GLsizei)d->vs_nidx, GL_UNSIGNED_INT, (const void *)(uintptr_t)pos, f0);
                s->dirty = true;
                G.st.draws++;
                G.st.indexed++;
                G.st.ns_call += now_ns() - tcall;
                return true;
            }
            G.iring_pos = 0;
        }
        /* sin anillo: como antes con glBufferData */
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ib, d->vs_idx, GL_STREAM_DRAW);
        GLenum mode = d->prim == 6 ? GL_TRIANGLE_STRIP : (d->prim == 7 || d->prim == 10) ? GL_TRIANGLE_FAN : GL_TRIANGLES;
        glDrawElementsBaseVertex(mode, (GLsizei)d->vs_nidx, GL_UNSIGNED_INT, 0, f0);
        s->dirty = true;
        G.st.draws++;
        G.st.indexed++;
        G.st.ns_call += now_ns() - tcall;
        return true;
    }
    switch (d->prim) {
    case 5: glDrawArrays(GL_TRIANGLES, f0, (GLsizei)(n - n % 3)); break;
    case 6: glDrawArrays(GL_TRIANGLE_STRIP, f0, (GLsizei)n); break;
    case 7:
    case 10: glDrawArrays(GL_TRIANGLE_FAN, f0, (GLsizei)n); break;
    case 8:
    case 9: {
        if (n >= 4 && quad_ibo(d->prim == 8 ? 0 : 1, n)) {
            uint32_t k = d->prim == 8 ? n / 4 * 6 : (n - 2) / 2 * 6;
            glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)k, GL_UNSIGNED_INT, 0, f0);
            break;
        }
        size_t need = (size_t)n * 3;
        if (need > G.idx_n) {
            free(G.idx);
            G.idx = malloc(need * 4);
            G.idx_n = G.idx ? need : 0;
        }
        if (!G.idx) {
            break;
        }
        size_t k = 0;
        if (d->prim == 8) {
            for (uint32_t i = 0; i + 3 < n; i += 4) {
                uint32_t q[6] = {i, i + 1, i + 2, i, i + 2, i + 3};
                memcpy(&G.idx[k], q, sizeof(q));
                k += 6;
            }
        } else {
            for (uint32_t i = 0; i + 3 < n; i += 2) {
                uint32_t q[6] = {i, i + 1, i + 3, i, i + 3, i + 2};
                memcpy(&G.idx[k], q, sizeof(q));
                k += 6;
            }
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(k * 4), G.idx, GL_STREAM_DRAW);
        glDrawElements(GL_TRIANGLES, (GLsizei)k, GL_UNSIGNED_INT, 0);
        break;
    }
    default: break;
    }
    s->dirty = true; /* el VAO queda puesto para el dibujo siguiente */
    G.st.draws++;
    G.st.ns_call += now_ns() - tcall;
    return true;
}

bool gpu_clear_color(uint32_t pa, uint32_t pitch, int h, int x0, int y0, int x1, int y1, uint32_t argb, unsigned mask)
{
    if (!G.on || !G.ram || pitch < 4 || h <= 0) {
        return false;
    }
    int si = surf_find(pa);
    if (si < 0 || G.surf[si].pitch != pitch || G.surf[si].h < h) {
        si = surf_get(pa, pitch, h, si >= 0 ? G.surf[si].fmt : 8);
    }
    Surf *s = &G.surf[si];
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    SC.valid = false; /* la caché de estado de gpu_draw ya no vale */
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, y0, x1 - x0, y1 - y0);
    glColorMask((mask & 4) != 0, (mask & 2) != 0, (mask & 1) != 0, (mask & 8) != 0);
    float c[4];
    argb_vec(argb, c);
    glClearColor(c[0], c[1], c[2], c[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    s->dirty = true;
    return true;
}

bool gpu_clear_zeta(uint32_t pa, uint32_t zpitch, int h, int x0, int y0, int x1, int y1, uint32_t value, unsigned zfmt,
                    unsigned flags)
{
    if (!G.on || !(flags & 3)) {
        return false;
    }
    int zi = -1;
    for (int i = 0; i < MAX_ZETA; i++) {
        if (G.zeta[i].rb && G.zeta[i].pa == pa) {
            zi = i;
        }
    }
    if (zi < 0) {
        int w = (int)(zpitch / (zfmt == 1 ? 2 : 4));
        zi = zeta_get(pa, zpitch, w, h);
    }
    /* un FBO con cualquier superficie de color y este Z (o el suyo propio) */
    GLuint fbo = 0;
    for (int f = 0; f < MAX_FBO; f++) {
        if (G.fbo[f].fbo && G.fbo[f].zeta == zi) {
            fbo = G.fbo[f].fbo;
        }
    }
    if (!fbo) { /* aún nadie dibuja con él: se borra al ponerle su primer FBO */
        Zeta *z = &G.zeta[zi];
        z->pend = true;
        if (flags & 1) {
            z->pend_z = (zfmt == 1 ? (value & 0xFFFF) / 65535.0 : (value >> 8) / 16777215.0);
        }
        if (flags & 2) {
            z->pend_s = (int)(value & 0xFF);
        }
        return true;
    }
    SC.valid = false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, y0, x1 - x0, y1 - y0);
    GLbitfield bits = 0;
    if (flags & 1) {
        glDepthMask(GL_TRUE);
        double zmax = zfmt == 1 ? 65535.0 : 16777215.0;
        glClearDepth((zfmt == 1 ? (value & 0xFFFF) : (value >> 8)) / zmax);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & 2) {
        glClearStencil((GLint)(value & 0xFF));
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    glClear(bits);
    return true;
}

void gpu_end_frame(void)
{
    G.frame++;
}

void gpu_reset(void)
{
    if (!G.ready) {
        return;
    }
    if (RB.on) { /* lo que venía en camino ya no vale (la RAM puede ser otra) */
        glDeleteSync(RB.fence);
        RB.on = false;
    }
    for (int f = 0; f < MAX_FBO; f++) {
        if (G.fbo[f].fbo) {
            glDeleteFramebuffers(1, &G.fbo[f].fbo);
        }
    }
    memset(G.fbo, 0, sizeof(G.fbo));
    for (int i = 0; i < MAX_SURF; i++) {
        if (G.surf[i].tex) {
            glDeleteFramebuffers(1, &G.surf[i].fbo);
            tex_del(&G.surf[i].tex);
        }
    }
    memset(G.surf, 0, sizeof(G.surf));
    for (int i = 0; i < MAX_ZETA; i++) {
        if (G.zeta[i].rb) {
            glDeleteRenderbuffers(1, &G.zeta[i].rb);
        }
    }
    memset(G.zeta, 0, sizeof(G.zeta));
    for (int i = 0; i < MAX_TEX; i++) {
        if (G.tex[i].tex) {
            tex_del(&G.tex[i].tex);
        }
    }
    memset(G.tex, 0, sizeof(G.tex));
}

/* ------------------------------------------------------------ pantalla */

bool gpu_present_ui(const uint32_t *rgba, int w, int h)
{
    if (!G.ready || G.win == EGL_NO_SURFACE) {
        return false;
    }
    if (!G.ui_tex) {
        glGenTextures(1, &G.ui_tex);
    }
    tex_unit(0);
    tex_bind(GL_TEXTURE_2D, G.ui_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (G.ui_w != w || G.ui_h != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        G.ui_w = w;
        G.ui_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    SC.valid = false; /* la caché de estado de gpu_draw ya no vale */
    EGLint sw = w, sh = h;
    eglQuerySurface(G.dpy, G.win, EGL_WIDTH, &sw);
    eglQuerySurface(G.dpy, G.win, EGL_HEIGHT, &sh);
    glViewport(0, 0, sw, sh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(G.ui_prog);
    glBindVertexArray(G.ui_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    eglSwapBuffers(G.dpy, G.win);
    return true;
}

#else /* XBOXNX_NO_GL: sin GPU, todo por software */

static GpuStats g_nost;
bool gpu_init(void *nwin) { (void)nwin; return false; }
bool gpu_ready(void) { return false; }
void gpu_enable(bool on) { (void)on; }
bool gpu_enabled(void) { return false; }
static char g_noinfo[160] = "sin GL";
const char *gpu_info(void) { return g_noinfo; }
void gpu_set_info(const char *t) { snprintf(g_noinfo, sizeof(g_noinfo), "%s", t); }
void (*gpu_log)(const char *paso);
bool gpu_draw(const GpuDraw *d) { (void)d; return false; }
bool gpu_clear_color(uint32_t pa, uint32_t pitch, int h, int x0, int y0, int x1, int y1, uint32_t argb, unsigned mask)
{
    (void)pa, (void)pitch, (void)h, (void)x0, (void)y0, (void)x1, (void)y1, (void)argb, (void)mask;
    return false;
}
bool gpu_clear_zeta(uint32_t pa, uint32_t zpitch, int h, int x0, int y0, int x1, int y1, uint32_t value, unsigned zfmt,
                    unsigned flags)
{
    (void)pa, (void)zpitch, (void)h, (void)x0, (void)y0, (void)x1, (void)y1, (void)value, (void)zfmt, (void)flags;
    return false;
}
void gpu_flush_surface(uint32_t pa) { (void)pa; }
uint32_t gpu_query_collect(void) { return 0; }
void gpu_query_reset(void) {}
void gpu_flush_surface_async(uint32_t pa) { (void)pa; }
void gpu_readback_poll(void) {}
void gpu_flush_all(void) {}
void gpu_invalidate(uint32_t pa, uint32_t len) { (void)pa, (void)len; }
void gpu_end_frame(void) {}
bool gpu_present_ui(const uint32_t *rgba, int w, int h) { (void)rgba, (void)w, (void)h; return false; }
const GpuStats *gpu_stats(void) { return &g_nost; }
void gpu_set_ram(uint8_t *ram, uint32_t size) { (void)ram, (void)size; }
void gpu_reset(void) {}

#endif
