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

/* Encadenar: al terminar, un bloque traducido busca el siguiente (por su
 * EIP) en esta tabla y salta a su entrada sin volver a run_blocks. Cada
 * entrada comprueba presupuesto, interrupción, página y generación del
 * código antes de correr; si algo no va, vuelve a run_blocks. */
#define JTAB (1u << 16)
typedef struct JSlot {
    uint32_t eip, pad;
    void *entry;
} JSlot;
static JSlot g_jtab[JTAB];
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
} Em;

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
static void exit_k(Em *e, unsigned k)
{
    movw(e, X0, k);
    if (e->nexits < sizeof(e->exits) / sizeof(e->exits[0])) {
        e->exits[e->nexits++] = fwd_b(e);
    } else {
        e->full = true;
    }
}

/* w9 = la dirección del operando de memoria */
static void emit_ea(Em *e, const Insn *in, bool with_fs)
{
    movw(e, W9, in->disp);
    if (in->base != NOREG) {
        ldrw(e, W10, XC, ROFF(in->base));
        rrr(e, ADDW, W9, W9, W10);
    }
    if (in->index != NOREG) {
        ldrw(e, W10, XC, ROFF(in->index));
        addw_lsl(e, W9, W9, W10, in->scale);
    }
    if (with_fs && in->fs) {
        ldrw(e, W10, XC, OFF(fs_base));
        rrr(e, ADDW, W9, W9, W10);
    }
}

/* Página de w9 (4 bytes enteros dentro): x10 = memoria del host, x11 = offset.
 * Si no está (o es de código y se va a escribir) salta al camino lento. */
static void emit_page(Em *e, bool write, uint32_t **slow1, uint32_t **slow2, uint32_t **slow3)
{
    lsrw(e, W10, W9, 12);
    ldrw_lsl2(e, W10, XPT, W10);
    *slow1 = fwd_cb(e, J_CBZ, W10);
    ubfx12(e, W11, W9);
    cmpwi(e, W11, 0xFFC);
    *slow2 = fwd_bcond(e, COND_HI);
    subwi(e, W10, W10, 1);
    *slow3 = NULL;
    if (write) { /* página con código traducido: lo hace la función (vm_touch) */
        ldrb_r(e, W13, XCODE, W10);
        *slow3 = fwd_cb(e, J_CBNZ, W13);
    }
    addx_lsl(e, W10, XRAM, W10, 12);
}

/* Llamar a la función de siempre de la instrucción i (con sus comprobaciones). */
static void emit_call(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    if (!e->fallback) {
        ((Block *)b)->slow |= 1ull << (i & 63); /* para el perfil (no el camino de respaldo) */
    }
    /* cuenta para la pantalla (lentas / de respaldo) */
    {
        uint32_t o = e->fallback ? OFF(st_cfb) : OFF(st_ccall);
        ldrx(e, W9, XC, o);
        E(e, 0x91000400u | 9u << 5 | 9u); /* add x9, x9, #1 */
        E(e, 0xF9000000u | (o / 8) << 10 | (uint32_t)XC << 5 | 9u);
    }
    /* la función (rdtsc, los relojes del hardware) ve icount al día: las de
     * los bloques encadenados antes (x24) y las i de este */
    ldrx(e, W9, XC, OFF(icount));
    E(e, 0x8B180129u); /* add x9, x9, x24 */
    if (i) {
        E(e, 0x91000000u | i << 10 | 9u << 5 | 9u); /* add x9, x9, #i */
    }
    E(e, 0xF9000000u | (OFF(icount) / 8) << 10 | (uint32_t)XC << 5 | 9u); /* str x9 */
    movw(e, W9, in->next);
    strw(e, W9, XC, OFF(eip));
    movx_r(e, X0, XC);
    /* la copia de la Insn junto al código: sigue valiendo aunque el bloque se libere */
    movx(e, X1, (uint64_t)(uintptr_t)(e->insn_rx + i));
    movx(e, W9, (uint64_t)(uintptr_t)in->fn);
    regs_spill(e);
    E(e, 0xD63F0000u | 9u << 5); /* blr x9 */
    regs_load(e);
    ldrx(e, W9, XC, OFF(icount));
    E(e, 0xCB180129u); /* sub x9, x9, x24 */
    if (i) {
        E(e, 0xD1000000u | i << 10 | 9u << 5 | 9u); /* sub x9, x9, #i */
    }
    E(e, 0xF9000000u | (OFF(icount) / 8) << 10 | (uint32_t)XC << 5 | 9u);
    /* paró: se hicieron i (y se dice en cuál) */
    ldrw(e, W9, XC, OFF(stop));
    uint32_t *ok = fwd_cb(e, J_CBZ, W9);
    movw(e, W9, in->start);
    strw(e, W9, XC, OFF(jit_stop_eip));
    exit_k(e, i);
    patch(e, ok);
    /* saltó: sale tras ella */
    if (ends_block(in->fn) || in->fn == x_interp) {
        ldrw(e, W9, XC, OFF(eip));
        movw(e, W10, in->next);
        cmpw(e, W9, W10);
        uint32_t *same = fwd_bcond(e, COND_EQ);
        exit_k(e, i + 1);
        patch(e, same);
    }
    /* escribió en el código de este bloque */
    movx(e, W9, (uint64_t)(uintptr_t)&vm->gen[b->ppage]);
    ldrw(e, W9, W9, 0);
    movw(e, W10, b->gen);
    cmpw(e, W9, W10);
    uint32_t *same = fwd_bcond(e, COND_EQ);
    exit_k(e, i + 1);
    patch(e, same);
}

