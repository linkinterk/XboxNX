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

typedef uint32_t (*JitFn)(Cpu *c);

#define JIT_SIZE_MAX (32u << 20)

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
    uint32_t *exits[512]; /* "b epílogo" por parchear */
    unsigned nexits;
    bool full;
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

static void ldrw(Em *e, int rt, int rn, uint32_t off) { E(e, 0xB9400000u | ((off / 4) << 10) | (uint32_t)rn << 5 | (uint32_t)rt); }
static void strw(Em *e, int rt, int rn, uint32_t off) { E(e, 0xB9000000u | ((off / 4) << 10) | (uint32_t)rn << 5 | (uint32_t)rt); }
static void ldrx(Em *e, int rt, int rn, uint32_t off) { E(e, 0xF9400000u | ((off / 8) << 10) | (uint32_t)rn << 5 | (uint32_t)rt); }
static void strb(Em *e, int rt, int rn, uint32_t off) { E(e, 0x39000000u | (off << 10) | (uint32_t)rn << 5 | (uint32_t)rt); }
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
    /* el intérprete (rdtsc, por ejemplo) ve icount como si fuera de una en una */
    bool icnt = in->fn == x_interp && i > 0;
    if (icnt) {
        ldrx(e, W9, XC, OFF(icount));
        E(e, 0x91000000u | i << 10 | 9u << 5 | 9u); /* add x9, x9, #i */
        E(e, 0xF9000000u | (OFF(icount) / 8) << 10 | (uint32_t)XC << 5 | 9u); /* str x9 */
    }
    movw(e, W9, in->next);
    strw(e, W9, XC, OFF(eip));
    movx_r(e, X0, XC);
    movx(e, X1, (uint64_t)(uintptr_t)in);
    movx(e, W9, (uint64_t)(uintptr_t)in->fn);
    E(e, 0xD63F0000u | 9u << 5); /* blr x9 */
    if (icnt) {
        ldrx(e, W9, XC, OFF(icount));
        E(e, 0xD1000000u | i << 10 | 9u << 5 | 9u); /* sub x9, x9, #i */
        E(e, 0xF9000000u | (OFF(icount) / 8) << 10 | (uint32_t)XC << 5 | 9u);
    }
    /* paró: se hicieron i */
    ldrw(e, W9, XC, OFF(stop));
    uint32_t *ok = fwd_cb(e, J_CBZ, W9);
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
    emit_call(e, b, i, vm);
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
    emit_call(e, b, i, vm);
    patch(e, done);
}

/* Una instrucción; true si puede quedar en código nativo (EIP sin poner;
 * su camino lento pone el mismo next). false: la función, que pone el EIP. */
static bool emit_insn(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    InsnFn f = in->fn;
    bool r32 = in->sz == 4;
    if (f == x_nop) {
        return true;
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
            emit_call(e, b, i, vm);
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
        emit_call(e, b, i, vm);
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
        Em e;
        e.p = (uint32_t *)(g_jit.rw + g_jit.used);
        e.end = (uint32_t *)(g_jit.rw + g_jit.size);
        e.nexits = 0;
        e.full = false;
        uint32_t *start = e.p;
        /* prólogo: x19 = c, x20 = ram, x21 = tabla de páginas, x22 = code[] */
        E(&e, 0xA9BD7BFDu); /* stp x29, x30, [sp, #-48]! */
        E(&e, 0xA90153F3u); /* stp x19, x20, [sp, #16] */
        E(&e, 0xA9025BF5u); /* stp x21, x22, [sp, #32] */
        movx_r(&e, XC, X0);
        ldrx(&e, W9, XC, OFF(vm));
        ldrx(&e, XRAM, W9, (uint32_t)offsetof(Vm, ram));
        ldrx(&e, XPT, W9, (uint32_t)offsetof(Vm, pt));
        ldrx(&e, XCODE, W9, (uint32_t)offsetof(Vm, code));
        bool stale = false;
        for (unsigned i = 0; i < b->n; i++) {
            stale = emit_insn(&e, b, i, c->vm);
        }
        if (stale) {
            movw(&e, W9, b->in[b->n - 1].next);
            strw(&e, W9, XC, OFF(eip));
        }
        movw(&e, X0, b->n);
        uint32_t *epi = e.p;
        E(&e, 0xA9425BF5u); /* ldp x21, x22, [sp, #32] */
        E(&e, 0xA94153F3u); /* ldp x19, x20, [sp, #16] */
        E(&e, 0xA8C37BFDu); /* ldp x29, x30, [sp], #48 */
        E(&e, 0xD65F03C0u); /* ret */
        if (e.full) {
            /* sin espacio: se vacía todo y se intenta otra vez */
            g_jit.used = 0;
            g_jit.epoch++;
            continue;
        }
        for (unsigned k = 0; k < e.nexits; k++) {
            *e.exits[k] |= (uint32_t)(epi - e.exits[k]) & 0x3FFFFFFu;
        }
        size_t off = (size_t)((uint8_t *)start - g_jit.rw), len = (size_t)((uint8_t *)e.p - (uint8_t *)start);
        jit_publish(off, len);
        g_jit.used = (off + len + 15) & ~(size_t)15;
        b->jit = (void *)(g_jit.rx + off);
        b->jit_epoch = g_jit.epoch;
        g_jit.compiled++;
        return true;
    }
    jit_publish(0, 0);
    return false;
}
