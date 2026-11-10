/*
 * XboxNX — etapa 2d.3: más instrucciones en código nativo ARM64.
 * Incluido en cpu_jit.inc (antes de emit_insn).
 *
 *  - x87 (D8-DF): cargar, guardar, aritmética, fxch, fchs, fabs, fld1, fldz,
 *    fild, fist/fistp con el redondeo del cw, fldcw / fnstcw, comparaciones
 *    y fnstsw ax. Los registros siguen en c->fp.st (double) con top.
 *  - SSE: movups/movaps/movss, add/sub/mul/div/min/max (ps y ss), and/andn/
 *    or/xor, unpck, shufps, comiss/ucomiss, cvttss2si, prefetch (nada).
 *  - inc/dec, shl/shr/sar de 32 bits con flags perezosos, jmp/call/ret y el
 *    jcc tras inc/dec/desplazamientos.
 *
 * Lo raro (un NaN que x86 da distinto, un resultado fuera del rango de float
 * con la precisión de 24 bits, el modo de redondeo distinto del más cercano)
 * salta antes de cambiar nada a la función de C de siempre, que hace la
 * instrucción entera: el resultado es siempre el del intérprete.
 */

/* ---------------------------------------------------------- más ARM64 */

enum { W14 = 14, W15 = 15 };