/* lf_* de una operación de 32 bits: w9 = a, w10 = b, w11 = res */
static void emit_lf(Em *e, unsigned kind)
{
    e->lf_kind = kind;
    movw(e, W12, kind);
    strb(e, W12, XC, OFF(lf_op));
    movw(e, W12, 4);
    strb(e, W12, XC, OFF(lf_sz));
    strb(e, XZR, XC, OFF(lf_cin));
    strw(e, W9, XC, OFF(lf_a));
    strw(e, W10, XC, OFF(lf_b));
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

/* Una carga de 32 bits de [ea] a w12, o la función de siempre. done: tras ambas. */
static void emit_load_or_call(Em *e, const Block *b, unsigned i, const Vm *vm, void (*then)(Em *, const Insn *))
{
    const Insn *in = &b->in[i];
    uint32_t *s1, *s2, *s3;
    emit_ea(e, in, true);
    emit_page(e, false, &s1, &s2, &s3);
    ldrw_r(e, W12, W10, W11);
    then(e, in);
    uint32_t *done = fwd_b(e);
    patch(e, s1);
    patch(e, s2);
    e->fallback = true;
    emit_call(e, b, i, vm);
    e->fallback = false;
    patch(e, done);
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

/* Guardar w12 en [w9] o la función de siempre. */
static void emit_store_or_call(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    uint32_t *s1, *s2, *s3;
    emit_page(e, true, &s1, &s2, &s3);
    strw_r(e, W12, W10, W11);
    uint32_t *done = fwd_b(e);
    patch(e, s1);
    patch(e, s2);
    patch(e, s3);
    e->fallback = true;
    emit_call(e, b, i, vm);
    e->fallback = false;
    patch(e, done);
}

/* ---- MMX en NEON (2d.3): los registros mm viven en c->fp.mm; v0 = destino,
 * v1 = fuente, v2 de paso. Halo 2 usa MMX sin parar. ---- */

#define MMOFF(n) (OFF(fp.mm) + 8u * (uint32_t)(n))

static void ldrd(Em *e, int vt, int rn, uint32_t off) { E(e, 0xFD400000u | ((off / 8) << 10) | (uint32_t)rn << 5 | (uint32_t)vt); }
static void strd(Em *e, int vt, int rn, uint32_t off) { E(e, 0xFD000000u | ((off / 8) << 10) | (uint32_t)rn << 5 | (uint32_t)vt); }
static void ldrd_r(Em *e, int vt, int rn, int rm) { E(e, 0xFC606800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)vt); }
static void strd_r(Em *e, int vt, int rn, int rm) { E(e, 0xFC206800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)vt); }

