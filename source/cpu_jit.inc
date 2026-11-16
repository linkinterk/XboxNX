/*
 * XboxNX — etapa 2d.2: JIT a ARM64 (incluido en cpu.c, solo en aarch64).
 *
 * Cada bloque decodificado (Insn) que se repite se traduce a una función
 * ARM64 que hace todas sus instrucciones seguidas, sin el bucle de
 * run_blocks. Las más comunes van en código nativo (mov, add/sub/cmp/and/
 * or/xor, test, lea, push, pop, cargas y guardados de 32 bits con la tabla
 * de páginas en línea); las demás llaman a su función de siempre (x_*). Los
 * flags siguen perezosos (lf_*), igual que en el intérprete.
 *
 * La función devuelve cuántas instrucciones terminó. Si una paró la CPU
 * (c->stop), es la siguiente a esas; si una saltó (EIP distinto de next)
 * o escribió en el código del bloque, se sale tras ella.
 */

/* (c, cuántas instrucciones puede hacer como mucho): las que hizo, entre
 * este bloque y los que encadenó */
typedef uint32_t (*JitFn)(Cpu *c, uint64_t budget);
/* etapa 5: la entrada común (prólogo) salta a la entrada del bloque */
typedef uint32_t (*JitEnter)(Cpu *c, uint64_t budget, void *entry);

/* Encadenar: al terminar, un bloque traducido busca el siguiente (por su
 * EIP) en esta tabla y salta a su entrada sin volver a run_blocks. Cada
 * entrada comprueba presupuesto, interrupción, página y generación del
 * código antes de correr; si algo no va, vuelve a run_blocks. */
#define JTAB (1u << 18) /* etapa 5: con 65536 Halo 2 (cientos de miles de bloques) volvía mucho a run_blocks */
typedef struct JSlot {
    uint32_t eip, pad;
    void *entry;
} JSlot;
static JSlot g_jtab[JTAB];
/* v0.18.61, bloques unidos: una salida a un EIP fijo salta por una casilla
 * de 8 bytes junto al código (ldr x13, casilla; br x13). Al principio la
 * casilla apunta a un trocito que busca el bloque en g_jtab; si lo
 * encuentra, escribe su entrada en la casilla (por la vista escribible del
 * búfer: g_link_delta = rw - rx) y la próxima vez se salta directo. La
 * entrada del otro bloque sigue comprobando presupuesto, interrupción,
 * página y código. Sin dos vistas (Horizon sin CodeMemory) no se escribe. */
static int64_t g_link_delta;
static uint8_t g_link_on;
static int g_jit_no_dead = -1; /* CPU_SIN_MUERTOS=1: guardar siempre los flags (pruebas) */
/* veces que corrió cada bloque traducido (para el perfil) */
#define JCNT (1u << 19)
static uint32_t g_jcnt[JCNT];
static uint32_t g_jcnt_next = 1;

static void jit_table_clear(void)
{
    memset(g_jtab, 0, sizeof(g_jtab));
}

#define JIT_SIZE_MAX (128u << 20) /* con 32 MB, Halo 2 lo vaciaba decenas de veces */

static struct {
    uint8_t *rw, *rx;
    size_t size, used;
    uint64_t compiled; /* bloques traducidos (para la pantalla) */
    uint32_t epoch; /* sube al vaciar el búfer: los bloques de antes ya no valen */
    bool tried, ok;
    size_t base;      /* etapa 5: [0, base) son las rutinas comunes (no se vacían) */
    uintptr_t slow_rx; /* la rutina común que llama a la función de C de una instrucción */
    uintptr_t enter_rx, epi_rx; /* el prólogo y el epílogo comunes */
    uintptr_t resolve_rx; /* v0.18.61: busca el bloque de una salida fija y la une */
    uintptr_t same_rx;    /* v0.18.62: lo mismo dentro de la misma página (a la entrada rápida) */
    uintptr_t ic_rx;      /* v0.18.63: busca un EIP variable (ret, saltos indirectos) y lo guarda en la caché del bloque */
#ifdef __SWITCH__
    Jit j;
#endif
} g_jit;

static bool jit_init(void)
{
    if (g_jit.tried) {
        return g_jit.ok;
    }
    g_jit.tried = true;
    g_jit.epoch = 1;
#ifdef __SWITCH__
    /* 32 MB (el código de Halo 2 traducido no cabía en 8 y se vaciaba
     * seguido); si no hay memoria, menos */
    for (g_jit.size = JIT_SIZE_MAX; R_FAILED(jitCreate(&g_jit.j, g_jit.size)); g_jit.size /= 2) {
        if (g_jit.size <= (4u << 20)) {
            return false;
        }
    }
    if (g_jit.j.type != JitType_CodeMemory && R_FAILED(jitTransitionToExecutable(&g_jit.j))) {
        return false;
    }
    g_jit.rw = jitGetRwAddr(&g_jit.j);
    g_jit.rx = jitGetRxAddr(&g_jit.j);
#else
    g_jit.size = JIT_SIZE_MAX;
    void *p = mmap(NULL, g_jit.size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return false;
    }
    g_jit.rw = g_jit.rx = p;
#endif
    g_jit.ok = true;
#ifdef __SWITCH__
    /* las casillas se escriben por la otra vista (solo con CodeMemory) */
    g_link_on = g_jit.j.type == JitType_CodeMemory && !getenv("XBOXNX_SIN_UNIR");
    g_link_delta = (int64_t)((intptr_t)g_jit.rw - (intptr_t)g_jit.rx);
#else
    g_link_on = !getenv("XBOXNX_SIN_UNIR");
    g_link_delta = 0;
#endif
    return true;
}

/* Antes de escribir código (con svcSetProcessMemoryPermission el búfer
 * no puede ser ejecutable y escribible a la vez; con CodeMemory hay dos vistas). */
static void jit_begin_write(void)
{
#ifdef __SWITCH__
    if (g_jit.j.type != JitType_CodeMemory) {
        jitTransitionToWritable(&g_jit.j);
    }
#endif
}

/* El código nuevo [off, off + len) ya escrito: visible para ejecutar. */
static void jit_publish(size_t off, size_t len)
{
#ifdef __SWITCH__
    if (g_jit.j.type == JitType_CodeMemory) {
        armDCacheFlush(g_jit.rw + off, len);
        armICacheInvalidate(g_jit.rx + off, len);
    } else {
        jitTransitionToExecutable(&g_jit.j);
    }
#else
    __builtin___clear_cache((char *)g_jit.rx + off, (char *)g_jit.rx + off + len);
#endif
}

/* ---------------------------------------------------------- emisor */

typedef struct Em {
    uint32_t *p, *end;
    const Insn *insn_rx; /* las Insn del bloque copiadas junto al código */
    uint32_t *exits[512]; /* "b epílogo" por parchear */
    unsigned nexits;
    bool full;
    bool fallback; /* emitiendo el camino de respaldo (no cuenta como lento) */
    unsigned lf_kind; /* LF_* de 32 bits que dejó la instrucción anterior (0: no se sabe) */
    bool lf_zres;     /* v0.18.71: lf_res guarda el resultado (de cualquier tamaño, sin
                       * extender el signo) de add/sub/lógica/inc/dec: ZF = (lf_res == 0) */
    bool lf_dead;     /* etapa 5: los flags de esta instrucción no los lee nadie (no guardarlos) */
    /* v0.18.61: los caminos lentos van al final del bloque (el rápido sigue
     * de largo, sin saltar por encima de ellos) */
    struct {
        uint32_t *back;
        uint16_t first, n, i;
        uint8_t xd, xl; /* v0.18.80: xmm por guardar antes y por recargar después */
        bool fz;
    } cold[96];
    unsigned ncold, npool;
    uint32_t *pool[320];
    /* v0.18.61: los flags de ARM (NZCV) dicen lo mismo que los del x86 de la
     * instrucción anterior (LF_SUB o LF_LOGIC; 0: no) */
    unsigned nzcv, nzcv_prev;
    uint32_t *jcc_site; /* un jcc a mitad del bloque hecho con b.cond: lo une jit_compile */
    bool jcc_none;      /* un jcc a la siguiente instrucción: sin salida */
    uint32_t exit_eip;  /* la última instrucción dejó un EIP fijo (jmp, call): 0 si no */
    bool fj;            /* la última es un jcc con los flags de ARM: dos salidas fijas */
    int fj_cc;
    uint32_t fj_taken, fj_next;
    struct {
        uint32_t *ldr;
        uint32_t target;
    } link[80];
    uint32_t page; /* la página virtual del bloque (las salidas dentro de ella van a la entrada rápida) */
    uint32_t *ic_ldr_eip, *ic_ldr_entry, *ic_adr; /* v0.18.63: la caché del último EIP variable (NULL: no hay) */
    unsigned nlink;
    /* v0.18.80, la página del último operando: x27 = la página en el host,
     * w28 = el offset de [base + index*scale + pc_d0] dentro de ella. Otro
     * operando con la misma base (sin tocar) y un desplazamiento cerca se
     * hace con add/cmp en vez de leer la tabla de páginas otra vez. */
    const Block *cur_b;
    const Insn *cur_in, *ea_in;
    unsigned cur_i;
    uint32_t *ea_mark;   /* el código justo tras emit_ea de cur_in (w9 = su dirección) */
    bool pc_cand;        /* cur_in puede usar o fijar la página (la primera vez) */
    bool pc_live, pc_wr; /* x27/w28 valen (y se comprobó el código: sirve para guardar) */
    uint8_t pc_base, pc_index, pc_scale;
    int32_t pc_d0;
    uint64_t pc_est[4];  /* instrucciones que la fijan: su camino lento la invalida */
    /* v0.18.80, los xmm en v16..v23 dentro del bloque: xl = cargados de
     * c->fp.xmm, xd = cambiados (hay que guardarlos antes de C y al salir) */
    bool xc_on, xc_locked, xc_bad;
    uint8_t xl, xd, xd_before; /* xd_before: al empezar la instrucción */
    uint8_t xc_cd, xc_cl;      /* el camino lento que se emite: guardar xc_cd, recargar xc_cl */
    uint32_t *xw_first;        /* la primera escritura a un xmm de esta instrucción */
    bool fz;                   /* v0.18.80: la instrucción deja NZCV de un fcmp (comiss): su lento lo rehace */
} Em;
/* v0.18.80: lf_kind/nzcv tras comiss/ucomiss hechos aquí (NZCV = fcmp a, b) */
#define LFK_FCMP 32u

static inline void E(Em *e, uint32_t ins)
{
    if (e->p < e->end) {
        *e->p++ = ins;
    } else {
        e->full = true;
    }
}

enum { X0 = 0, X1 = 1, W9 = 9, W10 = 10, W11 = 11, W12 = 12, W13 = 13, XC = 19, XRAM = 20, XPT = 21, XCODE = 22,
       XZR = 31 };
#define OFF(f) ((uint32_t)offsetof(Cpu, f))
#define ROFF(n) (OFF(r) + 4u * (uint32_t)(n))

static void movw(Em *e, int rd, uint32_t v)
{
    E(e, 0x52800000u | ((v & 0xFFFFu) << 5) | (uint32_t)rd);
    if (v >> 16) {
        E(e, 0x72A00000u | ((v >> 16) << 5) | (uint32_t)rd);
    }
}

static void movx(Em *e, int rd, uint64_t v)
{
    E(e, 0xD2800000u | ((uint32_t)(v & 0xFFFFu) << 5) | (uint32_t)rd);
    for (unsigned hw = 1; hw < 4; hw++) {
        uint32_t part = (uint32_t)(v >> (16 * hw)) & 0xFFFFu;
        if (part) {
            E(e, 0xF2800000u | (hw << 21) | (part << 5) | (uint32_t)rd);
        }
    }
}

/* Los 8 registros x86 viven en registros de ARM64 mientras corre el código
 * traducido (eax w2, ecx w3, edx w4, ebx w5, esp w6, ebp w7, esi w8, edi
 * w16): se cargan de c->r al entrar, se guardan al salir y alrededor de cada
 * llamada a C. Las lecturas/escrituras de c->r que piden los emisores (con
 * x19 y un offset < 32) se vuelven movimientos entre registros. */