static void ldrs_r(Em *e, int vt, int rn, int rm) { E(e, 0xBC606800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)vt); }
static void strs_r(Em *e, int vt, int rn, int rm) { E(e, 0xBC206800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)vt); }
static void ldrq_r(Em *e, int vt, int rn, int rm) { E(e, 0x3CE06800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)vt); }
static void strq_r(Em *e, int vt, int rn, int rm) { E(e, 0x3CA06800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)vt); }
static void ldrs_i(Em *e, int vt, int rn, uint32_t off) { E(e, 0xBD400000u | ((off / 4) << 10) | (uint32_t)rn << 5 | (uint32_t)vt); }
static void strs_i(Em *e, int vt, int rn, uint32_t off) { E(e, 0xBD000000u | ((off / 4) << 10) | (uint32_t)rn << 5 | (uint32_t)vt); }
static void ldrq_i(Em *e, int vt, int rn, uint32_t off) { E(e, 0x3DC00000u | ((off / 16) << 10) | (uint32_t)rn << 5 | (uint32_t)vt); }
static void strq_i(Em *e, int vt, int rn, uint32_t off) { E(e, 0x3D800000u | ((off / 16) << 10) | (uint32_t)rn << 5 | (uint32_t)vt); }
static void ldrx_r(Em *e, int rt, int rn, int rm) { E(e, 0xF8606800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt); }
static void strx_r(Em *e, int rt, int rn, int rm) { E(e, 0xF8206800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt); }
static void addxi(Em *e, int rd, int rn, uint32_t imm) { E(e, 0x91000000u | imm << 10 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void andw7(Em *e, int rd, int rn) { E(e, 0x12000800u | (uint32_t)rn << 5 | (uint32_t)rd); }
static void cset(Em *e, int rd, int cond) { E(e, 0x1A9F07E0u | (uint32_t)(cond ^ 1) << 12 | (uint32_t)rd); }
static void lslvw(Em *e, int rd, int rn, int rm) { E(e, 0x1AC02000u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void lslw(Em *e, int rd, int rn, unsigned s) { E(e, 0x53000000u | ((32u - s) & 31u) << 16 | (31u - s) << 10 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void movw_r(Em *e, int rd, int rm) { E(e, 0x2A0003E0u | (uint32_t)rm << 16 | (uint32_t)rd); }
#define BICW 0x0A200000u
/* add xd, xn, wm, uxtw #3 */
static void add_uxtw3(Em *e, int rd, int rn, int rm) { E(e, 0x8B204C00u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd); }
static void fop(Em *e, uint32_t op, int d, int n, int m) { E(e, op | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d); }
#define FADDD 0x1E602800u
#define FSUBD 0x1E603800u
#define FMULD 0x1E600800u
#define FDIVD 0x1E601800u
#define FADDS 0x1E202800u
#define FSUBS 0x1E203800u
#define FMULS 0x1E200800u
#define FDIVS 0x1E201800u
#define VFADD 0x4E20D400u
#define VFSUB 0x4EA0D400u
#define VFMUL 0x6E20DC00u
#define VFDIV 0x6E20FC00u
#define VFCMGT 0x6EA0E400u
#define VFCMEQ 0x4E20E400u
#define VAND 0x4E201C00u
#define VBIC 0x4E601C00u
#define VORR 0x4EA01C00u
#define VEOR 0x6E201C00u
#define VBSL 0x6E601C00u
#define VZIP1S 0x4E803800u
#define VZIP2S 0x4E807800u
static void fcmpd(Em *e, int n, int m) { E(e, 0x1E602000u | (uint32_t)m << 16 | (uint32_t)n << 5); }
static void fcmps(Em *e, int n, int m) { E(e, 0x1E202000u | (uint32_t)m << 16 | (uint32_t)n << 5); }
static void fcmpd0(Em *e, int n) { E(e, 0x1E602008u | (uint32_t)n << 5); }
static void fcvt_ds(Em *e, int d, int s) { E(e, 0x1E22C000u | (uint32_t)s << 5 | (uint32_t)d); } /* d <- s */
static void fcvt_sd(Em *e, int s, int d) { E(e, 0x1E624000u | (uint32_t)d << 5 | (uint32_t)s); } /* s <- d */
static void fmov_dx(Em *e, int d, int x) { E(e, 0x9E670000u | (uint32_t)x << 5 | (uint32_t)d); }
static void fmov_ws(Em *e, int w, int s) { E(e, 0x1E260000u | (uint32_t)s << 5 | (uint32_t)w); }
static void fabsd(Em *e, int d, int n) { E(e, 0x1E60C000u | (uint32_t)n << 5 | (uint32_t)d); }
static void fnegd(Em *e, int d, int n) { E(e, 0x1E614000u | (uint32_t)n << 5 | (uint32_t)d); }
static void ins_s(Em *e, int vd, unsigned di, int vn, unsigned si)
{
    E(e, 0x6E000400u | ((di << 3) | 4u) << 16 | (si << 2) << 11 | (uint32_t)vn << 5 | (uint32_t)vd);
}
/* b.cond hacia adelante a una lista de saltos lentos */
typedef struct Slow {
    uint32_t *at[16];
    unsigned n;
} Slow;
static void slow_add(Em *e, Slow *s, uint32_t *at)
{
    if (s->n < 16) {
        s->at[s->n++] = at;
    } else {
        e->full = true; /* no debería pasar */
    }
}
static void slow_bcond(Em *e, Slow *s, int cond) { slow_add(e, s, fwd_bcond(e, cond)); }
#define COND_MI 4
#define COND_PL 5
#define COND_VS 6
#define COND_VC 7
#define COND_GE 10
#define COND_LT 11
#define COND_GT 12
#define COND_LE 13

/* Termina una instrucción con camino lento: la rama rápida salta al final,
 * las lentas llaman a la función de siempre. */
static void slow_tail(Em *e, const Block *b, unsigned i, const Vm *vm, Slow *s)
{
    if (!s->n) {
        return;
    }
    uint32_t *done = fwd_b(e);
    for (unsigned k = 0; k < s->n; k++) {
        patch(e, s->at[k]);
    }
    e->fallback = true;
    emit_call(e, b, i, vm);
    e->fallback = false;
    patch(e, done);
}

/* La dirección del operando de memoria de n bytes: x10 + x11 (o al lento). */
static void mem_at(Em *e, const Insn *in, unsigned n, bool write, Slow *s)
{
    uint32_t *s1, *s2, *s3;
    emit_ea(e, in, true);
    emit_page_n(e, write, n, &s1, &s2, &s3);
    slow_add(e, s, s1);
    slow_add(e, s, s2);
    if (s3) {
        slow_add(e, s, s3);
    }
}

/* ---------------------------------------------------------- x87 */

#define STOFF ((uint32_t)offsetof(Cpu, fp.st))

/* x13 = &st[(top + k) & 7] - STOFF (se usa con ldrd/strd(…, x13, STOFF)); w12 = el índice */
static void st_addr(Em *e, unsigned k)
{
    ldrb_i(e, W12, XC, OFF(fp.top));
    if (k) {
        addwi(e, W12, W12, k);
    }
    andw7(e, W12, W12);
    add_uxtw3(e, W13, XC, W12);
}
static void st_load(Em *e, int vd, unsigned k)
{
    st_addr(e, k);
    ldrd(e, vd, W13, STOFF);
}
static void st_store(Em *e, int vs, unsigned k)
{
    st_addr(e, k);
    strd(e, vs, W13, STOFF);
}

/* fpush(d<v>) */
static void fpush_d(Em *e, int v)
{
    ldrb_i(e, W12, XC, OFF(fp.top));
    subwi(e, W12, W12, 1);
    andw7(e, W12, W12);
    strb(e, W12, XC, OFF(fp.top));
    add_uxtw3(e, W13, XC, W12);
    strd(e, v, W13, STOFF);
    movw(e, W14, 1);
    lslvw(e, W14, W14, W12);
    ldrb_i(e, W13, XC, OFF(fp.empty));
    rrr(e, BICW, W13, W13, W14);
    strb(e, W13, XC, OFF(fp.empty));
}

static void fpop_n(Em *e)
{
    ldrb_i(e, W12, XC, OFF(fp.top));
    movw(e, W14, 1);
    lslvw(e, W14, W14, W12);
    ldrb_i(e, W13, XC, OFF(fp.empty));
    rrr(e, ORRW, W13, W13, W14);
    strb(e, W13, XC, OFF(fp.empty));
    addwi(e, W12, W12, 1);
    andw7(e, W12, W12);
    strb(e, W12, XC, OFF(fp.top));
}

/* w15 = cw; si el redondeo (RC) no es el más cercano, al lento */
static void cw_rc0(Em *e, Slow *s)
{
    ldrh_i(e, W15, XC, OFF(fpu_cw));
    E(e, 0x7216041Fu | (uint32_t)W15 << 5); /* tst w15, #0xC00 */
    slow_bcond(e, s, COND_NE);
}

/* d2 = d0 op d1 como farith (op del x87: 0 add 1 mul 4 sub 5 subr 6 div 7 divr);
 * NaN al lento; con la precisión de 24 bits (cw en w15) se redondea a float
 * si está en el rango normal de float (si no, al lento). */
static void farith_n(Em *e, unsigned op, Slow *s)
{
    switch (op) {
    case 0: fop(e, FADDD, 2, 0, 1); break;
    case 1: fop(e, FMULD, 2, 0, 1); break;
    case 4: fop(e, FSUBD, 2, 0, 1); break;
    case 5: fop(e, FSUBD, 2, 1, 0); break;
    case 6: fop(e, FDIVD, 2, 0, 1); break;
    default: fop(e, FDIVD, 2, 1, 0); break;
    }
    fcmpd(e, 2, 2);
    slow_bcond(e, s, COND_VS);
    E(e, 0x7218041Fu | (uint32_t)W15 << 5); /* tst w15, #0x300 (PC) */
    uint32_t *full = fwd_bcond(e, COND_NE);
    fcmpd0(e, 2);
    uint32_t *zero = fwd_bcond(e, COND_EQ);
    fabsd(e, 3, 2);
    movx(e, W14, 0x3810000000000000ull); /* 2^-126 */
    fmov_dx(e, 4, W14);
    fcmpd(e, 3, 4);
    slow_bcond(e, s, COND_MI);
    movx(e, W14, 0x47EFFFFFE0000000ull); /* FLT_MAX */
    fmov_dx(e, 4, W14);
    fcmpd(e, 3, 4);
    slow_bcond(e, s, COND_GT);
    fcvt_sd(e, 3, 2);
    fcvt_ds(e, 2, 3);
    patch(e, full);
    patch(e, zero);
}

/* Comparación del x87 de d0 con d1: C3 C2 C0 en fpu_sw */
static void fcompare_n(Em *e)
{
    fcmpd(e, 0, 1);
    cset(e, W10, COND_EQ);
    cset(e, W11, COND_VS);
    cset(e, W12, COND_MI);
    rrr(e, ORRW, W10, W10, W11); /* C3 = igual o sin orden */
    rrr(e, ORRW, W12, W12, W11); /* C0 = menor o sin orden */
    lslw(e, W10, W10, 14);
    lslw(e, W11, W11, 10);
    lslw(e, W12, W12, 8);
    rrr(e, ORRW, W10, W10, W11);
    rrr(e, ORRW, W10, W10, W12);
    ldrh_i(e, W11, XC, OFF(fpu_sw));
    movw(e, W12, 0x4700); /* C0 C1 C2 C3 */
    rrr(e, BICW, W11, W11, W12);
    rrr(e, ORRW, W11, W11, W10);
    strh_i(e, W11, XC, OFF(fpu_sw));
}

/* d0 a entero de bits bits (32, 16 o 64) con el RC del cw (en w15): x12 */
static void fist_n(Em *e, unsigned bits)
{
    /* según RC: fcvtns / fcvtms / fcvtps / fcvtzs x12, d0 */
    lsrw(e, W14, W15, 10);
    E(e, 0x12000400u | (uint32_t)W14 << 5 | (uint32_t)W14); /* and w14, w14, #3 */
    cmpwi(e, W14, 1);
    uint32_t *r1 = fwd_bcond(e, COND_EQ);
    cmpwi(e, W14, 2);
    uint32_t *r2 = fwd_bcond(e, COND_EQ);
    cmpwi(e, W14, 3);
    uint32_t *r3 = fwd_bcond(e, COND_EQ);
    E(e, 0x9E600000u | 0u << 5 | (uint32_t)W12); /* fcvtns */
    uint32_t *d0 = fwd_b(e);
    patch(e, r1);
    E(e, 0x9E700000u | (uint32_t)W12); /* fcvtms */
    uint32_t *d1 = fwd_b(e);
    patch(e, r2);
    E(e, 0x9E680000u | (uint32_t)W12); /* fcvtps */
    uint32_t *d2 = fwd_b(e);
    patch(e, r3);
    E(e, 0x9E780000u | (uint32_t)W12); /* fcvtzs */
    patch(e, d0);
    patch(e, d1);
    patch(e, d2);
    /* fuera de rango o NaN: el entero indefinido */
    if (bits == 64) {
        movx(e, W14, 0x7FFFFFFFFFFFFFFFull);
        E(e, 0xEB0E019Fu); /* cmp x12, x14 */
        uint32_t *ok = fwd_bcond(e, COND_NE);
        movx(e, W12, 0x8000000000000000ull);
        patch(e, ok);
    } else {
        E(e, (bits == 32 ? 0xEB2CC19Fu : 0xEB2CA19Fu)); /* cmp x12, w12, sxtw / sxth */
        uint32_t *ok = fwd_bcond(e, COND_EQ);
        movx(e, W12, bits == 32 ? 0x80000000ull : 0x8000ull);
        patch(e, ok);
    }
    fcmpd(e, 0, 0);
    uint32_t *num = fwd_bcond(e, COND_VC);
    movx(e, W12, bits == 64 ? 0x8000000000000000ull : bits == 32 ? 0x80000000ull : 0x8000ull);
    patch(e, num);
}

static bool emit_x87(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    if (in->fn != x_interp || (in->sz & (INTERP_MARK | INTERP_0F | 7u)) != INTERP_MARK) {
        return false;
    }
    unsigned op = in->aop, reg = in->reg, k = in->rm;
    if (op < 0xD8 || op > 0xDF) {
        return false;
    }
    Slow s = {{0}, 0};
    if (!in->is_reg) {
        switch (op) {
        case 0xD8:
        case 0xDC: { /* st0 op= m32 / m64 */
            if (reg == 2 || reg == 3) {
                mem_at(e, in, op == 0xD8 ? 4 : 8, false, &s);
                if (op == 0xD8) {
                    ldrs_r(e, 1, W10, W11);
                    fcvt_ds(e, 1, 1);
                } else {
                    ldrd_r(e, 1, W10, W11);
                }
                st_load(e, 0, 0);
                fcompare_n(e);
                if (reg == 3) {
                    fpop_n(e);
                }
                break;
            }
            cw_rc0(e, &s);
            mem_at(e, in, op == 0xD8 ? 4 : 8, false, &s);
            if (op == 0xD8) {
                ldrs_r(e, 1, W10, W11);
                fcvt_ds(e, 1, 1);
            } else {
                ldrd_r(e, 1, W10, W11);
            }
            st_load(e, 0, 0);
            farith_n(e, reg, &s);
            strd(e, 2, W13, STOFF); /* x13 sigue en st0 */
            break;
        }
        case 0xD9:
            if (reg == 0) { /* fld m32 */
                mem_at(e, in, 4, false, &s);
                ldrs_r(e, 0, W10, W11);
                fcvt_ds(e, 0, 0);
                fpush_d(e, 0);
            } else if (reg == 2 || reg == 3) { /* fst / fstp m32 */
                cw_rc0(e, &s);
                mem_at(e, in, 4, true, &s);
                st_load(e, 0, 0);
                fcvt_sd(e, 0, 0);
                strs_r(e, 0, W10, W11);
                if (reg == 3) {
                    fpop_n(e);
                }
            } else if (reg == 5) { /* fldcw */
                mem_at(e, in, 2, false, &s);
                ldrh_r(e, W12, W10, W11);
                strh_i(e, W12, XC, OFF(fpu_cw));
            } else if (reg == 7) { /* fnstcw */
                mem_at(e, in, 2, true, &s);
                ldrh_i(e, W12, XC, OFF(fpu_cw));
                strh_r(e, W12, W10, W11);
            } else {
                return false;
            }
            break;
        case 0xDD:
            if (reg == 0) { /* fld m64 */
                mem_at(e, in, 8, false, &s);
                ldrd_r(e, 0, W10, W11);
                fpush_d(e, 0);
            } else if (reg == 2 || reg == 3) { /* fst / fstp m64 */
                mem_at(e, in, 8, true, &s);
                st_load(e, 0, 0);
                strd_r(e, 0, W10, W11);
                if (reg == 3) {
                    fpop_n(e);
                }
            } else {
                return false;
            }
            break;
        case 0xDB:
            if (reg == 0) { /* fild m32 */
                mem_at(e, in, 4, false, &s);
                ldrw_r(e, W12, W10, W11);
                E(e, 0x1E620000u | (uint32_t)W12 << 5 | 0u); /* scvtf d0, w12 */
                fpush_d(e, 0);
            } else if (reg == 2 || reg == 3) { /* fist / fistp m32 */
                ldrh_i(e, W15, XC, OFF(fpu_cw));
                mem_at(e, in, 4, true, &s);
                st_load(e, 0, 0);
                fist_n(e, 32);
                strw_r(e, W12, W10, W11);
                if (reg == 3) {
                    fpop_n(e);
                }
            } else {
                return false;
            }
            break;
        case 0xDF:
            if (reg == 0) { /* fild m16 */
                mem_at(e, in, 2, false, &s);
                ldrh_r(e, W12, W10, W11);
                E(e, 0x13003C00u | (uint32_t)W12 << 5 | (uint32_t)W12); /* sxth */
                E(e, 0x1E620000u | (uint32_t)W12 << 5 | 0u);
                fpush_d(e, 0);
            } else if (reg == 5) { /* fild m64 */
                mem_at(e, in, 8, false, &s);
                ldrx_r(e, W12, W10, W11);
                E(e, 0x9E620000u | (uint32_t)W12 << 5 | 0u); /* scvtf d0, x12 */
                fpush_d(e, 0);
            } else if (reg == 2 || reg == 3 || reg == 7) { /* fist(p) m16, fistp m64 */
                unsigned bits = reg == 7 ? 64 : 16;
                ldrh_i(e, W15, XC, OFF(fpu_cw));
                mem_at(e, in, bits / 8, true, &s);
                st_load(e, 0, 0);
                fist_n(e, bits);
                if (bits == 64) {
                    strx_r(e, W12, W10, W11);
                } else {
                    strh_r(e, W12, W10, W11);
                }
                if (reg != 2) {
                    fpop_n(e);
                }
            } else {
                return false;
            }
            break;
        default:
            return false;
        }
        slow_tail(e, b, i, vm, &s);
        return true;
    }
    /* formas de registro */
    unsigned m = 0xC0u | reg << 3 | k;
    switch (op) {
    case 0xD8: /* st0 = st0 op st(k) */
        st_load(e, 1, k);
        st_load(e, 0, 0);
        if (reg == 2 || reg == 3) {
            fcompare_n(e);
            if (reg == 3) {
                fpop_n(e);
            }
            break;
        }
        cw_rc0(e, &s);
        farith_n(e, reg, &s);
        strd(e, 2, W13, STOFF);
        break;
    case 0xDC:
    case 0xDE: { /* st(k) = st(k) op st0 (sub/div al revés); DE: y pop */
        if (op == 0xDE && m == 0xD9) { /* fcompp */
            st_load(e, 1, 1);
            st_load(e, 0, 0);
            fcompare_n(e);
            fpop_n(e);
            fpop_n(e);
            break;
        }
        if (reg == 2 || reg == 3) {
            if (op != 0xDC) {
                return false;
            }
            st_load(e, 1, k);
            st_load(e, 0, 0);
            fcompare_n(e);
            if (reg == 3) {
                fpop_n(e);
            }
            break;
        }
        static const unsigned swap[8] = {0, 1, 2, 3, 5, 4, 7, 6};
        cw_rc0(e, &s);
        st_load(e, 1, 0);
        st_load(e, 0, k);
        farith_n(e, swap[reg], &s);
        strd(e, 2, W13, STOFF); /* x13 en st(k) */
        if (op == 0xDE) {
            fpop_n(e);
        }
        break;
    }
    case 0xD9:
        if (reg == 0) { /* fld st(k) */
            st_load(e, 0, k);
            fpush_d(e, 0);
        } else if (reg == 1) { /* fxch */
            st_load(e, 1, k);
            st_load(e, 0, 0);
            strd(e, 1, W13, STOFF);
            st_store(e, 0, k);
        } else if (m == 0xE0 || m == 0xE1) { /* fchs / fabs */
            st_load(e, 0, 0);
            if (m == 0xE0) {
                fnegd(e, 0, 0);
            } else {
                fabsd(e, 0, 0);
            }
            strd(e, 0, W13, STOFF);
        } else if (m == 0xE8 || m == 0xEE) { /* fld1 / fldz */
            movx(e, W14, m == 0xE8 ? 0x3FF0000000000000ull : 0);
            fmov_dx(e, 0, W14);
            fpush_d(e, 0);
        } else if (m == 0xE4) { /* ftst */
            st_load(e, 0, 0);
            fmov_dx(e, 1, XZR);
            fcompare_n(e);
        } else if (m == 0xD0) { /* fnop */
        } else {
            return false;
        }
        break;
    case 0xDD:
        if (reg == 2 || reg == 3) { /* fst / fstp st(k) */
            st_load(e, 0, 0);
            st_store(e, 0, k);
            if (reg == 3) {
                fpop_n(e);
            }
        } else if (reg == 4 || reg == 5) { /* fucom / fucomp */
            st_load(e, 1, k);
            st_load(e, 0, 0);
            fcompare_n(e);
            if (reg == 5) {
                fpop_n(e);
            }
        } else {
            return false;
        }
        break;
    case 0xDA:
        if (m == 0xE9) { /* fucompp */
            st_load(e, 1, 1);
            st_load(e, 0, 0);
            fcompare_n(e);
            fpop_n(e);
            fpop_n(e);
            break;
        }
        return false;
    case 0xDF:
        if (m == 0xE0) { /* fnstsw ax */
            ldrh_i(e, W10, XC, OFF(fpu_sw));
            movw(e, W11, 0x3800);
            rrr(e, BICW, W10, W10, W11);
            ldrb_i(e, W11, XC, OFF(fp.top));
            lslw(e, W11, W11, 11);
            rrr(e, ORRW, W10, W10, W11);
            strh_i(e, W10, XC, ROFF(R_EAX));
            break;
        }
        return false;
    default:
        return false;
    }
    slow_tail(e, b, i, vm, &s);
    return true;
}

/* ---------------------------------------------------------- SSE */

/* x14 = &xmm[0] */
static void xmm_base(Em *e) { addxi(e, W14, XC, OFF(fp.xmm)); }

/* NaN en algún carril de v<v>.4s (o en s<v> si scalar): al lento */
static void nan_check(Em *e, int v, bool scalar, Slow *s)
{
    if (scalar) {
        fcmps(e, v, v);
        slow_bcond(e, s, COND_VS);
        return;
    }
    fop(e, VFCMEQ, 7, v, v);
    E(e, 0x6EB1A800u | 7u << 5 | 7u); /* uminv s7, v7.4s */
    fmov_ws(e, W12, 7);
    slow_add(e, s, fwd_cb(e, J_CBZ, W12));
}

static bool emit_sse(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    if (in->fn != x_interp || (in->sz & (INTERP_MARK | INTERP_0F)) != (INTERP_MARK | INTERP_0F)) {
        return false;
    }
    unsigned pfx = in->sz & 7u, op = in->aop, x = in->reg;
    if (pfx & (INTERP_P66 | INTERP_PF2)) {
        return false;
    }
    bool ss = (pfx & INTERP_PF3) != 0;
    Slow s = {{0}, 0};
    uint32_t xo = 16u * x;
    if (op == 0x18 && !ss) { /* prefetch: nada */
        return !in->is_reg;
    }
    switch (op) {
    case 0x10:
    case 0x28: /* movups / movaps / movss carga */
        if (op == 0x28 && ss) {
            return false;
        }
        xmm_base(e);
        if (in->is_reg) {
            if (ss) {
                ldrs_i(e, 0, W14, 16u * in->rm);
                strs_i(e, 0, W14, xo);
            } else {
                ldrq_i(e, 0, W14, 16u * in->rm);
                strq_i(e, 0, W14, xo);
            }
            return true;
        }
        mem_at(e, in, ss ? 4 : 16, false, &s);
        if (ss) {
            ldrs_r(e, 0, W10, W11); /* el resto a cero */
        } else {
            ldrq_r(e, 0, W10, W11);
        }
        xmm_base(e);
        strq_i(e, 0, W14, xo);
        break;
    case 0x11:
    case 0x29:
    case 0x2B: /* movups / movaps / movntps / movss guardar */
        if (ss && op != 0x11) {
            return false;
        }
        if (in->is_reg) {
            if (op == 0x2B) {
                return false;
            }
            xmm_base(e);
            if (ss) {
                ldrs_i(e, 0, W14, xo);
                strs_i(e, 0, W14, 16u * in->rm);
            } else {
                ldrq_i(e, 0, W14, xo);
                strq_i(e, 0, W14, 16u * in->rm);
            }
            return true;
        }
        mem_at(e, in, ss ? 4 : 16, true, &s);
        xmm_base(e);
        if (ss) {
            ldrs_i(e, 0, W14, xo);
            strs_r(e, 0, W10, W11);
        } else {
            ldrq_i(e, 0, W14, xo);
            strq_r(e, 0, W10, W11);
        }
        break;
    case 0x58: case 0x59: case 0x5C: case 0x5E: case 0x5D: case 0x5F: {
        /* v0 = a (destino), v1 = b */
        if (in->is_reg) {
            xmm_base(e);
            if (ss) {
                ldrs_i(e, 1, W14, 16u * in->rm);
            } else {
                ldrq_i(e, 1, W14, 16u * in->rm);
            }
        } else {
            mem_at(e, in, ss ? 4 : 16, false, &s);
            if (ss) {
                ldrs_r(e, 1, W10, W11);
            } else {
                ldrq_r(e, 1, W10, W11);
            }
            xmm_base(e);
        }
        if (ss) {
            ldrs_i(e, 0, W14, xo);
            switch (op) {
            case 0x58: fop(e, FADDS, 2, 0, 1); break;
            case 0x59: fop(e, FMULS, 2, 0, 1); break;
            case 0x5C: fop(e, FSUBS, 2, 0, 1); break;
            case 0x5E: fop(e, FDIVS, 2, 0, 1); break;
            default:
                fcmps(e, 0, 1);
                E(e, 0x1E200C00u | 1u << 16 | (uint32_t)(op == 0x5D ? COND_MI : COND_GT) << 12 | 0u << 5 | 2u); /* fcsel s2, s0, s1 */
                break;
            }
            if (op != 0x5D && op != 0x5F) {
                nan_check(e, 2, true, &s);
            }
            strs_i(e, 2, W14, xo);
        } else {
            ldrq_i(e, 0, W14, xo);
            switch (op) {
            case 0x58: fop(e, VFADD, 2, 0, 1); break;
            case 0x59: fop(e, VFMUL, 2, 0, 1); break;
            case 0x5C: fop(e, VFSUB, 2, 0, 1); break;
            case 0x5E: fop(e, VFDIV, 2, 0, 1); break;
            case 0x5D: /* a < b ? a : b */
                fop(e, VFCMGT, 2, 1, 0);
                fop(e, VBSL, 2, 0, 1);
                break;
            default: /* a > b ? a : b */
                fop(e, VFCMGT, 2, 0, 1);
                fop(e, VBSL, 2, 0, 1);
                break;
            }
            if (op != 0x5D && op != 0x5F) {
                nan_check(e, 2, false, &s);
            }
            strq_i(e, 2, W14, xo);
        }
        break;
    }
    case 0x54: case 0x55: case 0x56: case 0x57: case 0x14: case 0x15: case 0xC6: {
        if (ss) {
            return false;
        }
        if (in->is_reg) {
            xmm_base(e);
            ldrq_i(e, 1, W14, 16u * in->rm);
        } else {
            mem_at(e, in, 16, false, &s);
            ldrq_r(e, 1, W10, W11);
            xmm_base(e);
        }
        ldrq_i(e, 0, W14, xo);
        switch (op) {
        case 0x54: fop(e, VAND, 2, 0, 1); break;
        case 0x55: fop(e, VBIC, 2, 1, 0); break; /* ~a & b */
        case 0x56: fop(e, VORR, 2, 0, 1); break;
        case 0x57: fop(e, VEOR, 2, 0, 1); break;
        case 0x14: fop(e, VZIP1S, 2, 0, 1); break;
        case 0x15: fop(e, VZIP2S, 2, 0, 1); break;
        default: { /* shufps */
            unsigned imm = in->imm & 0xFF;
            ins_s(e, 2, 0, 0, imm & 3);
            ins_s(e, 2, 1, 0, (imm >> 2) & 3);
            ins_s(e, 2, 2, 1, (imm >> 4) & 3);
            ins_s(e, 2, 3, 1, (imm >> 6) & 3);
            break;
        }
        }
        strq_i(e, 2, W14, xo);
        break;
    }
    case 0x2E:
    case 0x2F: { /* ucomiss / comiss: ZF PF CF, OF SF AF a cero */
        if (ss) {
            return false;
        }
        xmm_base(e);
        if (in->is_reg) {
            ldrs_i(e, 1, W14, 16u * in->rm);
        } else {
            mem_at(e, in, 4, false, &s);
            ldrs_r(e, 1, W10, W11);
            xmm_base(e);
        }
        ldrs_i(e, 0, W14, xo);
        fcmps(e, 0, 1);
        /* sin orden: ZF PF CF; menor: CF; igual: ZF */
        cset(e, W10, COND_VS);
        cset(e, W11, COND_EQ);
        cset(e, W12, COND_MI);
        rrr(e, ORRW, W11, W11, W10);           /* ZF */
        rrr(e, ORRW, W12, W12, W10);           /* CF */
        lslw(e, W10, W10, 2);                  /* PF */
        lslw(e, W11, W11, 6);
        rrr(e, ORRW, W10, W10, W11);
        rrr(e, ORRW, W10, W10, W12);
        ldrw(e, W11, XC, OFF(flags));
        movw(e, W12, F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF);
        rrr(e, BICW, W11, W11, W12);
        rrr(e, ORRW, W11, W11, W10);
        strw(e, W11, XC, OFF(flags));
        strb(e, XZR, XC, OFF(lf_op));
        break;
    }
    case 0x2C: { /* cvttss2si r32, xmm/m32 */
        if (!ss) {
            return false;
        }
        if (in->is_reg) {
            xmm_base(e);
            ldrs_i(e, 0, W14, 16u * in->rm);
        } else {
            mem_at(e, in, 4, false, &s);
            ldrs_r(e, 0, W10, W11);
        }
        E(e, 0x1E380000u | 0u << 5 | (uint32_t)W12); /* fcvtzs w12, s0 */
        movw(e, W13, 0x80000000u);
        movw(e, W11, 0x7FFFFFFFu);
        cmpw(e, W12, W11);
        E(e, 0x1A800000u | (uint32_t)W12 << 16 | (uint32_t)COND_EQ << 12 | (uint32_t)W13 << 5 | (uint32_t)W12); /* csel w12, w13, w12, eq */
        fcmps(e, 0, 0);
        E(e, 0x1A800000u | (uint32_t)W12 << 16 | (uint32_t)COND_VS << 12 | (uint32_t)W13 << 5 | (uint32_t)W12); /* NaN */
        strw(e, W12, XC, ROFF(x));
        break;
    }
    default:
        return false;
    }
    slow_tail(e, b, i, vm, &s);
    return true;
}

/* ---------------------------------------------------------- enteros */

static uint32_t jit_lazy_cf(Cpu *c) { return lazy_cf(c); }

/* w12 = el CF de lo anterior (para inc/dec) */
static void emit_cf(Em *e, unsigned prev)
{
    switch (prev) {
    case LF_SUB:
        ldrw(e, W9, XC, OFF(lf_a));
        ldrw(e, W10, XC, OFF(lf_b));
        cmpw(e, W9, W10);
        cset(e, W12, 3); /* lo (carry clear = borrow) */
        return;
    case LF_ADD:
        ldrw(e, W9, XC, OFF(lf_a));
        ldrw(e, W10, XC, OFF(lf_b));
        E(e, 0x2B0A013Fu); /* cmn w9, w10 */
        cset(e, W12, 2); /* cs */
        return;
    case LF_LOGIC:
        movw(e, W12, 0);
        return;
    case LF_INC:
    case LF_DEC:
        ldrb_i(e, W12, XC, OFF(lf_cin));
        return;
    default:
        movx_r(e, X0, XC);
        movx(e, W9, (uint64_t)(uintptr_t)jit_lazy_cf);
        E(e, 0xD63F0000u | 9u << 5); /* blr x9 */
        movw_r(e, W12, X0);
        return;
    }
}

/* lf_* de inc/dec/desplazamiento: w9 = a, w10 = b, w11 = res, w12 = cin */
static void emit_lf_cin(Em *e, unsigned kind)
{
    e->lf_kind = kind;
    strb(e, W12, XC, OFF(lf_cin));
    movw(e, W12, kind);
    strb(e, W12, XC, OFF(lf_op));
    movw(e, W12, 4);
    strb(e, W12, XC, OFF(lf_sz));
    strw(e, W9, XC, OFF(lf_a));
    strw(e, W10, XC, OFF(lf_b));
    strw(e, W11, XC, OFF(lf_res));
}

/* jcc tras inc/dec (sin CF ni PF) o tras un desplazamiento (solo ZF y SF) */
static bool emit_jcc_more(Em *e, const Insn *in, unsigned prev)
{
    static const int sub[16] = {6, 7, -1, -1, 0, 1, -1, -1, 4, 5, -1, -1, 11, 10, 13, 12};
    unsigned c = in->aop & 15;
    int cc = sub[c];
    if (cc < 0) {
        return false;
    }
    if (prev == LF_SHL || prev == LF_SHR || prev == LF_SAR) {
        if (c != 4 && c != 5 && c != 8 && c != 9) {
            return false;
        }
        ldrw(e, W11, XC, OFF(lf_res));
        E(e, 0x6A0B017Fu); /* tst w11, w11 */
    } else if (prev == LF_DEC) {
        ldrw(e, W9, XC, OFF(lf_a));
        cmpwi(e, W9, 1);
    } else if (prev == LF_INC) {
        ldrw(e, W9, XC, OFF(lf_a));
        E(e, 0x3100041Fu | (uint32_t)W9 << 5); /* cmn w9, #1 */
    } else {
        return false;
    }
    movw(e, W12, in->next);
    movw(e, W13, in->imm);
    E(e, 0x1A800000u | (uint32_t)W12 << 16 | (uint32_t)cc << 12 | (uint32_t)W13 << 5 | (uint32_t)W12); /* csel */
    strw(e, W12, XC, OFF(eip));
    return true;
}

/* Las enteras nuevas; *ret: lo que devuelve emit_insn (false: puso el EIP). */
static bool emit_int(Em *e, const Block *b, unsigned i, const Vm *vm, unsigned prev, bool *ret)
{
    const Insn *in = &b->in[i];
    InsnFn f = in->fn;
    *ret = true;
    if (f == x_incdec && in->sz == 4 && in->is_reg) {
        emit_cf(e, prev);
        ldrw(e, W9, XC, ROFF(in->rm));
        movw(e, W10, 1);
        if (in->aop) {
            subwi(e, W11, W9, 1);
        } else {
            addwi(e, W11, W9, 1);
        }
        strw(e, W11, XC, ROFF(in->rm));
        emit_lf_cin(e, in->aop ? LF_DEC : LF_INC);
        return true;
    }
    if (f == x_shift && in->sz == 4 && in->is_reg && in->imm != 0xFF && (in->aop == 4 || in->aop == 5 ||
                                                                         in->aop == 6 || in->aop == 7)) {
        unsigned cnt = in->imm & 31;
        if (!cnt) {
            return false;
        }
        ldrw(e, W9, XC, ROFF(in->rm));
        if (in->aop == 5) {
            E(e, 0x53007C00u | cnt << 16 | (uint32_t)W9 << 5 | (uint32_t)W11); /* lsr */
        } else if (in->aop == 7) {
            E(e, 0x13007C00u | cnt << 16 | (uint32_t)W9 << 5 | (uint32_t)W11); /* asr */
        } else {
            lslw(e, W11, W9, cnt);
        }
        strw(e, W11, XC, ROFF(in->rm));
        movw(e, W10, cnt);
        movw(e, W12, 0);
        emit_lf_cin(e, in->aop == 5 ? LF_SHR : in->aop == 7 ? LF_SAR : LF_SHL);
        return true;
    }
    if (f == x_jcc && (prev == LF_INC || prev == LF_DEC || prev == LF_SHL || prev == LF_SHR || prev == LF_SAR)) {
        if (emit_jcc_more(e, in, prev)) {
            *ret = false;
            return true;
        }
        return false;
    }
    if (f == x_jmp) {
        movw(e, W12, in->imm);
        strw(e, W12, XC, OFF(eip));
        *ret = false;
        return true;
    }
    if (f == x_call || (f == x_ret && in->imm < 4092)) {
        uint32_t *s1, *s2, *s3;
        ldrw(e, W9, XC, ROFF(R_ESP));
        if (f == x_call) {
            subwi(e, W9, W9, 4);
            emit_page(e, true, &s1, &s2, &s3);
            movw(e, W12, in->next);
            strw_r(e, W12, W10, W11);
            strw(e, W9, XC, ROFF(R_ESP));
            movw(e, W12, in->imm);
            strw(e, W12, XC, OFF(eip));
        } else {
            emit_page(e, false, &s1, &s2, &s3);
            ldrw_r(e, W12, W10, W11);
            addwi(e, W9, W9, 4 + in->imm);
            strw(e, W9, XC, ROFF(R_ESP));
            strw(e, W12, XC, OFF(eip));
        }
        uint32_t *done = fwd_b(e);
        fallback_tail(e, b, i, vm, done, s1, s2, s3);
        *ret = false;
        return true;
    }
    return false;
}