/* Como emit_page, para 8 bytes. */
static void emit_page8(Em *e, bool write, uint32_t **slow1, uint32_t **slow2, uint32_t **slow3)
{
    lsrw(e, W10, W9, 12);
    ldrw_lsl2(e, W10, XPT, W10);
    *slow1 = fwd_cb(e, J_CBZ, W10);
    ubfx12(e, W11, W9);
    cmpwi(e, W11, 0xFF8);
    *slow2 = fwd_bcond(e, COND_HI);
    subwi(e, W10, W10, 1);
    *slow3 = NULL;
    if (write) {
        ldrb_r(e, W13, XCODE, W10);
        *slow3 = fwd_cb(e, J_CBNZ, W13);
    }
    addx_lsl(e, W10, XRAM, W10, 12);
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
    uint32_t *done = fwd_b(e);
    patch(e, s1);
    patch(e, s2);
    e->fallback = true;
    emit_call(e, b, i, vm);
    e->fallback = false;
    patch(e, done);
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
        uint32_t *done = fwd_b(e);
        patch(e, s1);
        patch(e, s2);
        patch(e, s3);
        e->fallback = true;
        emit_call(e, b, i, vm);
        e->fallback = false;
        patch(e, done);
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
    lsrw(e, W10, W9, 12);
    ldrw_lsl2(e, W10, XPT, W10);
    *slow1 = fwd_cb(e, J_CBZ, W10);
    ubfx12(e, W11, W9);
    cmpwi(e, W11, 0x1000 - n);
    *slow2 = fwd_bcond(e, COND_HI);
    subwi(e, W10, W10, 1);
    *slow3 = NULL;
    if (write) {
        ldrb_r(e, W13, XCODE, W10);
        *slow3 = fwd_cb(e, J_CBNZ, W13);
    }
    addx_lsl(e, W10, XRAM, W10, 12);
}

/* Dónde está un registro de 8 bits (AL..BH) dentro de c->r. */
static uint32_t r8off(unsigned r) { return r < 4 ? ROFF(r) : ROFF(r - 4) + 1; }