#define JIT_REGS 1
static const uint8_t k_rmap[8] = {2, 3, 4, 5, 6, 7, 8, 16};
static inline bool reg_off(int rn, uint32_t off) { return JIT_REGS && rn == 19 && off < 32; }
static void mov_w(Em *e, int d, int m)
{
    if (d != m) {
        E(e, 0x2A0003E0u | (uint32_t)m << 16 | (uint32_t)d); /* mov wd, wm */
    }
}
/* c->r de/hacia los registros (stp/ldp de pares de 32 bits) */
static void regs_spill(Em *e)
{
    if (!JIT_REGS) {
        return;
    }
    for (unsigned k = 0; k < 8; k += 2) {
        E(e, 0x29000000u | (k) << 15 | (uint32_t)k_rmap[k + 1] << 10 | 19u << 5 | k_rmap[k]); /* stp */
    }
}
static void regs_load(Em *e)
{
    if (!JIT_REGS) {
        return;
    }
    for (unsigned k = 0; k < 8; k += 2) {
        E(e, 0x29400000u | (k) << 15 | (uint32_t)k_rmap[k + 1] << 10 | 19u << 5 | k_rmap[k]); /* ldp */
    }
}
static void ldrw(Em *e, int rt, int rn, uint32_t off)
{
    if (reg_off(rn, off)) {
        mov_w(e, rt, k_rmap[off / 4]);
        return;
    }
    E(e, 0xB9400000u | ((off / 4) << 10) | (uint32_t)rn << 5 | (uint32_t)rt);
}
static void strw(Em *e, int rt, int rn, uint32_t off)
{
    if (reg_off(rn, off)) {
        mov_w(e, k_rmap[off / 4], rt == 31 ? 31 : rt);
        return;
    }
    E(e, 0xB9000000u | ((off / 4) << 10) | (uint32_t)rn << 5 | (uint32_t)rt);
}
static void ldrx(Em *e, int rt, int rn, uint32_t off) { E(e, 0xF9400000u | ((off / 8) << 10) | (uint32_t)rn << 5 | (uint32_t)rt); }
static void strb(Em *e, int rt, int rn, uint32_t off)
{
    if (reg_off(rn, off)) { /* bfi wR, wt, #8*(off%4), #8 */
        uint32_t lsb = 8u * (off & 3u);
        E(e, 0x33000000u | ((32u - lsb) & 31u) << 16 | 7u << 10 | (uint32_t)rt << 5 | k_rmap[off / 4]);
        return;
    }
    E(e, 0x39000000u | (off << 10) | (uint32_t)rn << 5 | (uint32_t)rt);
}
static void rrr(Em *e, uint32_t op, int rd, int rn, int rm) { E(e, op | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd); }
#define ADDW 0x0B000000u
#define SUBW 0x4B000000u
#define ANDW 0x0A000000u
#define ORRW 0x2A000000u
#define EORW 0x4A000000u
static void addw_lsl(Em *e, int rd, int rn, int rm, unsigned sh) { E(e, 0x0B000000u | (uint32_t)rm << 16 | sh << 10 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void addx_lsl(Em *e, int rd, int rn, int rm, unsigned sh) { E(e, 0x8B000000u | (uint32_t)rm << 16 | sh << 10 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void lslw_i(Em *e, int rd, int rn, unsigned s) { E(e, 0x53000000u | ((32u - s) & 31u) << 16 | (31u - s) << 10 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void lsrw(Em *e, int rd, int rn, unsigned s) { E(e, 0x53000000u | s << 16 | 31u << 10 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void ubfx12(Em *e, int rd, int rn) { E(e, 0x53000000u | 0u << 16 | 11u << 10 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void subwi(Em *e, int rd, int rn, uint32_t imm) { E(e, 0x51000000u | imm << 10 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void addwi(Em *e, int rd, int rn, uint32_t imm) { E(e, 0x11000000u | imm << 10 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void cmpwi(Em *e, int rn, uint32_t imm) { E(e, 0x7100001Fu | imm << 10 | (uint32_t)rn << 5); }
static void cmpw(Em *e, int rn, int rm) { E(e, 0x6B00001Fu | (uint32_t)rm << 16 | (uint32_t)rn << 5); }
static void ldrw_lsl2(Em *e, int rt, int rn, int rm) { E(e, 0xB8607800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt); }
static void ldrw_r(Em *e, int rt, int rn, int rm) { E(e, 0xB8606800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt); }
static void strw_r(Em *e, int rt, int rn, int rm) { E(e, 0xB8206800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt); }
static void ldrb_r(Em *e, int rt, int rn, int rm) { E(e, 0x38606800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt); }
static void movx_r(Em *e, int rd, int rm) { E(e, 0xAA0003E0u | (uint32_t)rm << 16 | (uint32_t)rd); }

/* saltos hacia adelante: se deja el hueco y se parchea */
enum { J_CBZ = 0x34000000u, J_CBNZ = 0x35000000u };
#define COND_EQ 0
#define COND_NE 1
#define COND_HI 8
static uint32_t *fwd_cb(Em *e, uint32_t kind, int rt)
{
    uint32_t *at = e->p;
    E(e, kind | (uint32_t)rt);
    return at;
}
static uint32_t *fwd_bcond(Em *e, int cond)
{
    uint32_t *at = e->p;
    E(e, 0x54000000u | (uint32_t)cond);
    return at;
}
static uint32_t *fwd_b(Em *e)
{
    uint32_t *at = e->p;
    E(e, 0x14000000u);
    return at;
}
static void patch(Em *e, uint32_t *at)
{
    if (e->full || !at) {
        return;
    }
    int32_t d = (int32_t)(e->p - at);
    if ((*at & 0xFC000000u) == 0x14000000u) {
        *at |= (uint32_t)d & 0x3FFFFFFu;
    } else {
        *at |= ((uint32_t)d & 0x7FFFFu) << 5;
    }
}

/* salir devolviendo k instrucciones hechas */
static void xc_store(Em *e, unsigned m);
static void xc_load(Em *e, unsigned m);
static void xc_flush(Em *e);
static inline void exit_k(Em *e, unsigned k)
{
    if (e->xw_first) {
        e->xc_bad = true; /* saldría con la escritura a medias */
    }
    xc_store(e, e->xd);
    movw(e, X0, k);
    if (e->nexits < sizeof(e->exits) / sizeof(e->exits[0])) {
        e->exits[e->nexits++] = fwd_b(e);
    } else {
        e->full = true;
    }
}

/* w9 = la dirección del operando de memoria. v0.18.61: con los registros
 * del x86 que viven en ARM directo y el desplazamiento como inmediato
 * (antes: mov+movk del desplazamiento, mov del registro y add). */
static void emit_ea_at(Em *e, const Insn *in, bool with_fs, int dst)
{
    int32_t d = (int32_t)in->disp;
    int src = -1;
    if (in->base != NOREG && in->index != NOREG) {
        addw_lsl(e, W9, k_rmap[in->base], k_rmap[in->index], in->scale);
        src = W9;
    } else if (in->base != NOREG) {
        src = k_rmap[in->base];
    } else if (in->index != NOREG) {
        if (in->scale) {
            lslw_i(e, W9, k_rmap[in->index], in->scale);
            src = W9;
        } else {
            src = k_rmap[in->index];
        }
    }
    if (src < 0) {
        movw(e, dst, in->disp);
    } else if (d == 0) {
        mov_w(e, dst, src);
    } else if (d > 0 && d < 4096) {
        addwi(e, dst, src, (uint32_t)d);
    } else if (d < 0 && d > -4096) {
        subwi(e, dst, src, (uint32_t)-d);
    } else {
        movw(e, W10, in->disp);
        rrr(e, ADDW, dst, src, W10);
    }
    if (with_fs && in->fs) {
        ldrw(e, W10, XC, OFF(fs_base));
        rrr(e, ADDW, dst, dst, W10);
    }
    if (dst == W9) {
        e->ea_mark = e->p;
        e->ea_in = in;
    }
}
static void emit_ea(Em *e, const Insn *in, bool with_fs) { emit_ea_at(e, in, with_fs, W9); }

/* v0.18.80, pruebas: CPU_JIT_LENTO=1 manda cada consulta de página al camino
 * lento (para probar los caminos lentos con los xmm y la página guardados) */
static int g_jit_lento = -1;
static uint32_t *fwd_cb(Em *e, uint32_t op, int rt);
/* el salto al lento si la página no está (pruebas: 1 siempre, 2 si el bit 4
 * de la dirección está puesto) */
static uint32_t *page_miss(Em *e)
{
    if (g_jit_lento == 1) {
        return fwd_cb(e, J_CBZ, 31);
    }
    if (g_jit_lento == 2) {
        E(e, 0x721C013Fu); /* tst w9, #0x10 */
        E(e, 0x1A8A13EAu); /* csel w10, wzr, w10, ne: como si no estuviera */
    }
    return fwd_cb(e, J_CBZ, W10);
}

/* ---- v0.18.80: la página del operando anterior (x27, w28) ---- */
static bool fp_flag_neutral(const Insn *in);
static bool is_mmx_op(InsnFn f);
#define PC_INVALID 0xFFFF0000u

/* Instrucciones con un operando de memoria del modrm que pueden usarla. */
static bool pc_cand_insn(const Insn *in)
{
    if (in->is_reg || in->fs) {
        return false;
    }
    InsnFn f = in->fn;
    return f == x_mov_ge || f == x_mov_eg || f == x_mov_ei || f == x_alu_eg || f == x_alu_ge || f == x_alu_ei ||
           f == x_test_eg || f == x_test_ei || f == x_movx || f == x_imul || f == x_movd_load || f == x_movd_store ||
           f == x_movq_load || f == x_movq_store || is_mmx_op(f) || fp_flag_neutral(in) ||
           (f == x_interp && (in->sz & (INTERP_MARK | INTERP_0F)) == (INTERP_MARK | INTERP_0F) &&
            (in->aop == 0x2E || in->aop == 0x2F));
}

static unsigned wreg(unsigned r, unsigned sz) { return sz == 1 ? (r & 3u) : r; }

/* ¿Puede la instrucción cambiar el registro r del x86? (si no se sabe, sí) */
static bool pc_writes(const Insn *in, unsigned r)
{
    InsnFn f = in->fn;
    if (f == x_nop || f == x_test_eg || f == x_test_ei || f == x_movq_load || f == x_movq_store ||
        f == x_movd_load || f == x_mmx_shift_i || f == x_emms || is_mmx_op(f)) {
        return false;
    }
    if (f == x_mov_ge || f == x_lea || f == x_imul) {
        return wreg(in->reg, in->sz) == r;
    }
    if (f == x_movx) {
        return in->reg == r;
    }
    if (f == x_mov_eg || f == x_mov_ei || f == x_incdec || f == x_shift || f == x_movd_store) {
        return in->is_reg && wreg(in->rm, in->sz) == r;
    }
    if (f == x_alu_eg || f == x_alu_ei) {
        return in->is_reg && in->aop != OP_CMP && wreg(in->rm, in->sz) == r;
    }
    if (f == x_alu_ge) {
        return in->aop != OP_CMP && wreg(in->reg, in->sz) == r;
    }
    if (f == x_cdq) {
        return r == R_EDX;
    }
    if (f == x_jcc) {
        return false;
    }
    if (f == x_cmov) {
        return wreg(in->reg, in->sz) == r;
    }
    if (f == x_setcc) {
        return in->is_reg && wreg(in->rm, 1) == r;
    }
    if (f == x_interp && (in->sz & (INTERP_MARK | INTERP_0F)) == (INTERP_MARK | INTERP_0F) &&
        (in->aop == 0x2E || in->aop == 0x2F)) {
        return false; /* comiss: solo flags */
    }
    if (f == x_push_r || f == x_push_i) {
        return r == R_ESP;
    }
    if (fp_flag_neutral(in)) {
        unsigned op = in->aop;
        if (in->sz & INTERP_0F) {
            if (op == 0x2C || op == 0x2D || op == 0x50 || op == 0xC5 || op == 0xD7) {
                return in->reg == r;
            }
            if (op == 0x7E) {
                return in->is_reg && in->rm == r;
            }
            return false;
        }
        return op == 0xDF && in->is_reg && in->reg == 4 && r == R_EAX; /* fnstsw ax */
    }
    return true;
}

static bool pc_same(const Insn *a, const Insn *b)
{
    return a->base == b->base && a->index == b->index && (a->index == NOREG || a->scale == b->scale);
}

/* ¿Vale la pena fijarla en la instrucción i? (otra más adelante la usaría) */
static bool pc_worth(const Block *b, unsigned i)
{
    const Insn *a = &b->in[i];
    for (unsigned j = i + 1; j < b->n && j < i + 24; j++) {
        const Insn *x = &b->in[j];
        if (pc_cand_insn(x) && pc_same(a, x)) {
            int32_t d = (int32_t)(x->disp - a->disp);
            if (d > -4000 && d < 4000) {
                return true;
            }
        }
        if ((a->base != NOREG && pc_writes(x, a->base)) ||
            (a->index != NOREG && pc_writes(x, a->index))) {
            return false;
        }
    }
    return false;
}

/* Antes de la consulta de la página de w9: si la de antes sirve, x10/x11
 * salen de x27/w28 y se salta la consulta (devuelve ese salto). */
static uint32_t *pc_pre(Em *e, bool write, unsigned n, bool *est)
{
    *est = false;
    if (!e->pc_cand || e->ea_mark != e->p || e->ea_in != e->cur_in) {
        return NULL;
    }
    e->pc_cand = false;
    const Insn *in = e->cur_in;
    if (e->pc_live && in->base == e->pc_base && in->index == e->pc_index &&
        (in->index == NOREG || in->scale == e->pc_scale) && (!write || e->pc_wr)) {
        int32_t d = (int32_t)in->disp - e->pc_d0;
        if (d > -4096 && d < 4096) {
            if (d >= 0) {
                addwi(e, W11, 28, (uint32_t)d);
            } else {
                subwi(e, W11, 28, (uint32_t)-d);
            }
            cmpwi(e, W11, 0x1000u - n);
            uint32_t *miss = fwd_bcond(e, COND_HI);
            movx_r(e, W10, 27);
            uint32_t *hit = fwd_b(e);
            patch(e, miss);
            return hit;
        }
    }
    *est = pc_worth(e->cur_b, e->cur_i);
    return NULL;
}

static void pc_post(Em *e, uint32_t *hit, bool est, bool write)
{
    if (est) {
        movx_r(e, 27, W10);
        mov_w(e, 28, W11);
        const Insn *in = e->cur_in;
        e->pc_live = true;
        e->pc_wr = write;
        e->pc_base = in->base;
        e->pc_index = in->index;
        e->pc_scale = in->scale;
        e->pc_d0 = (int32_t)in->disp;
        e->pc_est[(e->cur_i >> 6) & 3] |= 1ull << (e->cur_i & 63);
    }
    patch(e, hit);
}
/* lea: al registro del x86 (sin segmento) */
static void emit_ea_to(Em *e, const Insn *in, int dst) { emit_ea_at(e, in, false, dst); }

/* Página de w9 (4 bytes enteros dentro): x10 = memoria del host, x11 = offset.
 * Si no está (o es de código y se va a escribir) salta al camino lento. */
static void emit_page(Em *e, bool write, uint32_t **slow1, uint32_t **slow2, uint32_t **slow3)
{
    bool est;
    uint32_t *hit = pc_pre(e, write, 4, &est);
    lsrw(e, W10, W9, 12);
    ldrw_lsl2(e, W10, XPT, W10);
    ubfx12(e, W11, W9);
    cmpwi(e, W11, 0xFFC);
    *slow2 = fwd_bcond(e, COND_HI);
    *slow1 = page_miss(e); /* x10 = página + 1: x20 y x22 ya restan 1 */
    *slow3 = NULL;
    if (write) { /* página con código traducido: lo hace la función (vm_touch) */
        ldrb_r(e, W13, XCODE, W10);
        *slow3 = fwd_cb(e, J_CBNZ, W13);
    }
    addx_lsl(e, W10, XRAM, W10, 12);
    pc_post(e, hit, est, write);
}

/* Llamar a la función de siempre de la instrucción i (con sus comprobaciones). */
/* La cabecera antes de las copias de las Insn de un bloque (para la rutina común). */
typedef struct JHdr {
    uint32_t ppage, gen, pad[2];
} JHdr;

/* Lo que hace la rutina común (en C): la función de la instrucción i con
 * icount al día, y si hay que salir, k + 1 (k: las instrucciones hechas). */
static uint32_t jit_slow_c(Cpu *c, const Insn *in, uint32_t iw, uint64_t x24)
{
    uint32_t i = iw & 0xFFFFu;
    if (iw >> 31) {
        c->st_cfb++;
    } else {
        c->st_ccall++;
    }
    uint64_t before = x24 + i;
    c->icount += before;
    c->eip = in->next;
    in->fn(c, in);
    c->icount -= before;
    if (c->stop != CPU_RUNNING) {
        c->jit_stop_eip = in->start;
        return i + 1;
    }
    if ((ends_block(in->fn) || in->fn == x_interp) && c->eip != in->next) {
        return i + 2; /* saltó: sale tras ella */
    }
    const JHdr *h = (const JHdr *)((const uint8_t *)(in - i) - sizeof(JHdr));
    if (c->vm->gen[h->ppage] != h->gen) {
        return i + 2; /* escribió en el código de este bloque */
    }
    return 0;
}

static uintptr_t em_rx(const Em *e) { return (uintptr_t)g_jit.rx + (uintptr_t)((uint8_t *)e->p - g_jit.rw); }

/* La rutina común: guarda los registros, llama a jit_slow_c(c, x1, w9, x24),
 * los vuelve a cargar y regresa con w0 (se pone una vez al principio del búfer). */
static void emit_slow_stub(Em *e)
{
    E(e, 0xA9BF7BFDu); /* stp x29, x30, [sp, #-16]! */
    regs_spill(e);
    movx_r(e, X0, XC);
    E(e, 0x2A0903E2u); /* mov w2, w9 */
    E(e, 0xAA1803E3u); /* mov x3, x24 */
    movx(e, W9, (uint64_t)(uintptr_t)jit_slow_c);
    E(e, 0xD63F0000u | 9u << 5); /* blr x9 */
    regs_load(e);
    E(e, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
    E(e, 0xD65F03C0u); /* ret */
}

/* La instrucción i por su función de C (camino lento). Etapa 5: antes eran
 * ~40 instrucciones de ARM cada vez; ahora 5 y la rutina común. */
static void emit_call(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    (void)vm;
    if (!e->fallback) {
        ((Block *)b)->slow |= 1ull << (i & 63); /* para el perfil (no el camino de respaldo) */
    }
    /* adr x1, la copia de la Insn (junto al código) */
    /* v0.18.80: los xmm cambiados, a memoria (en el camino lento: los de
     * antes de la instrucción; su propia escritura viene después del salto) */
    if (e->fallback) {
        xc_store(e, e->xc_cd);
    } else {
        xc_flush(e);
    }
    intptr_t d = (intptr_t)(uintptr_t)(e->insn_rx + i) - (intptr_t)em_rx(e);
    E(e, 0x10000000u | ((uint32_t)d & 3u) << 29 | (((uint32_t)(d >> 2)) & 0x7FFFFu) << 5 | 1u);
    movw(e, W9, i | (e->fallback ? 0x80000000u : 0u));
    intptr_t bl = ((intptr_t)g_jit.slow_rx - (intptr_t)em_rx(e)) >> 2;
    E(e, 0x94000000u | ((uint32_t)bl & 0x3FFFFFFu)); /* bl la rutina común */
    uint32_t *go = fwd_cb(e, J_CBZ, X0);
    E(e, 0x51000400u); /* sub w0, w0, #1: las hechas */
    if (e->nexits < sizeof(e->exits) / sizeof(e->exits[0])) {
        e->exits[e->nexits++] = fwd_b(e);
    } else {
        e->full = true;
    }
    patch(e, go);
    if (e->fallback) {
        xc_load(e, e->xc_cl); /* lo que el camino rápido espera tener */
    } else {
        e->xl = 0;
    }
    if (e->fallback && (e->pc_est[(i >> 6) & 3] >> (i & 63) & 1)) {
        movw(e, 28, PC_INVALID); /* v0.18.80: no fijó la página: que nadie la use */
    }
}

/* lf_* de una operación de 32 bits: w9 = a, w10 = b, w11 = res */
_Static_assert(offsetof(Cpu, lf_op) % 4 == 0 && offsetof(Cpu, lf_sz) == offsetof(Cpu, lf_op) + 1 &&
                   offsetof(Cpu, lf_cin) == offsetof(Cpu, lf_op) + 2,
               "lf_op, lf_sz, lf_cin: una palabra");

static void emit_lf(Em *e, unsigned kind)
{
    e->lf_kind = kind;
    if (e->lf_dead) {
        return;
    }
    movw(e, W12, kind | 4u << 8); /* lf_op, lf_sz = 4, lf_cin = 0 de una vez */
    strw(e, W12, XC, OFF(lf_op));
    if (kind != LF_LOGIC) { /* v0.18.66: las lógicas solo usan el resultado (flags_sync, cond) */
        strw(e, W9, XC, OFF(lf_a));
        strw(e, W10, XC, OFF(lf_b));
    }
    strw(e, W11, XC, OFF(lf_res));
}

/* w11 = w9 op w10 y sus flags; devuelve false si la operación no va aquí */
static bool emit_alu_op(Em *e, unsigned aop)
{
    unsigned kind;
    switch (aop) {
    case OP_ADD: rrr(e, ADDW, W11, W9, W10); kind = LF_ADD; break;
    case OP_SUB:
    case OP_CMP: rrr(e, SUBW, W11, W9, W10); kind = LF_SUB; break;
    case OP_AND: rrr(e, ANDW, W11, W9, W10); kind = LF_LOGIC; break;
    case OP_OR: rrr(e, ORRW, W11, W9, W10); kind = LF_LOGIC; break;
    case OP_XOR: rrr(e, EORW, W11, W9, W10); kind = LF_LOGIC; break;
    default: return false;
    }
    emit_lf(e, kind);
    return true;
}

static bool alu_native(unsigned aop) { return aop != OP_ADC && aop != OP_SBB; }

/* El camino lento de la instrucción i (sus saltos en sites; los NULL no
 * cuentan): al final del bloque, y de ahí vuelve aquí. Si no cabe, en línea. */
/* v0.18.80: tras el camino lento de un comiss, NZCV como los dejaría fcmp
 * (de ZF PF CF en c->flags): mayor 0010, menor 1000, igual 0110, sin orden 0011 */
static void fz_rebuild(Em *e)
{
    ldrw(e, W9, XC, OFF(flags));
    E(e, 0x1200012Au); /* and w10, w9, #1 (CF) */
    E(e, 0x5306192Bu); /* ubfx w11, w9, #6, #1 (ZF) */
    E(e, 0x5302092Cu); /* ubfx w12, w9, #2, #1 (PF) */
    E(e, 0x2A0B054Au); /* orr w10, w10, w11, lsl #1 */
    E(e, 0x2A0C094Au); /* orr w10, w10, w12, lsl #2 */
    E(e, 0x531E754Au); /* lsl w10, w10, #2 */
    movw(e, W11, 0x30000682u); /* NZCV por índice, de a 4 bits */
    E(e, 0x1ACA256Bu); /* lsr w11, w11, w10 */
    E(e, 0x53040D6Bu); /* lsl w11, w11, #28 */
    E(e, 0xD51B420Bu); /* msr nzcv, x11 */
}

static void xc_sites(Em *e, uint32_t *const *sites, unsigned n)
{
    for (unsigned k = 0; k < n; k++) {
        if (sites[k] && e->xw_first && sites[k] > e->xw_first) {
            e->xc_bad = true; /* el lento repetiría la instrucción con el xmm ya cambiado */
        }
    }
}

static void slow_cold(Em *e, const Block *b, unsigned i, const Vm *vm, uint32_t *const *sites, unsigned n)
{
    xc_sites(e, sites, n);
    unsigned k0 = e->npool;
    if (e->ncold < sizeof(e->cold) / sizeof(e->cold[0]) && e->npool + n <= sizeof(e->pool) / sizeof(e->pool[0])) {
        for (unsigned k = 0; k < n; k++) {
            if (sites[k]) {
                e->pool[e->npool++] = sites[k];
            }
        }
        e->cold[e->ncold].back = e->p;
        e->cold[e->ncold].first = (uint16_t)k0;
        e->cold[e->ncold].n = (uint16_t)(e->npool - k0);
        e->cold[e->ncold].i = (uint16_t)i;
        e->cold[e->ncold].xd = e->xd_before;
        e->cold[e->ncold].xl = e->xl;
        e->cold[e->ncold].fz = e->fz;
        e->ncold++;
        return;
    }
    uint32_t *done = fwd_b(e);
    for (unsigned k = 0; k < n; k++) {
        patch(e, sites[k]);
    }
    e->xc_cd = e->xd_before;
    e->xc_cl = e->xl;
    e->fallback = true;
    emit_call(e, b, i, vm);
    e->fallback = false;
    if (e->fz) {
        fz_rebuild(e);
    }
    patch(e, done);
}

/* Los caminos lentos guardados: cada uno llama a la función de siempre y
 * vuelve a donde estaba. */
static void emit_colds(Em *e, const Block *b, const Vm *vm)
{
    for (unsigned c = 0; c < e->ncold; c++) {
        for (unsigned k = 0; k < e->cold[c].n; k++) {
            patch(e, e->pool[e->cold[c].first + k]);
        }
        e->xc_cd = e->cold[c].xd;
        e->xc_cl = e->cold[c].xl;
        e->fallback = true;
        emit_call(e, b, e->cold[c].i, vm);
        e->fallback = false;
        if (e->cold[c].fz) {
            fz_rebuild(e);
        }
        E(e, 0x14000000u | ((uint32_t)(e->cold[c].back - e->p) & 0x3FFFFFFu)); /* b de vuelta */
    }
    e->ncold = e->npool = 0;
}

static void then_mov_ge(Em *e, const Insn *in);
/* Una carga de 32 bits de [ea] a w12, o la función de siempre. done: tras ambas. */
static void emit_load_or_call(Em *e, const Block *b, unsigned i, const Vm *vm, void (*then)(Em *, const Insn *))
{
    const Insn *in = &b->in[i];
    uint32_t *s1, *s2, *s3;
    emit_ea(e, in, true);
    emit_page(e, false, &s1, &s2, &s3);
    if (then == then_mov_ge) { /* v0.18.61: directo al registro del x86 */
        ldrw_r(e, k_rmap[in->reg], W10, W11);
    } else {
        ldrw_r(e, W12, W10, W11);
        then(e, in);
    }
    uint32_t *sites[2] = {s1, s2};
    (void)s3;
    slow_cold(e, b, i, vm, sites, 2);
}

static void then_mov_ge(Em *e, const Insn *in) { strw(e, W12, XC, ROFF(in->reg)); }

static void then_alu_ge(Em *e, const Insn *in)
{
    ldrw(e, W9, XC, ROFF(in->reg));
    E(e, 0x2A0003E0u | (uint32_t)W12 << 16 | (uint32_t)W10); /* mov w10, w12 */
    emit_alu_op(e, in->aop);
    if (in->aop != OP_CMP) {
        strw(e, W11, XC, ROFF(in->reg));
    }
}

/* Guardar w<src> en [w9] o la función de siempre. */
static void emit_store_src(Em *e, const Block *b, unsigned i, const Vm *vm, int src)
{
    uint32_t *s1, *s2, *s3;
    emit_page(e, true, &s1, &s2, &s3);
    strw_r(e, src, W10, W11);
    uint32_t *sites[3] = {s1, s2, s3};
    slow_cold(e, b, i, vm, sites, 3);
}
static void emit_store_or_call(Em *e, const Block *b, unsigned i, const Vm *vm) { emit_store_src(e, b, i, vm, W12); }

/* ---- MMX en NEON (2d.3): los registros mm viven en c->fp.mm; v0 = destino,
 * v1 = fuente, v2 de paso. Halo 2 usa MMX sin parar. ---- */

#define MMOFF(n) (OFF(fp.mm) + 8u * (uint32_t)(n))

/* ---- v0.18.80: los xmm en registros (v16 + k) mientras corre el bloque ----
 * Las lecturas y escrituras de c->fp.xmm que piden los emisores (x19 +
 * XO + ...) se vuelven movimientos entre registros de NEON. Se guardan los
 * cambiados antes de cada llamada a C y al salir; tras C se vuelven a leer. */
#define XO OFF(fp.xmm)
#define XV(k) (16 + (k))
static void raw_ldrq(Em *e, int vt, int rn, uint32_t off) { E(e, 0x3DC00000u | ((off / 16) << 10) | (uint32_t)rn << 5 | (uint32_t)vt); }
static void raw_strq(Em *e, int vt, int rn, uint32_t off) { E(e, 0x3D800000u | ((off / 16) << 10) | (uint32_t)rn << 5 | (uint32_t)vt); }
static void vmov16(Em *e, int d, int n) { E(e, 0x4EA01C00u | (uint32_t)n << 16 | (uint32_t)n << 5 | (uint32_t)d); } /* orr vd.16b, vn, vn */
static void xc_store(Em *e, unsigned m)
{
    for (unsigned k = 0; k < 8; k++) {
        if (m >> k & 1) {
            raw_strq(e, XV(k), 19, XO + 16u * k);
        }
    }
}
static void xc_load(Em *e, unsigned m)
{
    for (unsigned k = 0; k < 8; k++) {
        if (m >> k & 1) {
            raw_ldrq(e, XV(k), 19, XO + 16u * k);
        }
    }
}
/* guardar los cambiados (siguen cargados) */
static void xc_flush(Em *e)
{
    xc_store(e, e->xd);
    e->xd = 0;
}
/* tras C: lo de c->fp.xmm puede ser otro */
static void xc_drop(Em *e)
{
    xc_flush(e);
    e->xl = 0;
}
static void xc_need(Em *e, unsigned k)
{
    if (!(e->xl >> k & 1)) {
        if (e->xc_locked) {
            e->xc_bad = true; /* una carga a mitad de la instrucción: sin caché */
        }
        raw_ldrq(e, XV(k), 19, XO + 16u * k);
        e->xl |= (uint8_t)(1u << k);
    }
}
static void xc_wrote(Em *e, unsigned k)
{
    e->xl |= (uint8_t)(1u << k);
    e->xd |= (uint8_t)(1u << k);
    if (!e->xw_first) {
        e->xw_first = e->p - 1;
    }
}
static bool xc_at(const Em *e, int rn, uint32_t off, unsigned *k, unsigned *sub)
{
    if (!e->xc_on || rn != 19 || off < XO || off >= XO + 128u) {
        return false;
    }
    *k = (off - XO) / 16u;
    *sub = (off - XO) % 16u;
    return true;
}
/* blr x9 a C dentro del código de una instrucción: los cambiados a memoria
 * antes y todos los cargados de vuelta después (en los dos caminos queda igual) */
static void xc_blr(Em *e)
{
    xc_store(e, e->xd);
    E(e, 0xD63F0000u | 9u << 5); /* blr x9 */
    xc_load(e, e->xl);
}
static void ldrq_i(Em *e, int vt, int rn, uint32_t off)
{
    unsigned k, sub;
    if (xc_at(e, rn, off, &k, &sub)) {
        xc_need(e, k);
        vmov16(e, vt, XV(k));
        return;
    }
    raw_ldrq(e, vt, rn, off);
}
static void strq_i(Em *e, int vt, int rn, uint32_t off)
{
    unsigned k, sub;
    if (xc_at(e, rn, off, &k, &sub)) {
        vmov16(e, XV(k), vt);
        xc_wrote(e, k);
        return;
    }
    raw_strq(e, vt, rn, off);
}
static void ldrs_i(Em *e, int vt, int rn, uint32_t off)
{
    unsigned k, sub;
    if (xc_at(e, rn, off, &k, &sub)) {
        xc_need(e, k);
        if (sub == 0) {
            E(e, 0x1E204000u | (uint32_t)XV(k) << 5 | (uint32_t)vt); /* fmov st, sk */
        } else {
            E(e, 0x5E000400u | ((sub / 4u) << 3 | 4u) << 16 | (uint32_t)XV(k) << 5 | (uint32_t)vt); /* mov st, vk.s[i] */
        }
        return;
    }
    E(e, 0xBD400000u | ((off / 4) << 10) | (uint32_t)rn << 5 | (uint32_t)vt);
}
static void strs_i(Em *e, int vt, int rn, uint32_t off)
{
    unsigned k, sub;
    if (xc_at(e, rn, off, &k, &sub)) {
        xc_need(e, k);
        E(e, 0x6E000400u | ((sub / 4u) << 3 | 4u) << 16 | (uint32_t)vt << 5 | (uint32_t)XV(k)); /* mov vk.s[i], vt.s[0] */
        xc_wrote(e, k);
        return;
    }
    E(e, 0xBD000000u | ((off / 4) << 10) | (uint32_t)rn << 5 | (uint32_t)vt);
}
static void ldrd(Em *e, int vt, int rn, uint32_t off)
{
    unsigned k, sub;
    if (xc_at(e, rn, off, &k, &sub) && (sub & 7) == 0) {
        xc_need(e, k);
        if (sub == 0) {
            E(e, 0x1E604000u | (uint32_t)XV(k) << 5 | (uint32_t)vt); /* fmov dt, dk */
        } else {
            E(e, 0x5E000400u | 0x18u << 16 | (uint32_t)XV(k) << 5 | (uint32_t)vt); /* mov dt, vk.d[1] */
        }
        return;
    }
    if (xc_at(e, rn, off, &k, &sub)) {
        e->xc_bad = true;
    }
    E(e, 0xFD400000u | ((off / 8) << 10) | (uint32_t)rn << 5 | (uint32_t)vt);
}
static void strd(Em *e, int vt, int rn, uint32_t off)
{
    unsigned k, sub;
    if (xc_at(e, rn, off, &k, &sub) && (sub & 7) == 0) {
        xc_need(e, k);
        E(e, 0x6E000400u | ((sub / 8u) << 4 | 8u) << 16 | (uint32_t)vt << 5 | (uint32_t)XV(k)); /* mov vk.d[i], vt.d[0] */
        xc_wrote(e, k);
        return;
    }
    if (xc_at(e, rn, off, &k, &sub)) {
        e->xc_bad = true;
    }
    E(e, 0xFD000000u | ((off / 8) << 10) | (uint32_t)rn << 5 | (uint32_t)vt);
}
static void ldrd_r(Em *e, int vt, int rn, int rm) { E(e, 0xFC606800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)vt); }
static void strd_r(Em *e, int vt, int rn, int rm) { E(e, 0xFC206800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)vt); }

/* Como emit_page, para 8 bytes. */
static void emit_page8(Em *e, bool write, uint32_t **slow1, uint32_t **slow2, uint32_t **slow3)
{
    bool est;
    uint32_t *hit = pc_pre(e, write, 8, &est);
    lsrw(e, W10, W9, 12);
    ldrw_lsl2(e, W10, XPT, W10);
    ubfx12(e, W11, W9);
    cmpwi(e, W11, 0xFF8);
    *slow2 = fwd_bcond(e, COND_HI);
    *slow1 = page_miss(e); /* x10 = página + 1: x20 y x22 ya restan 1 */
    *slow3 = NULL;
    if (write) {
        ldrb_r(e, W13, XCODE, W10);
        *slow3 = fwd_cb(e, J_CBNZ, W13);
    }
    addx_lsl(e, W10, XRAM, W10, 12);
    pc_post(e, hit, est, write);
}

/* La operación NEON de 64 bits sobre v0 (y v1); false si no la hay. */
static bool neon_op(Em *e, InsnFn f, const Insn *in)
{
#define V3(U, size, opc) (0x0E200400u | (uint32_t)(U) << 29 | (uint32_t)(size) << 22 | 1u << 16 | (uint32_t)(opc) << 11)
    uint32_t op = 0;
    if (f == x_paddb) op = V3(0, 0, 0x10);
    else if (f == x_paddw) op = V3(0, 1, 0x10);
    else if (f == x_paddd) op = V3(0, 2, 0x10);
    else if (f == x_psubb) op = V3(1, 0, 0x10);
    else if (f == x_psubw) op = V3(1, 1, 0x10);
    else if (f == x_psubd) op = V3(1, 2, 0x10);
    else if (f == x_paddsb) op = V3(0, 0, 0x01);
    else if (f == x_paddsw) op = V3(0, 1, 0x01);
    else if (f == x_psubsb) op = V3(0, 0, 0x05);
    else if (f == x_psubsw) op = V3(0, 1, 0x05);
    else if (f == x_paddusb) op = V3(1, 0, 0x01);
    else if (f == x_paddusw) op = V3(1, 1, 0x01);
    else if (f == x_psubusb) op = V3(1, 0, 0x05);
    else if (f == x_psubusw) op = V3(1, 1, 0x05);
    else if (f == x_pmullw) op = V3(0, 1, 0x13);
    else if (f == x_pcmpeqb) op = V3(1, 0, 0x11);
    else if (f == x_pcmpeqw) op = V3(1, 1, 0x11);
    else if (f == x_pcmpeqd) op = V3(1, 2, 0x11);
    else if (f == x_pcmpgtb) op = V3(0, 0, 0x06);
    else if (f == x_pcmpgtw) op = V3(0, 1, 0x06);
    else if (f == x_pcmpgtd) op = V3(0, 2, 0x06);
    else if (f == x_pminub) op = V3(1, 0, 0x0D);
    else if (f == x_pmaxub) op = V3(1, 0, 0x0C);
    else if (f == x_pminsw) op = V3(0, 1, 0x0D);
    else if (f == x_pmaxsw) op = V3(0, 1, 0x0C);
    else if (f == x_pavgb) op = V3(1, 0, 0x02);
    else if (f == x_pavgw) op = V3(1, 1, 0x02);
#undef V3
    if (op) {
        /* el campo Rm de V3 era 1 (v1); Rn = v0, Rd = v0 */
        E(e, op);
        return true;
    }
    if (f == x_mmx_logic) {
        switch (in->aop) {
        case 0xDB: E(e, 0x0E201C00u | 1u << 16); return true;           /* and v0, v0, v1 */
        case 0xDF: E(e, 0x0E601C00u | 0u << 16 | 1u << 5); return true; /* bic v0, v1, v0: ~a & b */
        case 0xEB: E(e, 0x0EA01C00u | 1u << 16); return true;           /* orr */
        default: E(e, 0x2E201C00u | 1u << 16); return true;             /* eor */
        }
    }
    if (f == x_pmulhw || f == x_pmulhuw) {
        E(e, (f == x_pmulhw ? 0x0E60C000u : 0x2E60C000u) | 1u << 16 | 0u << 5 | 2u); /* s/umull v2.4s, v0.4h, v1.4h */
        E(e, 0x0F108400u | 2u << 5 | 0u);                                         /* shrn v0.4h, v2.4s, #16 */
        return true;
    }
    if (f == x_pmaddwd) {
        E(e, 0x0E60C000u | 1u << 16 | 0u << 5 | 2u); /* smull v2.4s, v0.4h, v1.4h */
        E(e, 0x4EA0BC00u | 2u << 16 | 2u << 5 | 0u); /* addp v0.4s, v2.4s, v2.4s */
        return true;
    }
    if (f == x_punpck) {
        uint32_t size = (in->aop & 3) == 0 ? 0 : (in->aop & 3) == 1 ? 1 : 2;
        E(e, (in->aop >= 0x68 ? 0x0E007800u : 0x0E003800u) | size << 22 | 1u << 16); /* zip2 / zip1 v0, v0, v1 */
        return true;
    }
    if (f == x_pack) {
        E(e, 0x6E180400u | 1u << 5 | 0u); /* ins v0.d[1], v1.d[0] */
        if (in->aop == 0x63) {
            E(e, 0x0E214800u);            /* sqxtn v0.8b, v0.8h */
        } else if (in->aop == 0x67) {
            E(e, 0x2E212800u);            /* sqxtun v0.8b, v0.8h */
        } else {
            E(e, 0x0E614800u);            /* sqxtn v0.4h, v0.4s */
        }
        return true;
    }
    return false;
}

static bool is_mmx_op(InsnFn f)
{
    return f == x_paddb || f == x_paddw || f == x_paddd || f == x_psubb || f == x_psubw || f == x_psubd ||
           f == x_paddsb || f == x_paddsw || f == x_psubsb || f == x_psubsw || f == x_paddusb || f == x_paddusw ||
           f == x_psubusb || f == x_psubusw || f == x_pmullw || f == x_pmulhw || f == x_pmulhuw || f == x_pcmpeqb ||
           f == x_pcmpeqw || f == x_pcmpeqd || f == x_pcmpgtb || f == x_pcmpgtw || f == x_pcmpgtd || f == x_pminub ||
           f == x_pmaxub || f == x_pminsw || f == x_pmaxsw || f == x_pavgb || f == x_pavgw || f == x_mmx_logic ||
           f == x_pmaddwd || f == x_punpck || f == x_pack;
}

/* La fuente de 64 bits en v1 (registro mm o memoria); con memoria deja en
 * *slow los saltos al camino de respaldo. */
static void mmx_src(Em *e, const Insn *in, uint32_t **s1, uint32_t **s2)
{
    *s1 = *s2 = NULL;
    if (in->is_reg) {
        ldrd(e, 1, XC, MMOFF(in->rm));
        return;
    }
    uint32_t *s3;
    emit_ea(e, in, true);
    emit_page8(e, false, s1, s2, &s3);
    ldrd_r(e, 1, W10, W11);
}

static void mmx_tail(Em *e, const Block *b, unsigned i, const Vm *vm, uint32_t *s1, uint32_t *s2)
{
    if (!s1) {
        return;
    }
    uint32_t *sites[2] = {s1, s2};
    slow_cold(e, b, i, vm, sites, 2);
}

static bool emit_mmx(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    InsnFn f = in->fn;
    uint32_t *s1, *s2, *s3;
    if (is_mmx_op(f)) {
        mmx_src(e, in, &s1, &s2);
        ldrd(e, 0, XC, MMOFF(in->reg));
        neon_op(e, f, in);
        strd(e, 0, XC, MMOFF(in->reg));
        mmx_tail(e, b, i, vm, s1, s2);
        return true;
    }
    if (f == x_movq_load) {
        mmx_src(e, in, &s1, &s2);
        strd(e, 1, XC, MMOFF(in->reg));
        movw(e, W12, 1);
        strb(e, W12, XC, OFF(fp.mmx));
        mmx_tail(e, b, i, vm, s1, s2);
        return true;
    }
    if (f == x_movq_store) {
        ldrd(e, 0, XC, MMOFF(in->reg));
        if (in->is_reg) {
            strd(e, 0, XC, MMOFF(in->rm));
            return true;
        }
        emit_ea(e, in, true);
        emit_page8(e, true, &s1, &s2, &s3);
        strd_r(e, 0, W10, W11);
        uint32_t *sites[3] = {s1, s2, s3};
        slow_cold(e, b, i, vm, sites, 3);
        return true;
    }
    if (f == x_movd_load) {
        if (in->is_reg) {
            ldrw(e, W12, XC, ROFF(in->rm));
            E(e, 0x1E270000u | (uint32_t)W12 << 5 | 0u); /* fmov s0, w12 (el resto en 0) */
            strd(e, 0, XC, MMOFF(in->reg));
            s1 = s2 = NULL;
        } else {
            emit_ea(e, in, true);
            emit_page(e, false, &s1, &s2, &s3);
            ldrw_r(e, W12, W10, W11);
            E(e, 0x1E270000u | (uint32_t)W12 << 5 | 0u);
            strd(e, 0, XC, MMOFF(in->reg));
        }
        movw(e, W12, 1);
        strb(e, W12, XC, OFF(fp.mmx));
        mmx_tail(e, b, i, vm, s1, s2);
        return true;
    }
    if (f == x_movd_store) {
        ldrw(e, W12, XC, MMOFF(in->reg)); /* la mitad baja */
        if (in->is_reg) {
            strw(e, W12, XC, ROFF(in->rm));
            return true;
        }
        emit_ea(e, in, true);
        emit_store_or_call(e, b, i, vm);
        return true;
    }
    if (f == x_mmx_shift_i) {
        unsigned bits = in->sz * 8u, cnt = in->imm, kind = in->reg;
        ldrd(e, 0, XC, MMOFF(in->rm));
        if (bits == 64) {
            if (cnt >= 64) {
                E(e, 0x2F00E400u); /* movi d0, #0 */
            } else if (cnt) {
                if (kind == 2) {
                    E(e, 0x7F000400u | (128u - cnt) << 16); /* ushr d0, d0, #cnt */
                } else {
                    E(e, 0x5F005400u | (64u + cnt) << 16);  /* shl d0, d0, #cnt */
                }
            }
        } else if (cnt >= bits && kind != 4) {
            E(e, 0x2F00E400u);
        } else {
            unsigned c2 = cnt >= bits ? bits - 1 : cnt;
            if (c2) {
                if (kind == 6) {
                    E(e, 0x0F005400u | (bits + c2) << 16);      /* shl v0.4h/2s, v0, #c2 */
                } else {
                    E(e, (kind == 4 ? 0x0F000400u : 0x2F000400u) | (2 * bits - c2) << 16); /* sshr / ushr */
                }
            }
        }
        strd(e, 0, XC, MMOFF(in->rm));
        return true;
    }
    return false;
}

/* ---- 2d.3: más instrucciones en código nativo ---- */

static void ldrb_i(Em *e, int rt, int rn, uint32_t off)
{
    if (reg_off(rn, off)) { /* ubfx wt, wR, #8*(off%4), #8 */
        uint32_t lsb = 8u * (off & 3u);
        E(e, 0x53000000u | lsb << 16 | (lsb + 7u) << 10 | (uint32_t)k_rmap[off / 4] << 5 | (uint32_t)rt);
        return;
    }
    E(e, 0x39400000u | (off << 10) | (uint32_t)rn << 5 | (uint32_t)rt);
}
static void ldrh_i(Em *e, int rt, int rn, uint32_t off)
{
    if (reg_off(rn, off)) { /* uxth */
        E(e, 0x53000000u | 0u << 16 | 15u << 10 | (uint32_t)k_rmap[off / 4] << 5 | (uint32_t)rt);
        return;
    }
    E(e, 0x79400000u | ((off / 2) << 10) | (uint32_t)rn << 5 | (uint32_t)rt);
}
static void strh_i(Em *e, int rt, int rn, uint32_t off)
{
    if (reg_off(rn, off)) { /* bfi wR, wt, #0, #16 */
        E(e, 0x33000000u | 0u << 16 | 15u << 10 | (uint32_t)rt << 5 | k_rmap[off / 4]);
        return;
    }
    E(e, 0x79000000u | ((off / 2) << 10) | (uint32_t)rn << 5 | (uint32_t)rt);
}
static void ldrh_r(Em *e, int rt, int rn, int rm) { E(e, 0x78606800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt); }
static void strb_r(Em *e, int rt, int rn, int rm) { E(e, 0x38206800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt); }
static void strh_r(Em *e, int rt, int rn, int rm) { E(e, 0x78206800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt); }

/* Como emit_page, para un acceso de n bytes. */
static void emit_page_n(Em *e, bool write, unsigned n, uint32_t **slow1, uint32_t **slow2, uint32_t **slow3)
{
    bool est;
    uint32_t *hit = pc_pre(e, write, n, &est);
    lsrw(e, W10, W9, 12);
    ldrw_lsl2(e, W10, XPT, W10);
    ubfx12(e, W11, W9);
    *slow2 = NULL;
    if (n > 1) { /* v0.18.71: un byte nunca cruza de página */
        cmpwi(e, W11, 0x1000 - n);
        *slow2 = fwd_bcond(e, COND_HI);
    }
    *slow1 = page_miss(e); /* x10 = página + 1: x20 y x22 ya restan 1 */
    *slow3 = NULL;
    if (write) {
        ldrb_r(e, W13, XCODE, W10);
        *slow3 = fwd_cb(e, J_CBNZ, W13);
    }
    addx_lsl(e, W10, XRAM, W10, 12);
    pc_post(e, hit, est, write);
}

/* Dónde está un registro de 8 bits (AL..BH) dentro de c->r. */
static uint32_t r8off(unsigned r) { return r < 4 ? ROFF(r) : ROFF(r - 4) + 1; }

static void fallback_tail(Em *e, const Block *b, unsigned i, const Vm *vm, uint32_t *done, uint32_t *s1,
                          uint32_t *s2, uint32_t *s3)
{
    if (!e->full && done == e->p - 1) { /* el "b done" que se acaba de poner: no hace falta */
        e->p--;
        uint32_t *sites[3] = {s1, s2, s3};
        slow_cold(e, b, i, vm, sites, 3);
        return;
    }
    uint32_t *sites[3] = {s1, s2, s3};
    xc_sites(e, sites, 3);
    patch(e, s1);
    patch(e, s2);
    if (s3) {
        patch(e, s3);
    }
    e->xc_cd = e->xd_before;
    e->xc_cl = e->xl;
    e->fallback = true;
    emit_call(e, b, i, vm);
    e->fallback = false;
    if (e->fz) {
        fz_rebuild(e);
    }
    patch(e, done);
}

/* jcc tras cmp/sub/test/and/or/xor de 32 bits hechos aquí: los flags de
 * ARM64 dicen lo mismo (con el carry al revés). -1: no se puede. */
static int arm_cond(unsigned kind, unsigned x86)
{
    static const int sub[16] = {6, 7, 3, 2, 0, 1, 9, 8, 4, 5, -1, -1, 11, 10, 13, 12};
    if (kind == LF_LOGIC) {
        if (x86 == 2) return 16; /* nunca */
        if (x86 == 3) return 14; /* siempre */
        if (x86 == 6) return 0;  /* ZF */
        if (x86 == 7) return 1;
    }
    return sub[x86 & 15];
}

static bool emit_more(Em *e, const Block *b, unsigned i, const Vm *vm, unsigned prev_kind)
{
    const Insn *in = &b->in[i];
    InsnFn f = in->fn;
    uint32_t *s1, *s2, *s3;
    if (f == x_jcc && prev_kind == LFK_FCMP && e->nzcv_prev == LFK_FCMP) {
        /* v0.18.80: jcc tras comiss/ucomiss: NZCV de fcmp. OF = SF = 0 */
        static const int8_t fc[16] = {16, 14, 11, 10, -1, -1, 13, 12, 16, 14, 6, 7, 16, 14, -1, -1};
        int cc = fc[in->aop & 15];
        if (cc < 0) {
            return false;
        }
        if (i + 1 < b->n && in->imm == in->next) {
            e->jcc_none = true;
            return true;
        }
        if (i + 1 == b->n && g_link_on && cc != 14 && cc != 16) {
            e->fj = true;
            e->fj_cc = cc;
            e->fj_taken = in->imm;
            e->fj_next = in->next;
            return true;
        }
        if (i + 1 < b->n && cc != 14 && cc != 16) {
            e->jcc_site = fwd_bcond(e, cc);
            return true;
        }
        movw(e, W12, in->next);
        if (cc == 14) {
            movw(e, W12, in->imm);
        } else if (cc != 16) {
            movw(e, W13, in->imm);
            E(e, 0x1A800000u | (uint32_t)W12 << 16 | (uint32_t)cc << 12 | (uint32_t)W13 << 5 | (uint32_t)W12); /* csel */
        }
        strw(e, W12, XC, OFF(eip));
        return true;
    }
    if (f == x_jcc && (prev_kind == LF_SUB || prev_kind == LF_LOGIC)) {
        int cc = arm_cond(prev_kind, in->aop);
        if (cc < 0) {
            return false;
        }
        if (i + 1 < b->n && in->imm == in->next) {
            e->jcc_none = true; /* salta a la siguiente: no hace nada */
            return true;
        }
        if (i + 1 == b->n && g_link_on && cc != 14 && cc != 16) {
            /* v0.18.61: el último: dos salidas fijas (jit_compile) */
            if (e->nzcv_prev != prev_kind) {
                if (prev_kind == LF_SUB) {
                    ldrw(e, W9, XC, OFF(lf_a));
                    ldrw(e, W10, XC, OFF(lf_b));
                    cmpw(e, W9, W10);
                } else {
                    ldrw(e, W11, XC, OFF(lf_res));
                    E(e, 0x6A0B017Fu); /* tst w11, w11 */
                }
            }
            e->fj = true;
            e->fj_cc = cc;
            e->fj_taken = in->imm;
            e->fj_next = in->next;
            return true;
        }
        if (i + 1 < b->n && cc != 14 && cc != 16) {
            /* v0.18.61: a mitad del bloque, un b.cond a la salida (que pone
             * el EIP); si no salta, sigue sin tocar nada */
            if (e->nzcv_prev != prev_kind) {
                if (prev_kind == LF_SUB) {
                    ldrw(e, W9, XC, OFF(lf_a));
                    ldrw(e, W10, XC, OFF(lf_b));
                    cmpw(e, W9, W10);
                } else {
                    ldrw(e, W11, XC, OFF(lf_res));
                    E(e, 0x6A0B017Fu); /* tst w11, w11 */
                }
            }
            e->jcc_site = fwd_bcond(e, cc);
            return true;
        }
        movw(e, W12, in->next);
        if (cc == 14) {
            movw(e, W12, in->imm);
        } else if (cc != 16) {
            if (e->nzcv_prev == prev_kind) {
                /* los flags de ARM ya son los de la anterior */
            } else if (prev_kind == LF_SUB) {
                ldrw(e, W9, XC, OFF(lf_a));
                ldrw(e, W10, XC, OFF(lf_b));
                cmpw(e, W9, W10);
            } else {
                ldrw(e, W11, XC, OFF(lf_res));
                E(e, 0x6A0B017Fu); /* tst w11, w11 */
            }
            movw(e, W13, in->imm);
            E(e, 0x1A800000u | (uint32_t)W12 << 16 | (uint32_t)cc << 12 | (uint32_t)W13 << 5 | (uint32_t)W12); /* csel w12, w13, w12, cc */
        }
        strw(e, W12, XC, OFF(eip));
        return true;
    }
    if (f == x_movx && in->sz == 4 && (in->aop == 1 || in->aop == 2)) {
        bool sx = in->imm != 0;
        if (in->is_reg) {
            if (in->aop == 1) {
                ldrb_i(e, W12, XC, r8off(in->rm));
            } else {
                ldrh_i(e, W12, XC, ROFF(in->rm));
            }
            s1 = s2 = NULL;
        } else {
            emit_ea(e, in, true);
            emit_page_n(e, false, in->aop, &s1, &s2, &s3);
            if (in->aop == 1) {
                ldrb_r(e, W12, W10, W11);
            } else {
                ldrh_r(e, W12, W10, W11);
            }
        }
        if (sx) {
            E(e, (in->aop == 1 ? 0x13001C00u : 0x13003C00u) | (uint32_t)W12 << 5 | (uint32_t)W12); /* sxtb / sxth */
        }
        strw(e, W12, XC, ROFF(in->reg));
        if (s1) {
            uint32_t *done = fwd_b(e);
            fallback_tail(e, b, i, vm, done, s1, s2, NULL);
        }
        return true;
    }
    if ((f == x_mov_ge || f == x_mov_eg || f == x_mov_ei) && (in->sz == 1 || in->sz == 2)) {
        unsigned n = in->sz;
        uint32_t ro = n == 1 ? r8off(in->reg) : ROFF(in->reg);
        if (f == x_mov_ge) { /* registro <- E */
            if (in->is_reg) {
                if (n == 1) {
                    ldrb_i(e, W12, XC, r8off(in->rm));
                    strb(e, W12, XC, ro);
                } else {
                    ldrh_i(e, W12, XC, ROFF(in->rm));
                    strh_i(e, W12, XC, ro);
                }
                return true;
            }
            emit_ea(e, in, true);
            emit_page_n(e, false, n, &s1, &s2, &s3);
            if (n == 1) {
                ldrb_r(e, W12, W10, W11);
                strb(e, W12, XC, ro);
            } else {
                ldrh_r(e, W12, W10, W11);
                strh_i(e, W12, XC, ro);
            }
            uint32_t *done = fwd_b(e);
            fallback_tail(e, b, i, vm, done, s1, s2, NULL);
            return true;
        }
        /* E <- registro o inmediato */
        if (f == x_mov_eg) {
            if (n == 1) {
                ldrb_i(e, W12, XC, ro);
            } else {
                ldrh_i(e, W12, XC, ro);
            }
        } else {
            movw(e, W12, in->imm & (n == 1 ? 0xFFu : 0xFFFFu));
        }
        if (in->is_reg) {
            if (n == 1) {
                strb(e, W12, XC, r8off(in->rm));
            } else {
                strh_i(e, W12, XC, ROFF(in->rm));
            }
            return true;
        }
        emit_ea(e, in, true);
        emit_page_n(e, true, n, &s1, &s2, &s3);
        if (n == 1) {
            strb_r(e, W12, W10, W11);
        } else {
            strh_r(e, W12, W10, W11);
        }
        uint32_t *done = fwd_b(e);
        fallback_tail(e, b, i, vm, done, s1, s2, s3);
        return true;
    }
    return false;
}

#include "cpu_jit_fp.inc"

/* Una instrucción; true si puede quedar en código nativo (EIP sin poner;
 * su camino lento pone el mismo next). false: la función, que pone el EIP. */
/* v0.18.80: SSE/MMX (0F) y x87 que ni leen ni escriben EFLAGS: antes
 * cortaban la búsqueda y los flags de la instrucción de antes se guardaban
 * aunque nadie los leyera (en el código de Halo 2, mucho entre cuentas). */
static bool fp_flag_neutral(const Insn *in)
{
    if (in->fn != x_interp || !(in->sz & INTERP_MARK)) {
        return false;
    }
    unsigned op = in->aop;
    if (in->sz & INTERP_0F) {
        return (op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2D) || (op >= 0x50 && op <= 0x7F) ||
               op == 0xC2 || op == 0xC6 || (op >= 0xD0 && op <= 0xFE);
    }
    if (op < 0xD8 || op > 0xDF) {
        return false;
    }
    if (in->is_reg && (op == 0xDB || op == 0xDF) && (in->reg == 5 || in->reg == 6)) {
        return false; /* fcomi / fucomi (y con pop) */
    }
    if (in->is_reg && (op == 0xDA || op == 0xDB) && in->reg < 4) {
        return false; /* fcmov */
    }
    return true;
}

/* v0.18.61: no leen ni escriben los flags (ni en su camino lento) */
static bool lf_neutral(const Insn *in)
{
    InsnFn f = in->fn;
    return f == x_nop || f == x_lea || f == x_mov_ge || f == x_mov_eg || f == x_mov_ei || f == x_movx ||
           f == x_push_r || f == x_push_i || f == x_pop_r || is_mmx_op(f) || f == x_movq_load || f == x_movq_store ||
           f == x_movd_load || f == x_movd_store || f == x_mmx_shift_i || f == x_cdq || f == x_emms ||
           fp_flag_neutral(in);
}

/* pisa todos los flags sin leerlos (alu sin adc/sbb, test; de cualquier
 * tamaño, también con memoria: su camino lento los calcula de cero) */
static bool lf_writer_all(const Insn *in)
{
    InsnFn f = in->fn;
    if (f == x_alu_eg || f == x_alu_ge || f == x_alu_ei) {
        return in->aop != OP_ADC && in->aop != OP_SBB;
    }
    /* v0.18.80: imul (mul_flags) y shl/shr/sar con cuenta fija no nula */
    if (f == x_imul) {
        return true;
    }
    if (f == x_shift) {
        return in->aop >= 4 && in->imm != 0xFF && (in->imm & 31) != 0;
    }
    if (f == x_interp && (in->sz & (INTERP_MARK | INTERP_0F)) == (INTERP_MARK | INTERP_0F) &&
        (in->aop == 0x2E || in->aop == 0x2F) && !(in->sz & (INTERP_PF3 | INTERP_PF2))) {
        return true; /* v0.18.80: (u)comiss / (u)comisd: ZF PF CF y el resto a cero */
    }
    return f == x_test_eg || f == x_test_ei;
}



/* Los flags de la instrucción i no hacen falta si después, en el mismo
 * bloque y antes de que nadie los lea, otra los pisa todos (v0.18.61: no
 * solo la siguiente; puede haber movs, lea, push/pop... en medio). */
static bool lf_dead_at(const Block *b, unsigned i)
{
    const Insn *a = &b->in[i];
    bool src = lf_writer_all(a) || (a->fn == x_incdec && a->is_reg) || is_rot_reg(a) ||
               (a->fn == x_shift && a->is_reg && a->aop >= 4); /* v0.18.80: también por CL (si es 0 no cambian) */
    if (!src) {
        return false;
    }
    for (unsigned j = i + 1; j < b->n && j < i + 32; j++) {
        if (lf_writer_all(&b->in[j])) {
            return true;
        }
        if (!lf_neutral(&b->in[j])) {
            return false;
        }
    }
    return false;
}

static bool emit_insn(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    InsnFn f = in->fn;
    bool r32 = in->sz == 4;
    unsigned prev_kind = e->lf_kind;
    bool prev_z = e->lf_zres;
    /* v0.18.71: mov entre registros o de un inmediato a un registro, lea y
     * nop no emiten nada que toque NZCV: lo que dejó la anterior sigue ahí
     * (antes un mov entre el cmp y el jcc obligaba a recargar los flags) */
    bool keep_nzcv = f == x_nop || (r32 && (f == x_lea || ((f == x_mov_ge || f == x_mov_eg || f == x_mov_ei) && in->is_reg)));
    e->nzcv_prev = e->nzcv;
    e->nzcv = keep_nzcv ? e->nzcv_prev : 0;
    if (f == x_kcall) { /* v0.18.68: la hora sin salir del JIT */
        e->lf_kind = 0;
        emit_kcall(e, b, i, vm);
        return false; /* el EIP lo puso kfast (o paró) */
    }
    if (f == x_nop || f == x_lea || f == x_mov_ge || f == x_mov_eg || f == x_mov_ei || f == x_movx || f == x_push_r ||
        f == x_push_i || f == x_pop_r || is_mmx_op(f) || f == x_movq_load || f == x_movq_store || f == x_movd_load ||
        f == x_movd_store || f == x_mmx_shift_i || f == x_jcc || f == x_setcc || f == x_cmov) {
        /* no tocan los flags: lo que dejó la anterior sigue valiendo (v0.18.61:
         * también jcc, setcc y cmov, que solo los leen) */
    } else {
        e->lf_kind = 0;
        e->lf_zres = false;
    }
    if (f == x_nop) {
        return true;
    }
    if (emit_mmx(e, b, i, vm)) {
        return true;
    }
    unsigned jk = prev_kind;
    if (!jk && prev_z && f == x_jcc && (in->aop == 4 || in->aop == 5)) {
        jk = LF_LOGIC; /* v0.18.71: jz/jnz tras una operación de 8 o 16 bits: tst del resultado */
    }
    if (emit_more(e, b, i, vm, jk)) {
        return f != x_jcc; /* el jcc pone el EIP él mismo */
    }
    if (emit_x87(e, b, i, vm) || emit_sse(e, b, i, vm)) {
        return true;
    }
    bool nat;
    if (emit_int(e, b, i, vm, prev_kind, &nat)) {
        return nat;
    }
    if (f == x_mov_eg && r32) {
        if (in->is_reg) {
            mov_w(e, k_rmap[in->rm], k_rmap[in->reg]); /* v0.18.71: una sola instrucción */
            return true;
        }
        emit_ea(e, in, true);
        emit_store_src(e, b, i, vm, k_rmap[in->reg]); /* directo del registro del x86 */
        return true;
    }
    if (f == x_mov_ei && r32) {
        if (in->is_reg) {
            movw(e, k_rmap[in->rm], in->imm); /* v0.18.71: directo al registro */
            return true;
        }
        movw(e, W12, in->imm);
        emit_ea(e, in, true);
        emit_store_or_call(e, b, i, vm);
        return true;
    }
    if (f == x_mov_ge && r32) {
        if (in->is_reg) {
            mov_w(e, k_rmap[in->reg], k_rmap[in->rm]);
            return true;
        }
        emit_load_or_call(e, b, i, vm, then_mov_ge);
        return true;
    }
    if (f == x_lea && r32) {
        emit_ea_to(e, in, k_rmap[in->reg]); /* v0.18.61: directo al registro */
        return true;
    }
    if ((f == x_alu_eg || f == x_alu_ge) && r32 && alu_native(in->aop)) {
        if (in->is_reg) {
            int dst = f == x_alu_eg ? in->rm : in->reg, src = f == x_alu_eg ? in->reg : in->rm;
            ldrw(e, W9, XC, ROFF(dst));
            ldrw(e, W10, XC, ROFF(src));
            emit_alu_op(e, in->aop);
            if (in->aop != OP_CMP) {
                strw(e, W11, XC, ROFF(dst));
            }
            return true;
        }
        if (f == x_alu_ge) {
            emit_load_or_call(e, b, i, vm, then_alu_ge);
            return true;
        }
    }
    if (f == x_alu_ei && r32 && in->is_reg && alu_native(in->aop)) {
        ldrw(e, W9, XC, ROFF(in->rm));
        movw(e, W10, in->imm);
        emit_alu_op(e, in->aop);
        if (in->aop != OP_CMP) {
            strw(e, W11, XC, ROFF(in->rm));
        }
        return true;
    }
    if ((f == x_test_eg || f == x_test_ei) && r32 && in->is_reg) {
        ldrw(e, W9, XC, ROFF(in->rm));
        if (f == x_test_eg) {
            ldrw(e, W10, XC, ROFF(in->reg));
        } else {
            movw(e, W10, in->imm);
        }
        emit_alu_op(e, OP_AND);
        return true;
    }
    if ((f == x_push_r && in->reg != R_ESP) || f == x_push_i) {
        if (r32) {
            int src = W12;
            if (f == x_push_r) {
                src = k_rmap[in->reg];
            } else {
                movw(e, W12, in->imm);
            }
            subwi(e, W9, k_rmap[R_ESP], 4);
            uint32_t *s1, *s2, *s3;
            emit_page(e, true, &s1, &s2, &s3);
            strw_r(e, src, W10, W11);
            strw(e, W9, XC, ROFF(R_ESP));
            uint32_t *sites[3] = {s1, s2, s3};
            slow_cold(e, b, i, vm, sites, 3);
            return true;
        }
    }
    if (f == x_pop_r && r32 && in->reg != R_ESP) {
        uint32_t *s1, *s2, *s3;
        mov_w(e, W9, k_rmap[R_ESP]);
        emit_page(e, false, &s1, &s2, &s3);
        ldrw_r(e, k_rmap[in->reg], W10, W11);
        addwi(e, k_rmap[R_ESP], W9, 4);
        uint32_t *sites[2] = {s1, s2};
        slow_cold(e, b, i, vm, sites, 2);
        return true;
    }
    if (emit_extra(e, b, i, vm)) {
        return true;
    }
    emit_call(e, b, i, vm);
    return false;
}

/* v0.18.62: una salida a un bloque de la misma página. Quien salta pasó al
 * entrar las comprobaciones de página y de código, y si escribió en su
 * página ya salió (jit_slow_c); entonces el destino, si se tradujo con la
 * misma página física y el mismo código, no las necesita: su entrada
 * rápida. Si luego alguien escribe en la página, el que salta tampoco
 * vuelve a correr (su entrada normal falla), así que la casilla no se usa.
 * Devuelve la entrada (y la escribe en la casilla) o NULL: a run_blocks. */
static void *jit_resolve_same(Cpu *c, uint32_t eip, uintptr_t slot_rx)
{
    uint32_t pe = c->vm->pt[eip >> 12];
    if (!pe) {
        return NULL;
    }
    uint32_t pp = pe - 1;
    BlockSlot *tab = c->blocks;
    if (!tab) {
        return NULL;
    }
    for (Block *t = tab[(eip ^ (eip >> 16)) & (BLK_HASH - 1)].b; t; t = t->next) {
        if (t->eip == eip && t->jit && t->jit_fast && t->jit_epoch == g_jit.epoch && t->ppage == pp &&
            t->gen == c->vm->gen[pp] && !t->spin) {
            if (g_link_on && slot_rx) {
                *(void **)(slot_rx + (uintptr_t)g_link_delta) = t->jit_fast;
            }
            return t->jit_fast;
        }
    }
    return NULL;
}

/* Una salida al EIP fijo target por su casilla (x24 ya contado). */
static void emit_link(Em *e, uint32_t target)
{
    if (e->nlink >= sizeof(e->link) / sizeof(e->link[0])) {
        /* sin lugar: el resolvedor sin casilla (x14 = 0), como la tabla de antes */
        movw(e, W9, target);
        strw(e, W9, XC, OFF(eip));
        E(e, 0xD280000Eu); /* mov x14, #0 */
        intptr_t d = ((intptr_t)g_jit.resolve_rx - (intptr_t)em_rx(e)) >> 2;
        E(e, 0x14000000u | ((uint32_t)d & 0x3FFFFFFu));
        return;
    }
    e->link[e->nlink].ldr = e->p;
    e->link[e->nlink].target = target;
    e->nlink++;
    E(e, 0x58000000u | 13u);        /* ldr x13, casilla (se parchea) */
    E(e, 0xD61F0000u | 13u << 5);   /* br x13 */
}

/* Al final del bloque: los trocitos que buscan (pone el EIP y va al
 * resolvedor común con x14 = la casilla) y las casillas. */
static void emit_links(Em *e)
{
    uint32_t *stub[80], *adr[80];
    for (unsigned k = 0; k < e->nlink; k++) {
        stub[k] = e->p;
        movw(e, W9, e->link[k].target);
        strw(e, W9, XC, OFF(eip));
        adr[k] = e->p;
        E(e, 0x10000000u | 14u); /* adr x14, casilla (se parchea) */
        bool same = (e->link[k].target >> 12) == e->page;
        intptr_t d = ((intptr_t)(same ? g_jit.same_rx : g_jit.resolve_rx) - (intptr_t)em_rx(e)) >> 2;
        E(e, 0x14000000u | ((uint32_t)d & 0x3FFFFFFu));
    }
    if (((uintptr_t)e->p & 7u) != 0) {
        E(e, 0xD503201Fu); /* nop: las casillas alineadas a 8 */
    }
    if (e->ic_ldr_eip && !e->full) { /* la caché: EIP (8 bytes con relleno) y su entrada */
        uint32_t *ic = e->p;
        /* al principio: un EIP que no existe y, por si acaso, la entrada es
         * el camino de la búsqueda */
        uint64_t miss = (uint64_t)((uintptr_t)g_jit.rx + (uintptr_t)((uint8_t *)e->ic_adr - g_jit.rw));
        E(e, 0xFFFFFFFFu);
        E(e, 0);
        E(e, (uint32_t)miss);
        E(e, (uint32_t)(miss >> 32));
        if (!e->full) {
            int32_t d1 = (int32_t)(ic - e->ic_ldr_eip), d2 = (int32_t)(ic + 2 - e->ic_ldr_entry);
            *e->ic_ldr_eip |= ((uint32_t)d1 & 0x7FFFFu) << 5;
            *e->ic_ldr_entry |= ((uint32_t)d2 & 0x7FFFFu) << 5;
            int32_t da = (int32_t)((uint8_t *)ic - (uint8_t *)e->ic_adr);
            *e->ic_adr |= ((uint32_t)da & 3u) << 29 | (((uint32_t)da >> 2) & 0x7FFFFu) << 5;
        }
        e->ic_ldr_eip = NULL;
    }
    for (unsigned k = 0; k < e->nlink && !e->full; k++) {
        uint32_t *slot = e->p;
        uint64_t v = (uint64_t)((uintptr_t)g_jit.rx + (uintptr_t)((uint8_t *)stub[k] - g_jit.rw));
        E(e, (uint32_t)v);
        E(e, (uint32_t)(v >> 32));
        if (e->full) {
            break;
        }
        int32_t dl = (int32_t)(slot - e->link[k].ldr); /* en palabras */
        *e->link[k].ldr |= ((uint32_t)dl & 0x7FFFFu) << 5;
        int32_t da = (int32_t)((uint8_t *)slot - (uint8_t *)adr[k]); /* en bytes */
        *adr[k] |= ((uint32_t)da & 3u) << 29 | (((uint32_t)da >> 2) & 0x7FFFFu) << 5;
    }
    e->nlink = 0;
}

/* Traduce el bloque; false si no hay JIT o no cupo. */
/* v0.18.71: una racha de push (o de pop) de 32 bits, como en la entrada y la
 * salida de cada función: una sola comprobación de página para todos (el
 * rango entero en la misma página, mapeada y sin código) y luego cada
 * escritura o lectura con su desplazamiento. Si la comprobación falla, el
 * código de siempre de cada uno. Devuelve cuántas instrucciones tomó (0:
 * ninguna, no es una racha). */
static int g_jit_no_runs = -1;
static int g_jit_xc = -1; /* v0.18.80: CPU_SIN_XMM=1 deja los xmm siempre en memoria */
static int g_jit_pc = -1; /* v0.18.80: CPU_SIN_PAGINA=1 apaga la página del operando anterior */
static bool is_push32(const Insn *in)
{
    return in->sz == 4 && ((in->fn == x_push_r && in->reg != R_ESP) || in->fn == x_push_i);
}
static bool is_pop32(const Insn *in) { return in->sz == 4 && in->fn == x_pop_r && in->reg != R_ESP; }

static unsigned emit_stack_run(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    if (g_jit_no_runs) {
        return 0;
    }
    bool push = is_push32(&b->in[i]);
    if (!push && !is_pop32(&b->in[i])) {
        return 0;
    }
    unsigned k = 1;
    while (i + k < b->n && k < 8 && (push ? is_push32(&b->in[i + k]) : is_pop32(&b->in[i + k]))) {
        k++;
    }
    if (k < 2) {
        return 0;
    }
    uint32_t *s1, *s2, *s3;
    if (push) {
        subwi(e, W9, k_rmap[R_ESP], 4 * k);
    } else {
        mov_w(e, W9, k_rmap[R_ESP]);
    }
    emit_page_n(e, push, 4 * k, &s1, &s2, &s3);
    E(e, 0x8B0B014Du); /* add x13, x10, x11 */
    for (unsigned j = 0; j < k; j++) {
        const Insn *in = &b->in[i + j];
        if (push) {
            int src = W12;
            if (in->fn == x_push_r) {
                src = k_rmap[in->reg];
            } else {
                movw(e, W12, in->imm);
            }
            strw(e, src, 13, 4 * (k - 1 - j)); /* el primero, más arriba */
        } else {
            ldrw(e, k_rmap[in->reg], 13, 4 * j);
        }
    }
    if (push) {
        mov_w(e, k_rmap[R_ESP], W9);
    } else {
        addwi(e, k_rmap[R_ESP], W9, 4 * k);
    }
    uint32_t *done = fwd_b(e);
    patch(e, s1);
    patch(e, s2);
    patch(e, s3);
    for (unsigned j = 0; j < k; j++) {
        emit_insn(e, b, i + j, vm); /* el camino de siempre, uno por uno */
    }
    patch(e, done);
    e->nzcv = e->nzcv_prev = 0;
    return k;
}

static bool jit_compile(Cpu *c, Block *b)
{
    if (!jit_init() || !c->vm->code || !b->n) {
        return false;
    }
    jit_begin_write();
    if (!g_jit.base) {
        /* las rutinas comunes, una vez al principio del búfer */
        Em s0;
        memset(&s0, 0, sizeof(s0));
        s0.p = (uint32_t *)g_jit.rw;
        s0.end = (uint32_t *)(g_jit.rw + 4096);
        g_jit.slow_rx = (uintptr_t)g_jit.rx;
        emit_slow_stub(&s0);
        /* la entrada: x19 = c, x20 = ram, x21 = tabla de páginas, x22 = code[],
         * x23 = presupuesto que queda, x24 = instrucciones de los bloques encadenados */
        g_jit.enter_rx = em_rx(&s0);
        E(&s0, 0xA9BA7BFDu); /* stp x29, x30, [sp, #-96]! */
        E(&s0, 0xA90153F3u); /* stp x19, x20, [sp, #16] */
        E(&s0, 0xA9025BF5u); /* stp x21, x22, [sp, #32] */
        E(&s0, 0xA90363F7u); /* stp x23, x24, [sp, #48] */
        E(&s0, 0xA9046BF9u); /* stp x25, x26, [sp, #64] */
        E(&s0, 0xA90573FBu); /* stp x27, x28, [sp, #80] (v0.18.80) */
        E(&s0, 0xAA0203EAu); /* mov x10, x2 (w2 es eax) */
        movx_r(&s0, XC, X0);
        movx_r(&s0, 23, X1);
        movx(&s0, 24, 0);
        regs_load(&s0);
        ldrx(&s0, W9, XC, OFF(vm));
        ldrx(&s0, XRAM, W9, (uint32_t)offsetof(Vm, ram));
        ldrx(&s0, XPT, W9, (uint32_t)offsetof(Vm, pt));
        ldrx(&s0, XCODE, W9, (uint32_t)offsetof(Vm, code));
        ldrx(&s0, 26, W9, (uint32_t)offsetof(Vm, gen)); /* v0.18.68: x26 = gen[], x25 = &yield */
        ldrx(&s0, 25, XC, OFF(yield));
        E(&s0, 0xD1400694u); /* sub x20, x20, #1, lsl #12: la tabla de páginas guarda página + 1 */
        E(&s0, 0xD10006D6u); /* sub x22, x22, #1 */
        E(&s0, 0xD61F0000u | 10u << 5); /* br x10 */
        /* la salida: w0 = las de este bloque */
        g_jit.epi_rx = em_rx(&s0);
        regs_spill(&s0);
        strw(&s0, XZR, XC, OFF(jit_cur)); /* fuera del JIT */
        E(&s0, 0x0B180000u); /* add w0, w0, w24 */
        E(&s0, 0xA94573FBu); /* ldp x27, x28, [sp, #80] */
        E(&s0, 0xA9446BF9u); /* ldp x25, x26, [sp, #64] */
        E(&s0, 0xA94363F7u); /* ldp x23, x24, [sp, #48] */
        E(&s0, 0xA9425BF5u); /* ldp x21, x22, [sp, #32] */
        E(&s0, 0xA94153F3u); /* ldp x19, x20, [sp, #16] */
        E(&s0, 0xA8C67BFDu); /* ldp x29, x30, [sp], #96 */
        E(&s0, 0xD65F03C0u); /* ret */
        /* v0.18.61, el resolvedor: w9 = EIP (ya puesto en c->eip), x14 = la
         * casilla (vista ejecutable; 0: ninguna) */
        g_jit.resolve_rx = em_rx(&s0);
        E(&s0, 0x4A49412Au);                          /* eor w10, w9, w9, lsr #16 */
        E(&s0, 0x1200454Au);                          /* and w10, w10, #0x3FFFF */
        movx(&s0, W11, (uint64_t)(uintptr_t)g_jtab);
        E(&s0, 0x8B0A116Bu);                          /* add x11, x11, x10, lsl #4 */
        ldrw(&s0, W12, W11, 0);
        cmpw(&s0, W12, W9);
        uint32_t *rmiss1 = fwd_bcond(&s0, COND_NE);
        ldrx(&s0, W13, W11, 8);
        uint32_t *rmiss2 = fwd_cb(&s0, J_CBZ | 0x80000000u, W13); /* cbz x13 */
        uint32_t *rgo1 = fwd_cb(&s0, J_CBZ | 0x80000000u, 14);    /* cbz x14: sin casilla */
        movx(&s0, W12, (uint64_t)(uintptr_t)&g_link_on);
        E(&s0, 0x39400000u | 12u << 5 | 12u);         /* ldrb w12, [x12] */
        uint32_t *rgo2 = fwd_cb(&s0, J_CBZ, W12);
        movx(&s0, W12, (uint64_t)(uintptr_t)&g_link_delta);
        ldrx(&s0, W12, W12, 0);
        E(&s0, 0x8B0C01CEu);                          /* add x14, x14, x12 */
        E(&s0, 0xF90001CDu);                          /* str x13, [x14] */
        patch(&s0, rgo1);
        patch(&s0, rgo2);
        E(&s0, 0xD61F0000u | 13u << 5);               /* br x13 */
        patch(&s0, rmiss1);
        patch(&s0, rmiss2);
        movw(&s0, X0, 0);
        {
            intptr_t d = ((intptr_t)g_jit.epi_rx - (intptr_t)em_rx(&s0)) >> 2;
            E(&s0, 0x14000000u | ((uint32_t)d & 0x3FFFFFFu)); /* b al epílogo */
        }
        /* v0.18.62, el de la misma página: w9 = EIP, x14 = la casilla; en C */
        g_jit.same_rx = em_rx(&s0);
        regs_spill(&s0);
        movx_r(&s0, X0, XC);
        movw_r(&s0, X1, W9);
        movx_r(&s0, 2, 14);
        movx(&s0, W9, (uint64_t)(uintptr_t)jit_resolve_same);
        E(&s0, 0xD63F0000u | 9u << 5); /* blr x9 */
        movx_r(&s0, W13, X0);
        regs_load(&s0);
        uint32_t *smiss = fwd_cb(&s0, J_CBZ | 0x80000000u, W13); /* cbz x13 */
        E(&s0, 0xD61F0000u | 13u << 5);                          /* br x13 */
        patch(&s0, smiss);
        movw(&s0, X0, 0);
        {
            intptr_t d = ((intptr_t)g_jit.epi_rx - (intptr_t)em_rx(&s0)) >> 2;
            E(&s0, 0x14000000u | ((uint32_t)d & 0x3FFFFFFu)); /* b al epílogo */
        }
        /* v0.18.63, el de la caché de un EIP variable: w9 = EIP, x14 = la caché */
        g_jit.ic_rx = em_rx(&s0);
        E(&s0, 0x4A49412Au);                          /* eor w10, w9, w9, lsr #16 */
        E(&s0, 0x1200454Au);                          /* and w10, w10, #0x3FFFF */
        movx(&s0, W11, (uint64_t)(uintptr_t)g_jtab);
        E(&s0, 0x8B0A116Bu);                          /* add x11, x11, x10, lsl #4 */
        ldrw(&s0, W12, W11, 0);
        cmpw(&s0, W12, W9);
        uint32_t *imiss1 = fwd_bcond(&s0, COND_NE);
        ldrx(&s0, W13, W11, 8);
        uint32_t *imiss2 = fwd_cb(&s0, J_CBZ | 0x80000000u, W13); /* cbz x13 */
        movx(&s0, W12, (uint64_t)(uintptr_t)&g_link_delta);
        ldrx(&s0, W12, W12, 0);
        E(&s0, 0x8B0C01CEu);                          /* add x14, x14, x12 */
        E(&s0, 0xB90001C9u);                          /* str w9, [x14] */
        E(&s0, 0xF90005CDu);                          /* str x13, [x14, #8] */
        E(&s0, 0xD61F0000u | 13u << 5);               /* br x13 */
        patch(&s0, imiss1);
        patch(&s0, imiss2);
        movw(&s0, X0, 0);
        {
            intptr_t d = ((intptr_t)g_jit.epi_rx - (intptr_t)em_rx(&s0)) >> 2;
            E(&s0, 0x14000000u | ((uint32_t)d & 0x3FFFFFFu)); /* b al epílogo */
        }
        size_t len = (size_t)((uint8_t *)s0.p - g_jit.rw);
        jit_publish(0, len);
        jit_begin_write();
        g_jit.base = (len + 63) & ~(size_t)63;
        g_jit.used = g_jit.base;
    }
    bool xc_off = false;
    if (g_jit_lento < 0) {
        const char *l = getenv("CPU_JIT_LENTO");
        g_jit_lento = l ? atoi(l) : 0;
    }
    if (g_jit_xc < 0) {
        g_jit_xc = getenv("CPU_SIN_XMM") == NULL;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        b->slow = 0;
        b->helper = 0;
        Em e;
        memset(&e, 0, sizeof(e)); /* v0.18.73: nada sin inicializar */
        /* primero las Insn (las funciones de C las reciben) y luego el código */
        size_t ibytes = sizeof(JHdr) + (((size_t)b->n * sizeof(Insn) + 15) & ~(size_t)15);
        if (g_jit.used + ibytes + 4096 > g_jit.size) {
            g_jit.used = g_jit.base;
            g_jit.epoch++;
            jit_table_clear();
            memset(g_jcnt, 0, sizeof(g_jcnt));
            g_jcnt_next = 1;
        }
        size_t data_off = g_jit.used;
        JHdr hdr = {b->ppage, b->gen, {0, 0}};
        memcpy(g_jit.rw + data_off, &hdr, sizeof(hdr));
        memcpy(g_jit.rw + data_off + sizeof(JHdr), b->in, (size_t)b->n * sizeof(Insn));
        e.insn_rx = (const Insn *)(g_jit.rx + data_off + sizeof(JHdr));
        e.p = (uint32_t *)(g_jit.rw + data_off + ibytes);
        e.end = (uint32_t *)(g_jit.rw + g_jit.size);
        e.nexits = 0;
        e.full = false;
        e.fallback = false;
        e.lf_kind = 0;
        e.lf_zres = false; /* v0.18.73: sin esto venía con basura (saltos al revés) */
        e.ncold = e.npool = 0;
        e.nzcv = e.nzcv_prev = 0;
        e.jcc_site = NULL;
        e.jcc_none = false;
        e.exit_eip = 0;
        e.fj = false;
        e.nlink = 0;
        e.ic_ldr_eip = NULL;
        e.pc_live = e.pc_cand = false;
        e.xc_on = g_jit_xc && !xc_off;
        e.xl = e.xd = 0;
        e.ea_mark = NULL;
        memset(e.pc_est, 0, sizeof(e.pc_est));
        uint32_t *start = e.p;
        Slow bx = {{0}, 0}; /* no se puede entrar: vuelve a run_blocks */
        /* entrada desde otro bloque */
        uint32_t *chain = e.p;
        e.page = b->eip >> 12;
        /* v0.18.68: con desplazamientos inmediatos cuando caben (x21 = pt[],
         * x26 = gen[], x25 = &yield) */
        uint32_t vp = b->eip >> 12;
        if (vp < 4096) {
            E(&e, 0xB9400000u | vp << 10 | (uint32_t)XPT << 5 | W10); /* ldr w10, [x21, #vp*4] */
        } else {
            movw(&e, W9, vp);
            ldrw_lsl2(&e, W10, XPT, W9);
        }
        if (b->ppage + 1 < 4096) {
            cmpwi(&e, W10, b->ppage + 1);
        } else {
            movw(&e, W11, b->ppage + 1);
            cmpw(&e, W10, W11);
        }
        slow_bcond(&e, &bx, COND_NE);                /* la página ya es otra */
        if (b->ppage < 4096) {
            E(&e, 0xB9400000u | b->ppage << 10 | 26u << 5 | W10); /* ldr w10, [x26, #ppage*4] */
        } else {
            movw(&e, W9, b->ppage);
            ldrw_lsl2(&e, W10, 26, W9);
        }
        if (b->gen < 4096) {
            cmpwi(&e, W10, b->gen);
        } else {
            movw(&e, W11, b->gen);
            cmpw(&e, W10, W11);
        }
        slow_bcond(&e, &bx, COND_NE);                /* el código cambió */
        uint32_t *fast = e.p; /* v0.18.62: desde la misma página se entra aquí */
        E(&e, 0xF1000000u | b->n << 10 | 23u << 5 | 23u); /* subs x23, x23, #n */
        slow_bcond(&e, &bx, 3);                      /* b.lo: no alcanza */
        E(&e, 0x39400000u | 25u << 5 | W9);          /* ldrb w9, [x25] */
        slow_add(&e, &bx, fwd_cb(&e, J_CBNZ, W9));   /* una interrupción */
        /* etapa 5: en vez de contar (leer, sumar, escribir), dice qué bloque
         * corre; un hilo aparte (sampler.c) lo muestrea */
        uint32_t cnt = 0;
        movw(&e, W9, b->eip);
        strw(&e, W9, XC, OFF(jit_cur));
        bool stale = false;
        /* etapa 5, superbloques: un jcc a mitad del bloque; si salta, sale
         * por aquí con las instrucciones hechas hasta él */
        uint32_t *side_at[BLK_MAX];
        unsigned side_k[BLK_MAX], nside = 0;
        uint32_t side_tgt[BLK_MAX]; /* 0: el EIP ya está puesto */
        for (unsigned i = 0; i < b->n; i++) {
            if (g_jit_no_dead < 0) {
                g_jit_no_dead = getenv("CPU_SIN_MUERTOS") != NULL;
            }
            if (g_jit_no_runs < 0) {
                g_jit_no_runs = getenv("CPU_SIN_RACHAS") != NULL;
            }
            e.lf_dead = !g_jit_no_dead && lf_dead_at(b, i);
            e.cur_b = b;
            e.cur_in = &b->in[i];
            e.cur_i = i;
            e.ea_mark = NULL;
            e.pc_cand = false;
            /* v0.18.80: los xmm en registros */
            if (e.xc_on) {
                const Insn *xi = &b->in[i];
                if (xi->fn == x_kcall) {
                    xc_drop(&e);
                } else if (xi->fn == x_jcc || i + 1 == b->n) {
                    xc_flush(&e); /* puede salir del bloque */
                }
                e.xc_locked = false;
                if (xi->fn == x_interp && (xi->sz & (INTERP_MARK | INTERP_0F)) == (INTERP_MARK | INTERP_0F) &&
                    !(xi->sz & (INTERP_P66 | INTERP_PF2))) {
                    unsigned xop = xi->aop;
                    bool full = (xop == 0x10 || xop == 0x28) && (!xi->is_reg || !(xi->sz & INTERP_PF3));
                    if (!full) {
                        xc_need(&e, xi->reg & 7u);
                    }
                    if (xi->is_reg) {
                        xc_need(&e, xi->rm & 7u);
                    }
                }
                e.xc_locked = true;
            }
            e.xd_before = e.xd;
            e.xc_cd = e.xd;
            e.xc_cl = e.xl;
            e.xw_first = NULL;
            e.fz = false;
            e.jcc_site = NULL;
            e.jcc_none = false;
            e.exit_eip = 0;
            e.fj = false;
            unsigned run = emit_stack_run(&e, b, i, c->vm);
            if (run) {
                i += run - 1;
                stale = true;
                e.lf_dead = false;
                e.pc_live = false;
                continue;
            }
            if (g_jit_pc < 0) {
                g_jit_pc = getenv("CPU_SIN_PAGINA") == NULL;
            }
            e.pc_cand = g_jit_pc && pc_cand_insn(&b->in[i]);
            stale = emit_insn(&e, b, i, c->vm);
            e.pc_cand = false;
            e.lf_dead = false;
            if (e.pc_live && (!stale ||
                              (e.pc_base != NOREG && pc_writes(&b->in[i], e.pc_base)) ||
                              (e.pc_index != NOREG && pc_writes(&b->in[i], e.pc_index)))) {
                e.pc_live = false;
            }
            if (b->in[i].fn == x_jcc && i + 1 < b->n && !e.jcc_none) {
                if (e.jcc_site) { /* b.cond directo: la salida pone el EIP del destino */
                    side_at[nside] = e.jcc_site;
                    side_tgt[nside] = b->in[i].imm;
                } else {
                    ldrw(&e, W9, XC, OFF(eip));
                    movw(&e, W10, b->in[i].next);
                    cmpw(&e, W9, W10);
                    side_at[nside] = fwd_bcond(&e, COND_NE);
                    side_tgt[nside] = 0;
                }
                side_k[nside++] = i + 1;
            }
        }
        e.xc_locked = false;
        xc_flush(&e); /* v0.18.80: los xmm cambiados, a memoria antes de salir */
        if (e.xc_bad) { /* no se pudo con los xmm en registros: otra vez sin eso */
            xc_off = true;
            attempt--;
            continue;
        }
        /* terminó: el siguiente (v0.18.61: si es fijo, por su casilla) */
        uint32_t fixed = stale ? b->in[b->n - 1].next : e.exit_eip;
        if (stale && !g_link_on) {
            movw(&e, W9, b->in[b->n - 1].next);
            strw(&e, W9, XC, OFF(eip));
        }
        E(&e, 0x91000000u | b->n << 10 | 24u << 5 | 24u); /* add x24, x24, #n */
        if (g_link_on && e.fj) {
            uint32_t *tk = fwd_bcond(&e, e.fj_cc);
            emit_link(&e, e.fj_next);
            patch(&e, tk);
            emit_link(&e, e.fj_taken);
        } else if (g_link_on && fixed) {
            emit_link(&e, fixed);
        }
        uint32_t *look = e.p;
        uint32_t *miss1 = NULL, *miss2 = NULL;
        ldrw(&e, W9, XC, OFF(eip));
        if (g_link_on) {
            /* v0.18.63: primero la caché del último EIP de aquí (ret a quien
             * llamó, el mismo salto indirecto); si no, el resolvedor común */
            e.ic_ldr_eip = e.p;
            E(&e, 0x18000000u | W12);                /* ldr w12, caché.eip (se parchea) */
            cmpw(&e, W12, W9);
            uint32_t *icm = fwd_bcond(&e, COND_NE);
            e.ic_ldr_entry = e.p;
            E(&e, 0x58000000u | 13u);                /* ldr x13, caché.entrada */
            E(&e, 0xD61F0000u | 13u << 5);           /* br x13 */
            patch(&e, icm);
            e.ic_adr = e.p;
            E(&e, 0x10000000u | 14u);                /* adr x14, caché */
            intptr_t d = ((intptr_t)g_jit.ic_rx - (intptr_t)em_rx(&e)) >> 2;
            E(&e, 0x14000000u | ((uint32_t)d & 0x3FFFFFFu));
        } else {
            E(&e, 0x4A49412Au);                          /* eor w10, w9, w9, lsr #16 */
            E(&e, 0x1200454Au);                          /* and w10, w10, #0x3FFFF (JTAB - 1) */
            movx(&e, W11, (uint64_t)(uintptr_t)g_jtab);
            E(&e, 0x8B0A116Bu);                          /* add x11, x11, x10, lsl #4 */
            ldrw(&e, W12, W11, 0);
            cmpw(&e, W12, W9);
            miss1 = fwd_bcond(&e, COND_NE);
            ldrx(&e, W13, W11, 8);
            miss2 = fwd_cb(&e, J_CBZ | 0x80000000u, W13); /* cbz x13 */
            E(&e, 0xD61F0000u | 13u << 5);               /* br x13 */
        }
        /* las salidas de los jcc de en medio: devuelve al presupuesto las que
         * no se hicieron, cuenta las hechas y busca el siguiente */
        for (unsigned k = 0; k < nside; k++) {
            patch(&e, side_at[k]);
            if (side_tgt[k] && !g_link_on) {
                movw(&e, W9, side_tgt[k]);
                strw(&e, W9, XC, OFF(eip));
            }
            E(&e, 0x91000000u | (b->n - side_k[k]) << 10 | 23u << 5 | 23u); /* add x23, x23, #(n - k) */
            E(&e, 0x91000000u | side_k[k] << 10 | 24u << 5 | 24u);          /* add x24, x24, #k */
            if (side_tgt[k] && g_link_on) {
                emit_link(&e, side_tgt[k]);
            } else {
                E(&e, 0x14000000u | ((uint32_t)(look - e.p) & 0x3FFFFFFu)); /* b look */
            }
        }
        /* no se pudo entrar: el EIP es el de este bloque y nada hecho */
        for (unsigned k = 0; k < bx.n; k++) {
            patch(&e, bx.at[k]);
        }
        movw(&e, W9, b->eip);
        strw(&e, W9, XC, OFF(eip));
        patch(&e, miss1);
        patch(&e, miss2);
        movw(&e, X0, 0);
        {
            intptr_t d = ((intptr_t)g_jit.epi_rx - (intptr_t)em_rx(&e)) >> 2;
            E(&e, 0x14000000u | ((uint32_t)d & 0x3FFFFFFu)); /* b al epílogo común */
        }
        emit_colds(&e, b, c->vm); /* los caminos lentos, fuera de la línea */
        emit_links(&e);           /* los trocitos de las salidas fijas y sus casillas */
        if (e.full) {
            /* sin espacio: se vacía todo y se intenta otra vez */
            g_jit.used = g_jit.base;
            g_jit.epoch++;
            jit_table_clear();
            memset(g_jcnt, 0, sizeof(g_jcnt));
            g_jcnt_next = 1;
            continue;
        }
        for (unsigned k = 0; k < e.nexits; k++) {
            uintptr_t at = (uintptr_t)g_jit.rx + (uintptr_t)((uint8_t *)e.exits[k] - g_jit.rw);
            *e.exits[k] |= (uint32_t)(((intptr_t)g_jit.epi_rx - (intptr_t)at) >> 2) & 0x3FFFFFFu;
        }
        size_t off = (size_t)((uint8_t *)start - g_jit.rw), len = (size_t)((uint8_t *)e.p - (uint8_t *)start);
        jit_publish(data_off, off + len - data_off);
        g_jit.used = (off + len + 15) & ~(size_t)15;
        b->jit = (void *)(g_jit.rx + off);
        b->jit_chain = (void *)(g_jit.rx + off + (size_t)((uint8_t *)chain - (uint8_t *)start));
        b->jit_fast = (void *)(g_jit.rx + off + (size_t)((uint8_t *)fast - (uint8_t *)start));
        b->jcnt = cnt;
        b->jit_epoch = g_jit.epoch;
        g_jit.compiled++;
        return true;
    }
    jit_publish(0, 0);
    return false;
}