static void fallback_tail(Em *e, const Block *b, unsigned i, const Vm *vm, uint32_t *done, uint32_t *s1,
                          uint32_t *s2, uint32_t *s3)
{
    patch(e, s1);
    patch(e, s2);
    if (s3) {
        patch(e, s3);
    }
    e->fallback = true;
    emit_call(e, b, i, vm);
    e->fallback = false;
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
    if (f == x_jcc && (prev_kind == LF_SUB || prev_kind == LF_LOGIC)) {
        int cc = arm_cond(prev_kind, in->aop);
        if (cc < 0) {
            return false;
        }
        movw(e, W12, in->next);
        if (cc == 14) {
            movw(e, W12, in->imm);
        } else if (cc != 16) {
            if (prev_kind == LF_SUB) {
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
static bool emit_insn(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    InsnFn f = in->fn;
    bool r32 = in->sz == 4;
    unsigned prev_kind = e->lf_kind;
    if (f == x_nop || f == x_lea || f == x_mov_ge || f == x_mov_eg || f == x_mov_ei || f == x_movx || f == x_push_r ||
        f == x_push_i || f == x_pop_r || is_mmx_op(f) || f == x_movq_load || f == x_movq_store || f == x_movd_load ||
        f == x_movd_store || f == x_mmx_shift_i) {
        /* no tocan los flags: lo que dejó la anterior sigue valiendo */
    } else {
        e->lf_kind = 0;
    }
    if (f == x_nop) {
        return true;
    }
    if (emit_mmx(e, b, i, vm)) {
        return true;
    }
    if (emit_more(e, b, i, vm, prev_kind)) {
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
        ldrw(e, W12, XC, ROFF(in->reg));
        if (in->is_reg) {
            strw(e, W12, XC, ROFF(in->rm));
            return true;
        }
        emit_ea(e, in, true);
        emit_store_or_call(e, b, i, vm);
        return true;
    }
    if (f == x_mov_ei && r32) {
        movw(e, W12, in->imm);
        if (in->is_reg) {
            strw(e, W12, XC, ROFF(in->rm));
            return true;
        }
        emit_ea(e, in, true);
        emit_store_or_call(e, b, i, vm);
        return true;
    }
    if (f == x_mov_ge && r32) {
        if (in->is_reg) {
            ldrw(e, W12, XC, ROFF(in->rm));
            strw(e, W12, XC, ROFF(in->reg));
            return true;
        }
        emit_load_or_call(e, b, i, vm, then_mov_ge);
        return true;
    }
    if (f == x_lea && r32) {
        emit_ea(e, in, false);
        strw(e, W9, XC, ROFF(in->reg));
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
            if (f == x_push_r) {
                ldrw(e, W12, XC, ROFF(in->reg));
            } else {
                movw(e, W12, in->imm);
            }
            ldrw(e, W9, XC, ROFF(R_ESP));
            subwi(e, W9, W9, 4);
            uint32_t *s1, *s2, *s3;
            emit_page(e, true, &s1, &s2, &s3);
            strw_r(e, W12, W10, W11);
            strw(e, W9, XC, ROFF(R_ESP));
            uint32_t *done = fwd_b(e);
            patch(e, s1);
            patch(e, s2);
            patch(e, s3);
            e->fallback = true;
            emit_call(e, b, i, vm);
            e->fallback = false;
            patch(e, done);
            return true;
        }
    }
    if (f == x_pop_r && r32 && in->reg != R_ESP) {
        ldrw(e, W9, XC, ROFF(R_ESP));
        uint32_t *s1, *s2, *s3;
        emit_page(e, false, &s1, &s2, &s3);
        ldrw_r(e, W12, W10, W11);
        strw(e, W12, XC, ROFF(in->reg));
        addwi(e, W9, W9, 4);
        strw(e, W9, XC, ROFF(R_ESP));
        uint32_t *done = fwd_b(e);
        patch(e, s1);
        patch(e, s2);
        e->fallback = true;
        emit_call(e, b, i, vm);
        e->fallback = false;
        patch(e, done);
        return true;
    }
    emit_call(e, b, i, vm);
    return false;
}

/* Traduce el bloque; false si no hay JIT o no cupo. */
static bool jit_compile(Cpu *c, Block *b)
{
    if (!jit_init() || !c->vm->code || !b->n) {
        return false;
    }
    jit_begin_write();
    for (int attempt = 0; attempt < 2; attempt++) {
        b->slow = 0;
        Em e;
        /* primero las Insn (las funciones de C las reciben) y luego el código */
        size_t ibytes = ((size_t)b->n * sizeof(Insn) + 15) & ~(size_t)15;
        if (g_jit.used + ibytes + 4096 > g_jit.size) {
            g_jit.used = 0;
            g_jit.epoch++;
            jit_table_clear();
            memset(g_jcnt, 0, sizeof(g_jcnt));
            g_jcnt_next = 1;
        }
        size_t data_off = g_jit.used;
        memcpy(g_jit.rw + data_off, b->in, (size_t)b->n * sizeof(Insn));
        e.insn_rx = (const Insn *)(g_jit.rx + data_off);
        e.p = (uint32_t *)(g_jit.rw + data_off + ibytes);
        e.end = (uint32_t *)(g_jit.rw + g_jit.size);
        e.nexits = 0;
        e.full = false;
        e.fallback = false;
        e.lf_kind = 0;
        uint32_t *start = e.p;
        Slow bx = {{0}, 0}; /* no se puede entrar: vuelve a run_blocks */
        /* prólogo: x19 = c, x20 = ram, x21 = tabla de páginas, x22 = code[],
         * x23 = presupuesto que queda, x24 = instrucciones de los bloques encadenados */
        E(&e, 0xA9BC7BFDu); /* stp x29, x30, [sp, #-64]! */
        E(&e, 0xA90153F3u); /* stp x19, x20, [sp, #16] */
        E(&e, 0xA9025BF5u); /* stp x21, x22, [sp, #32] */
        E(&e, 0xA90363F7u); /* stp x23, x24, [sp, #48] */
        movx_r(&e, XC, X0);
        movx_r(&e, 23, X1);
        movx(&e, 24, 0);
        regs_load(&e);
        ldrx(&e, W9, XC, OFF(vm));
        ldrx(&e, XRAM, W9, (uint32_t)offsetof(Vm, ram));
        ldrx(&e, XPT, W9, (uint32_t)offsetof(Vm, pt));
        ldrx(&e, XCODE, W9, (uint32_t)offsetof(Vm, code));
        /* entrada desde otro bloque */
        uint32_t *chain = e.p;
        E(&e, 0xF100001Fu | b->n << 10 | 23u << 5); /* cmp x23, #n */
        slow_bcond(&e, &bx, 3);                      /* b.lo: no alcanza */
        E(&e, 0xD1000000u | b->n << 10 | 23u << 5 | 23u); /* sub x23, x23, #n */
        ldrx(&e, W9, XC, OFF(yield));
        ldrb_i(&e, W9, W9, 0);
        slow_add(&e, &bx, fwd_cb(&e, J_CBNZ, W9));   /* una interrupción */
        movw(&e, W9, b->eip >> 12);
        ldrw_lsl2(&e, W10, XPT, W9);
        movw(&e, W11, b->ppage + 1);
        cmpw(&e, W10, W11);
        slow_bcond(&e, &bx, COND_NE);                /* la página ya es otra */
        movx(&e, W9, (uint64_t)(uintptr_t)&c->vm->gen[b->ppage]);
        ldrw(&e, W10, W9, 0);
        movw(&e, W11, b->gen);
        cmpw(&e, W10, W11);
        slow_bcond(&e, &bx, COND_NE);                /* el código cambió */
        uint32_t cnt = g_jcnt_next < JCNT ? g_jcnt_next++ : 0;
        if (cnt) {
            movx(&e, W9, (uint64_t)(uintptr_t)&g_jcnt[cnt]);
            ldrw(&e, W10, W9, 0);
            addwi(&e, W10, W10, 1);
            strw(&e, W10, W9, 0);
        }
        bool stale = false;
        for (unsigned i = 0; i < b->n; i++) {
            stale = emit_insn(&e, b, i, c->vm);
        }
        if (stale) {
            movw(&e, W9, b->in[b->n - 1].next);
            strw(&e, W9, XC, OFF(eip));
        }
        /* terminó: el siguiente en la tabla */
        E(&e, 0x91000000u | b->n << 10 | 24u << 5 | 24u); /* add x24, x24, #n */
        ldrw(&e, W9, XC, OFF(eip));
        E(&e, 0x4A49412Au);                          /* eor w10, w9, w9, lsr #16 */
        E(&e, 0x12003D4Au);                          /* and w10, w10, #0xFFFF */
        movx(&e, W11, (uint64_t)(uintptr_t)g_jtab);
        E(&e, 0x8B0A116Bu);                          /* add x11, x11, x10, lsl #4 */
        ldrw(&e, W12, W11, 0);
        cmpw(&e, W12, W9);
        uint32_t *miss1 = fwd_bcond(&e, COND_NE);
        ldrx(&e, W13, W11, 8);
        uint32_t *miss2 = fwd_cb(&e, J_CBZ | 0x80000000u, W13); /* cbz x13 */
        E(&e, 0xD61F0000u | 13u << 5);               /* br x13 */
        /* no se pudo entrar: el EIP es el de este bloque y nada hecho */
        for (unsigned k = 0; k < bx.n; k++) {
            patch(&e, bx.at[k]);
        }
        movw(&e, W9, b->eip);
        strw(&e, W9, XC, OFF(eip));
        patch(&e, miss1);
        patch(&e, miss2);
        movw(&e, X0, 0);
        uint32_t *epi = e.p;
        regs_spill(&e);
        E(&e, 0x0B180000u); /* add w0, w0, w24 */
        E(&e, 0xA94363F7u); /* ldp x23, x24, [sp, #48] */
        E(&e, 0xA9425BF5u); /* ldp x21, x22, [sp, #32] */
        E(&e, 0xA94153F3u); /* ldp x19, x20, [sp, #16] */
        E(&e, 0xA8C47BFDu); /* ldp x29, x30, [sp], #64 */
        E(&e, 0xD65F03C0u); /* ret */
        if (e.full) {
            /* sin espacio: se vacía todo y se intenta otra vez */
            g_jit.used = 0;
            g_jit.epoch++;
            jit_table_clear();
            memset(g_jcnt, 0, sizeof(g_jcnt));
            g_jcnt_next = 1;
            continue;
        }
        for (unsigned k = 0; k < e.nexits; k++) {
            *e.exits[k] |= (uint32_t)(epi - e.exits[k]) & 0x3FFFFFFu;
        }
        size_t off = (size_t)((uint8_t *)start - g_jit.rw), len = (size_t)((uint8_t *)e.p - (uint8_t *)start);
        jit_publish(data_off, off + len - data_off);
        g_jit.used = (off + len + 15) & ~(size_t)15;
        b->jit = (void *)(g_jit.rx + off);
        b->jit_chain = (void *)(g_jit.rx + off + (size_t)((uint8_t *)chain - (uint8_t *)start));
        b->jcnt = cnt;
        b->jit_epoch = g_jit.epoch;
        g_jit.compiled++;
        return true;
    }
    jit_publish(0, 0);
    return false;
}
