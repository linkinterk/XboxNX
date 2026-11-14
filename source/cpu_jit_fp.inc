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
    slow_cold(e, b, i, vm, s->at, s->n);
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
    case 0x12: case 0x13: case 0x16: case 0x17: { /* movlps movhlps / movhps movlhps */
        if (ss) {
            return false;
        }
        bool high = op >= 0x16;
        uint32_t half = xo + (high ? 8u : 0u);
        if (op == 0x13 || op == 0x17) {
            if (in->is_reg) {
                return true; /* con registro no hace nada (como el intérprete) */
            }
            mem_at(e, in, 8, true, &s);
            xmm_base(e);
            ldrd(e, 0, W14, half);
            strd_r(e, 0, W10, W11);
            break;
        }
        if (in->is_reg) {
            xmm_base(e);
            ldrd(e, 0, W14, 16u * in->rm + (high ? 0u : 8u));
            strd(e, 0, W14, half);
            return true;
        }
        mem_at(e, in, 8, false, &s);
        ldrd_r(e, 0, W10, W11);
        xmm_base(e);
        strd(e, 0, W14, half);
        break;
    }
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
        regs_spill(e);
        E(e, 0xD63F0000u | 9u << 5); /* blr x9 */
        regs_load(e);
        movw_r(e, W12, X0);
        return;
    }
}

/* lf_* de inc/dec/desplazamiento: w9 = a, w10 = b, w11 = res, w12 = cin */
static void emit_lf_cin(Em *e, unsigned kind)
{
    e->lf_kind = kind;
    if (e->lf_dead) {
        return;
    }
    /* lf_op, lf_sz = 4 y lf_cin (w12) de una vez */
    movw(e, 14, kind | 4u << 8);
    E(e, 0x2A000000u | (uint32_t)W12 << 16 | 16u << 10 | 14u << 5 | 14u); /* orr w14, w14, w12, lsl #16 */
    strw(e, 14, XC, OFF(lf_op));
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

/* ---------------------------------------------------------- más enteras (v0.18.37) */

/* lf_* completos: w9 = a, w10 = b, w11 = res; cin en el registro cin (o 0 si -1) */
static void emit_lf_full(Em *e, unsigned kind, unsigned sz, int cin)
{
    if (e->lf_dead) {
        e->lf_kind = (sz == 4 && cin < 0 && kind != LF_MUL) ? kind : 0;
        return;
    }
    if (cin < 0) {
        movw(e, W12, kind | sz << 8); /* lf_op, lf_sz, lf_cin = 0 de una vez */
        strw(e, W12, XC, OFF(lf_op));
    } else {
        movw(e, W12, kind);
        strb(e, W12, XC, OFF(lf_op));
        movw(e, W12, sz);
        strb(e, W12, XC, OFF(lf_sz));
        strb(e, cin, XC, OFF(lf_cin));
    }
    strw(e, W9, XC, OFF(lf_a));
    strw(e, W10, XC, OFF(lf_b));
    strw(e, W11, XC, OFF(lf_res));
    /* los saltos nativos solo saben de 32 bits sin acarreo */
    e->lf_kind = (sz == 4 && cin < 0 && kind != LF_MUL) ? kind : 0;
}

/* un operando de sz bytes: de [x13] (mem) o del registro r */
static void opnd_ld(Em *e, int rt, bool mem, unsigned r, unsigned sz)
{
    if (mem) {
        if (sz == 1) {
            ldrb_i(e, rt, W13, 0);
        } else if (sz == 2) {
            ldrh_i(e, rt, W13, 0);
        } else {
            ldrw(e, rt, W13, 0);
        }
    } else if (sz == 1) {
        ldrb_i(e, rt, XC, r8off(r));
    } else if (sz == 2) {
        ldrh_i(e, rt, XC, ROFF(r));
    } else {
        ldrw(e, rt, XC, ROFF(r));
    }
}
static void opnd_st(Em *e, int rt, bool mem, unsigned r, unsigned sz)
{
    if (mem) {
        if (sz == 1) {
            strb(e, rt, W13, 0);
        } else if (sz == 2) {
            strh_i(e, rt, W13, 0);
        } else {
            strw(e, rt, W13, 0);
        }
    } else if (sz == 1) {
        strb(e, rt, XC, r8off(r));
    } else if (sz == 2) {
        strh_i(e, rt, XC, ROFF(r));
    } else {
        strw(e, rt, XC, ROFF(r));
    }
}
static void zext_sz(Em *e, int rd, unsigned sz)
{
    if (sz == 1) {
        E(e, 0x53001C00u | (uint32_t)rd << 5 | (uint32_t)rd); /* uxtb */
    } else if (sz == 2) {
        E(e, 0x53003C00u | (uint32_t)rd << 5 | (uint32_t)rd); /* uxth */
    }
}
/* x13 = la dirección en el host del operando de memoria de n bytes (o al lento) */
static void mem_x13(Em *e, const Insn *in, unsigned n, bool write, Slow *sl)
{
    mem_at(e, in, n, write, sl);
    E(e, 0x8B0B014Du); /* add x13, x10, x11 */
}

/* alu de 8/16/32 bits (también adc/sbb y con memoria), test */
static bool emit_alu_any(Em *e, const Block *b, unsigned i, const Vm *vm, unsigned prev)
{
    const Insn *in = &b->in[i];
    InsnFn f = in->fn;
    bool alu_f = f == x_alu_eg || f == x_alu_ge || f == x_alu_ei;
    bool test_f = f == x_test_eg || f == x_test_ei;
    unsigned sz = in->sz;
    if (!(alu_f || test_f) || (sz != 1 && sz != 2 && sz != 4)) {
        return false;
    }
    unsigned aop = test_f ? OP_AND : in->aop;
    bool cin = aop == OP_ADC || aop == OP_SBB;
    bool wr = !test_f && aop != OP_CMP;
    bool ge = f == x_alu_ge;
    bool mem = !in->is_reg;
    uint32_t m = sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    Slow sl = {{0}, 0};
    if (sz == 4 && !mem && !cin) {
        /* v0.18.61: de registros (o registro e inmediato) con los registros
         * del x86 directo; sin flags si nadie los lee */
        unsigned kind = aop == OP_ADD ? LF_ADD : (aop == OP_SUB || aop == OP_CMP) ? LF_SUB : LF_LOGIC;
        int d = k_rmap[ge ? in->reg : in->rm], src;
        bool imm = f == x_alu_ei || f == x_test_ei;
        uint32_t iv = in->imm;
        static const uint32_t opc[8] = {ADDW, ORRW, 0, 0, ANDW, SUBW, EORW, SUBW};
        static const uint32_t opcs[8] = {ADDW, ORRW, 0, 0, 0x6A000000u, 0x6B000000u, EORW, 0x6B000000u}; /* ands / subs */
        e->lf_kind = kind;
        if (e->lf_dead) {
            if (!wr) {
                return true; /* cmp / test sin nadie que lea los flags */
            }
            if (imm && (aop == OP_ADD || aop == OP_SUB) && (iv < 4096 || iv > 0xFFFFF000u)) {
                bool add = (aop == OP_ADD) == (iv < 4096);
                uint32_t k = iv < 4096 ? iv : (uint32_t)-iv;
                E(e, (add ? 0x11000000u : 0x51000000u) | k << 10 | (uint32_t)d << 5 | (uint32_t)d);
                return true;
            }
            if (imm) {
                movw(e, W10, iv);
                src = W10;
            } else {
                src = k_rmap[ge ? in->rm : in->reg];
            }
            rrr(e, opc[aop], d, d, src);
            return true;
        }
        if (imm) {
            movw(e, W10, iv);
            src = W10;
        } else {
            src = k_rmap[ge ? in->rm : in->reg];
        }
        movw(e, W12, kind | 4u << 8);
        strw(e, W12, XC, OFF(lf_op));
        strw(e, d, XC, OFF(lf_a));
        strw(e, src, XC, OFF(lf_b));
        rrr(e, opcs[aop], W11, d, src);
        strw(e, W11, XC, OFF(lf_res));
        if (wr) {
            mov_w(e, d, W11);
        }
        e->nzcv = (aop == OP_SUB || aop == OP_CMP || aop == OP_AND) ? kind : 0;
        return true;
    }
    if (cin) {
        emit_cf(e, prev);
        movw_r(e, W15, W12);
    }
    if (mem) {
        mem_x13(e, in, sz, wr && !ge, &sl);
    }
    if (ge) {
        opnd_ld(e, W9, false, in->reg, sz);
        opnd_ld(e, W10, mem, in->rm, sz);
    } else {
        opnd_ld(e, W9, mem, in->rm, sz);
        if (f == x_alu_ei || f == x_test_ei) {
            movw(e, W10, in->imm & m);
        } else {
            opnd_ld(e, W10, false, in->reg, sz);
        }
    }
    unsigned kind;
    switch (aop) {
    case OP_ADD: rrr(e, ADDW, W11, W9, W10); kind = LF_ADD; break;
    case OP_ADC: rrr(e, ADDW, W11, W9, W10); rrr(e, ADDW, W11, W11, W15); kind = LF_ADD; break;
    case OP_SUB:
    case OP_CMP: rrr(e, SUBW, W11, W9, W10); kind = LF_SUB; break;
    case OP_SBB: rrr(e, SUBW, W11, W9, W10); rrr(e, SUBW, W11, W11, W15); kind = LF_SUB; break;
    case OP_AND: rrr(e, ANDW, W11, W9, W10); kind = LF_LOGIC; break;
    case OP_OR: rrr(e, ORRW, W11, W9, W10); kind = LF_LOGIC; break;
    default: rrr(e, EORW, W11, W9, W10); kind = LF_LOGIC; break;
    }
    zext_sz(e, W11, sz);
    if (wr) {
        if (ge) {
            opnd_st(e, W11, false, in->reg, sz);
        } else {
            opnd_st(e, W11, mem, in->rm, sz);
        }
    }
    emit_lf_full(e, kind, sz, cin ? W15 : -1);
    unsigned keep = e->lf_kind;
    slow_tail(e, b, i, vm, &sl);
    e->lf_kind = keep;
    return true;
}

/* imul r32, r/m32 (, imm) */
static bool emit_imul(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    if (in->fn != x_imul || in->sz != 4) {
        return false;
    }
    Slow sl = {{0}, 0};
    if (in->is_reg) {
        ldrw(e, W9, XC, ROFF(in->rm));
    } else {
        mem_x13(e, in, 4, false, &sl);
        ldrw(e, W9, W13, 0);
    }
    if (in->aop) {
        movw(e, W10, in->imm);
    } else {
        ldrw(e, W10, XC, ROFF(in->reg));
    }
    E(e, 0x9B207C00u | (uint32_t)W10 << 16 | (uint32_t)W9 << 5 | W11); /* smull x11, w9, w10 */
    E(e, 0x93407C00u | (uint32_t)W11 << 5 | W12);                     /* sxtw x12, w11 */
    E(e, 0xEB00001Fu | (uint32_t)W11 << 16 | (uint32_t)W12 << 5);     /* cmp x12, x11 */
    cset(e, W10, COND_NE);
    strw(e, W11, XC, ROFF(in->reg));
    movw_r(e, W9, W11);
    emit_lf_full(e, LF_MUL, 4, -1);
    slow_tail(e, b, i, vm, &sl);
    e->lf_kind = 0;
    return true;
}

/* F7 de 32 bits: not, neg, mul, imul, div, idiv */
static bool emit_grp3(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    if (in->fn != x_interp || in->aop != 0xF7 || in->sz != 4 || in->reg < 2) {
        return false;
    }
    unsigned op = in->reg;
    bool mem = !in->is_reg;
    Slow sl = {{0}, 0};
    if (mem) {
        mem_x13(e, in, 4, op <= 3, &sl);
    }
    switch (op) {
    case 2: /* not */
        opnd_ld(e, W9, mem, in->rm, 4);
        E(e, 0x2A2003E0u | (uint32_t)W9 << 16 | W11); /* mvn w11, w9 */
        opnd_st(e, W11, mem, in->rm, 4);
        break;
    case 3: /* neg */
        opnd_ld(e, W10, mem, in->rm, 4);
        movw(e, W9, 0);
        rrr(e, SUBW, W11, W9, W10);
        opnd_st(e, W11, mem, in->rm, 4);
        emit_lf_full(e, LF_SUB, 4, -1);
        break;
    case 4:
    case 5: /* mul / imul: edx:eax = eax * v */
        opnd_ld(e, W10, mem, in->rm, 4);
        ldrw(e, W9, XC, ROFF(R_EAX));
        E(e, (op == 4 ? 0x9BA07C00u : 0x9B207C00u) | (uint32_t)W10 << 16 | (uint32_t)W9 << 5 | W11);
        E(e, 0xD360FC00u | (uint32_t)W11 << 5 | W12); /* lsr x12, x11, #32 */
        strw(e, W11, XC, ROFF(R_EAX));
        strw(e, W12, XC, ROFF(R_EDX));
        if (op == 4) {
            cmpwi(e, W12, 0);
        } else {
            E(e, 0x93407C00u | (uint32_t)W11 << 5 | W12);                 /* sxtw x12, w11 */
            E(e, 0xEB00001Fu | (uint32_t)W11 << 16 | (uint32_t)W12 << 5); /* cmp x12, x11 */
        }
        cset(e, W10, COND_NE);
        movw_r(e, W9, W11);
        emit_lf_full(e, LF_MUL, 4, -1);
        break;
    default: { /* div / idiv: cero o desborde, la función de siempre (excepción) */
        opnd_ld(e, W14, mem, in->rm, 4);
        slow_add(e, &sl, fwd_cb(e, J_CBZ, W14));
        ldrw(e, W9, XC, ROFF(R_EAX));
        ldrw(e, W10, XC, ROFF(R_EDX));
        E(e, 0xAA000000u | (uint32_t)W10 << 16 | 32u << 10 | (uint32_t)W9 << 5 | W9); /* orr x9, x9, x10, lsl #32 */
        if (op == 6) {
            E(e, 0x9AC00800u | (uint32_t)W14 << 16 | (uint32_t)W9 << 5 | W11); /* udiv x11, x9, x14 */
            E(e, 0xD360FC00u | (uint32_t)W11 << 5 | W12);                     /* lsr x12, x11, #32 */
            slow_add(e, &sl, fwd_cb(e, J_CBNZ | 0x80000000u, W12));
        } else {
            E(e, 0x93407C00u | (uint32_t)W14 << 5 | W14);                     /* sxtw x14, w14 */
            E(e, 0x9AC00C00u | (uint32_t)W14 << 16 | (uint32_t)W9 << 5 | W11); /* sdiv x11, x9, x14 */
            E(e, 0x93407C00u | (uint32_t)W11 << 5 | W12);                     /* sxtw x12, w11 */
            E(e, 0xEB00001Fu | (uint32_t)W11 << 16 | (uint32_t)W12 << 5);     /* cmp x12, x11 */
            slow_bcond(e, &sl, COND_NE);
        }
        E(e, 0x9B008000u | (uint32_t)W14 << 16 | (uint32_t)W9 << 10 | (uint32_t)W11 << 5 | W12); /* msub x12, x11, x14, x9 */
        strw(e, W11, XC, ROFF(R_EAX));
        strw(e, W12, XC, ROFF(R_EDX));
        break;
    }
    }
    slow_tail(e, b, i, vm, &sl);
    e->lf_kind = 0;
    return true;
}

/* desplazamientos: de 32 bits por CL, de 8 bits con cuenta fija 1..7 */
static bool emit_shift_more(Em *e, const Insn *in)
{
    if (in->fn != x_shift || !in->is_reg || (in->aop != 4 && in->aop != 5 && in->aop != 6 && in->aop != 7)) {
        return false;
    }
    unsigned kind = in->aop == 5 ? LF_SHR : in->aop == 7 ? LF_SAR : LF_SHL;
    if (in->sz == 4 && in->imm == 0xFF) {
        ldrw(e, W10, XC, ROFF(R_ECX));
        E(e, 0x12001000u | (uint32_t)W10 << 5 | W10); /* and w10, w10, #31 */
        uint32_t *skip = fwd_cb(e, J_CBZ, W10);
        ldrw(e, W9, XC, ROFF(in->rm));
        uint32_t opc = kind == LF_SHR ? 0x1AC02400u : kind == LF_SAR ? 0x1AC02800u : 0x1AC02000u;
        E(e, opc | (uint32_t)W10 << 16 | (uint32_t)W9 << 5 | W11);
        strw(e, W11, XC, ROFF(in->rm));
        movw(e, W12, 0);
        emit_lf_cin(e, kind);
        patch(e, skip);
        e->lf_kind = 0;
        return true;
    }
    if (in->sz == 1 && in->imm >= 1 && in->imm <= 7) {
        unsigned cnt = in->imm;
        ldrb_i(e, W9, XC, r8off(in->rm));
        if (kind == LF_SHL) {
            lslw(e, W11, W9, cnt);
        } else if (kind == LF_SHR) {
            E(e, 0x53007C00u | cnt << 16 | (uint32_t)W9 << 5 | W11);
        } else {
            E(e, 0x13001C00u | (uint32_t)W9 << 5 | W11);               /* sxtb w11, w9 */
            E(e, 0x13007C00u | cnt << 16 | (uint32_t)W11 << 5 | W11); /* asr */
        }
        zext_sz(e, W11, 1);
        strb(e, W11, XC, r8off(in->rm));
        movw(e, W10, cnt);
        emit_lf_full(e, kind, 1, -1);
        return true;
    }
    return false;
}

static uint32_t jit_cond(Cpu *c, uint32_t n) { return cond(c, (int)n) ? 1u : 0u; }

/* w0 = la condición n con los flags que haya (función de C, sin más) */
static void emit_cond_call(Em *e, unsigned n)
{
    movx_r(e, X0, XC);
    movw(e, X1, n);
    movx(e, W9, (uint64_t)(uintptr_t)jit_cond);
    regs_spill(e);
    E(e, 0xD63F0000u | 9u << 5); /* blr x9 */
    regs_load(e);
}

/* x13 = la dirección en el host del operando de memoria de 4 bytes (o al lento) */
static void mem_addr4(Em *e, const Insn *in, bool write, uint32_t **s1, uint32_t **s2, uint32_t **s3)
{
    emit_ea(e, in, true);
    emit_page(e, write, s1, s2, s3);
    E(e, 0x8B0B014Du); /* add x13, x10, x11 */
}

/* Las enteras nuevas; *ret: lo que devuelve emit_insn (false: puso el EIP). */
static bool emit_int(Em *e, const Block *b, unsigned i, const Vm *vm, unsigned prev, bool *ret)
{
    const Insn *in = &b->in[i];
    InsnFn f = in->fn;
    *ret = true;
    uint32_t *s1, *s2, *s3;
    /* alu de 32 bits con memoria: [m] op= reg/imm, cmp [m], test [m] */
    if (((f == x_alu_ei || f == x_alu_eg) && in->sz == 4 && !in->is_reg && alu_native(in->aop)) ||
        ((f == x_test_ei || f == x_test_eg) && in->sz == 4 && !in->is_reg)) {
        bool test = f == x_test_ei || f == x_test_eg;
        unsigned aop = test ? OP_AND : in->aop;
        bool wr = !test && aop != OP_CMP;
        mem_addr4(e, in, wr, &s1, &s2, &s3);
        if (f == x_alu_eg || f == x_test_eg) {
            ldrw(e, W10, XC, ROFF(in->reg));
        } else {
            movw(e, W10, in->imm);
        }
        ldrw(e, W9, W13, 0);
        emit_alu_op(e, aop);
        if (wr) {
            strw(e, W11, W13, 0);
        }
        uint32_t *done = fwd_b(e);
        fallback_tail(e, b, i, vm, done, s1, s2, s3);
        return true;
    }
    if (f == x_incdec && in->sz == 4 && !in->is_reg) {
        emit_cf(e, prev);
        mem_addr4(e, in, true, &s1, &s2, &s3);
        ldrw(e, W9, W13, 0);
        if (in->aop) {
            subwi(e, W11, W9, 1);
        } else {
            addwi(e, W11, W9, 1);
        }
        strw(e, W11, W13, 0);
        movw(e, W10, 1);
        emit_lf_cin(e, in->aop ? LF_DEC : LF_INC);
        uint32_t *done = fwd_b(e);
        fallback_tail(e, b, i, vm, done, s1, s2, s3);
        return true;
    }
    if (emit_alu_any(e, b, i, vm, prev) || emit_imul(e, b, i, vm) || emit_grp3(e, b, i, vm) || emit_shift_more(e, in)) {
        return true;
    }
    /* jcc tras inc/dec/desplazamientos de aquí: con los flags de ARM64 */
    if (f == x_jcc && (prev == LF_INC || prev == LF_DEC || prev == LF_SHL || prev == LF_SHR || prev == LF_SAR) &&
        emit_jcc_more(e, in, prev)) {
        *ret = false;
        return true;
    }
    /* jcc, setcc y cmov con flags de antes del bloque: la condición en C */
    if (f == x_jcc) {
        emit_cond_call(e, in->aop);
        movw(e, W12, in->next);
        movw(e, W13, in->imm);
        E(e, 0x7100001Fu); /* cmp w0, #0 */
        E(e, 0x1A800000u | (uint32_t)W12 << 16 | (uint32_t)COND_NE << 12 | (uint32_t)W13 << 5 | (uint32_t)W12);
        strw(e, W12, XC, OFF(eip));
        *ret = false;
        return true;
    }
    if (f == x_setcc && in->is_reg) {
        emit_cond_call(e, in->aop);
        strb(e, X0, XC, r8off(in->rm));
        return true;
    }
    if (f == x_cmov && in->sz == 4 && in->is_reg) {
        emit_cond_call(e, in->aop);
        ldrw(e, W12, XC, ROFF(in->reg));
        ldrw(e, W13, XC, ROFF(in->rm));
        E(e, 0x7100001Fu); /* cmp w0, #0 */
        E(e, 0x1A800000u | (uint32_t)W12 << 16 | (uint32_t)COND_NE << 12 | (uint32_t)W13 << 5 | (uint32_t)W12);
        strw(e, W12, XC, ROFF(in->reg));
        return true;
    }
    /* call / jmp / push indirectos (FF /2 /4 /6) de 32 bits */
    if (f == x_call_e || f == x_jmp_e || (f == x_push_e && in->sz == 4)) {
        uint32_t *t1 = NULL, *t2 = NULL, *t3 = NULL;
        if (in->is_reg) {
            ldrw(e, W14, XC, ROFF(in->rm));
        } else {
            mem_addr4(e, in, false, &t1, &t2, &t3);
            ldrw(e, W14, W13, 0);
        }
        uint32_t *p1 = NULL, *p2 = NULL, *p3 = NULL;
        if (f != x_jmp_e) { /* push (la siguiente o el valor) */
            ldrw(e, W9, XC, ROFF(R_ESP));
            subwi(e, W9, W9, 4);
            emit_page(e, true, &p1, &p2, &p3);
            if (f == x_call_e) {
                movw(e, W12, in->next);
                strw_r(e, W12, W10, W11);
            } else {
                strw_r(e, W14, W10, W11);
            }
            strw(e, W9, XC, ROFF(R_ESP));
        }
        if (f != x_push_e) {
            strw(e, W14, XC, OFF(eip));
            *ret = false;
        }
        Slow sl = {{0}, 0};
        uint32_t *all[6] = {t1, t2, t3, p1, p2, p3};
        for (unsigned k = 0; k < 6; k++) {
            if (all[k]) {
                slow_add(e, &sl, all[k]);
            }
        }
        slow_tail(e, b, i, vm, &sl);
        return true;
    }
    if (f == x_incdec && in->sz == 4 && in->is_reg) {
        int d = k_rmap[in->rm];
        if (e->lf_dead) { /* v0.18.61: nadie lee sus flags: solo el registro */
            if (in->aop) {
                subwi(e, d, d, 1);
            } else {
                addwi(e, d, d, 1);
            }
            e->lf_kind = in->aop ? LF_DEC : LF_INC;
            return true;
        }
        emit_cf(e, prev);
        mov_w(e, W9, d);
        movw(e, W10, 1);
        if (in->aop) {
            subwi(e, W11, W9, 1);
        } else {
            addwi(e, W11, W9, 1);
        }
        mov_w(e, d, W11);
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
    if (f == x_jmp) {
        if (g_link_on && i + 1 == b->n) {
            e->exit_eip = in->imm; /* por la casilla (el trocito pone el EIP) */
        } else {
            movw(e, W12, in->imm);
            strw(e, W12, XC, OFF(eip));
        }
        *ret = false;
        return true;
    }
    if (f == x_call || (f == x_ret && in->imm < 4092)) {
        ldrw(e, W9, XC, ROFF(R_ESP));
        if (f == x_call) {
            subwi(e, W9, W9, 4);
            emit_page(e, true, &s1, &s2, &s3);
            movw(e, W12, in->next);
            strw_r(e, W12, W10, W11);
            strw(e, W9, XC, ROFF(R_ESP));
            movw(e, W12, in->imm);
            strw(e, W12, XC, OFF(eip)); /* (también lo pone el trocito de la casilla) */
            if (i + 1 == b->n) {
                e->exit_eip = in->imm;
            }
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

/* ---------------------------------------------------------- etapa 5: más
 * instrucciones sin step() (v0.18.61). Con los datos de perfil.txt de la
 * Switch (tests/slow_scan.c): rep movs/stos, cvtsi2ss, fcomip, bsf, fxam,
 * fnstsw, cmpss, movmskps, rcpss, sqrt, leave, cdq, setcc en memoria... */

/* rol / ror de registro con cuenta fija (nativos solo si sus flags no se leen) */
static bool is_rot_reg(const Insn *in)
{
    return in->fn == x_shift && in->is_reg && (in->aop == 0 || in->aop == 1) && in->imm != 0xFF &&
           (in->sz == 1 || in->sz == 4) && (in->imm & 31) != 0;
}

/* El bloque llama a una función de C directa (no por step): para el perfil. */
static void mark_helper(Em *e, const Block *b, unsigned i)
{
    if (!e->fallback) {
        ((Block *)b)->helper |= 1ull << (i & 63);
    }
}

/* x1 = la copia de la Insn i (junto al código) */
static void adr_insn(Em *e, unsigned i)
{
    intptr_t d = (intptr_t)(uintptr_t)(e->insn_rx + i) - (intptr_t)em_rx(e);
    E(e, 0x10000000u | ((uint32_t)d & 3u) << 29 | (((uint32_t)(d >> 2)) & 0x7FFFFu) << 5 | 1u);
}

/* fn(c, x1, x2): guarda los registros del x86 antes (x2 es eax: se pone
 * después de guardarlos, desde w9 si a2_w9) y los vuelve a cargar. */
static void call_c(Em *e, uintptr_t fn, int x1_insn, bool a2_w9)
{
    regs_spill(e);
    if (a2_w9) {
        movw_r(e, 2, W9);
    }
    movx_r(e, X0, XC);
    if (x1_insn >= 0) {
        adr_insn(e, (unsigned)x1_insn);
    }
    movx(e, W9, (uint64_t)fn);
    E(e, 0xD63F0000u | 9u << 5); /* blr x9 */
    regs_load(e);
}

/* Lo que devuelven las funciones directas: 0 hecha, 1 no está (la de
 * siempre, por step), 2 paró (un fallo de memoria): sale con i hechas. */
static void helper_result(Em *e, unsigned i, Slow *s)
{
    uint32_t *ok = fwd_cb(e, J_CBZ, X0);
    cmpwi(e, X0, 1);
    slow_bcond(e, s, COND_EQ);
    exit_k(e, i);
    patch(e, ok);
}

/* x87 de registros por fpu_reg (sin decodificar) */
static uint32_t jit_x87r(Cpu *c, const Insn *in)
{
    return fpu_reg(c, in->aop, (uint8_t)(0xC0u | in->reg << 3 | in->rm)) ? 0u : 1u;
}

/* x87 con memoria en a */
static uint32_t jit_x87m(Cpu *c, const Insn *in, uint32_t a)
{
    if (!fpu_mem(c, in->aop, in->reg, a)) {
        return 1;
    }
    if (c->stop != CPU_RUNNING) {
        c->jit_stop_eip = in->start;
        return 2;
    }
    return 0;
}

/* las de un byte que hacía step() */
static uint32_t jit_op1(Cpu *c, const Insn *in)
{
    int osz = (in->sz & INTERP_P66) ? 2 : 4;
    switch (in->aop) {
    case 0x60: { /* pushad */
        uint32_t esp = c->r[R_ESP];
        for (int r = 0; r < 8 && c->stop == CPU_RUNNING; r++) {
            push(c, osz, r == R_ESP ? esp : c->r[r]);
        }
        break;
    }
    case 0x61: /* popad */
        for (int r = 7; r >= 0 && c->stop == CPU_RUNNING; r--) {
            uint32_t v = pop(c, osz);
            if (r != R_ESP && c->stop == CPU_RUNNING) {
                reg_set(c, r, osz, v);
            }
        }
        break;
    case 0x9C: push(c, osz, (FL(c) & 0x00244FD5u) | 0x2u); break;
    case 0x9E: FL(c) = (FL(c) & ~0xD5u) | ((c->r[R_EAX] >> 8) & 0xD5u); break;
    case 0x9F: reg_set(c, 4, 1, (FL(c) & 0xD5u) | 0x2u); break;
    case 0xD7: { /* xlat */
        uint32_t v = mrd(c, c->r[R_EBX] + (c->r[R_EAX] & 0xFF) + (in->fs ? c->fs_base : 0), 1);
        if (c->stop == CPU_RUNNING) {
            reg_set(c, R_EAX, 1, v);
        }
        break;
    }
    case 0xF5: FL(c) ^= F_CF; break;
    case 0xF8: FL(c) &= ~F_CF; break;
    case 0xF9: FL(c) |= F_CF; break;
    case 0xFC: FL(c) &= ~F_DF; break;
    case 0xFD: FL(c) |= F_DF; break;
    default: return 1;
    }
    if (c->stop != CPU_RUNNING) {
        c->jit_stop_eip = in->start;
        return 2;
    }
    return 0;
}

/* SSE nuevas en código nativo; false: no es de estas */
static bool emit_sse2(Em *e, const Block *b, unsigned i, const Vm *vm)
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
    uint32_t xo = 16u * x;
    Slow s = {{0}, 0};
    switch (op) {
    case 0x2A: /* cvtsi2ss xmm, r/m32 */
        if (!ss) {
            return false;
        }
        if (in->is_reg) {
            ldrw(e, W12, XC, ROFF(in->rm));
        } else {
            mem_at(e, in, 4, false, &s);
            ldrw_r(e, W12, W10, W11);
        }
        E(e, 0x1E220000u | (uint32_t)W12 << 5 | 0u); /* scvtf s0, w12 */
        xmm_base(e);
        strs_i(e, 0, W14, xo);
        break;
    case 0x50: { /* movmskps r32, xmm */
        if (ss || !in->is_reg) {
            return false;
        }
        xmm_base(e);
        ldrq_i(e, 0, W14, 16u * in->rm);
        E(e, 0x6F210400u);             /* ushr v0.4s, v0.4s, #31 */
        E(e, 0x0E043C00u | (uint32_t)W12);           /* umov w12, v0.s[0] */
        E(e, 0x0E0C3C00u | (uint32_t)W13);           /* umov w13, v0.s[1] */
        E(e, 0x0E143C00u | (uint32_t)X0);            /* umov w0, v0.s[2] */
        E(e, 0x0E1C3C00u | (uint32_t)X1);            /* umov w1, v0.s[3] */
        E(e, 0x2A000000u | (uint32_t)W13 << 16 | 1u << 10 | (uint32_t)W12 << 5 | W12); /* orr w12, w12, w13, lsl #1 */
        E(e, 0x2A000000u | (uint32_t)X0 << 16 | 2u << 10 | (uint32_t)W12 << 5 | W12);
        E(e, 0x2A000000u | (uint32_t)X1 << 16 | 3u << 10 | (uint32_t)W12 << 5 | W12);
        strw(e, W12, XC, ROFF(x));
        return true;
    }
    case 0x51: case 0x52: case 0x53: { /* sqrt / rsqrt / rcp (exactos, como el intérprete) */
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
            if (op == 0x53) {
                E(e, 0x1E2E1003u);                 /* fmov s3, #1.0 */
                fop(e, FDIVS, 2, 3, 1);
            } else {
                E(e, 0x1E21C000u | 1u << 5 | 2u);  /* fsqrt s2, s1 */
                if (op == 0x52) {
                    E(e, 0x1E2E1003u);
                    fop(e, FDIVS, 2, 3, 2);
                }
            }
            nan_check(e, 2, true, &s);
            strs_i(e, 2, W14, xo);
        } else {
            if (op == 0x53) {
                E(e, 0x4F03F603u);                 /* fmov v3.4s, #1.0 */
                fop(e, VFDIV, 2, 3, 1);
            } else {
                E(e, 0x6EA1F800u | 1u << 5 | 2u);  /* fsqrt v2.4s, v1.4s */
                if (op == 0x52) {
                    E(e, 0x4F03F603u);
                    fop(e, VFDIV, 2, 3, 2);
                }
            }
            nan_check(e, 2, false, &s);
            strq_i(e, 2, W14, xo);
        }
        break;
    }
    case 0xC2: { /* cmpps / cmpss: a (v0) pred b (v1) -> todo unos o ceros */
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
        } else {
            ldrq_i(e, 0, W14, xo);
        }
        unsigned pred = in->imm & 7;
        bool inv = pred >= 4;
        switch (pred & 3) {
        case 0: fop(e, VFCMEQ, 2, 0, 1); break;      /* a == b */
        case 1: fop(e, VFCMGT, 2, 1, 0); break;      /* b > a */
        case 2: fop(e, 0x6E20E400u, 2, 1, 0); break; /* b >= a */
        default: /* ordenados: a == a y b == b */
            fop(e, VFCMEQ, 3, 0, 0);
            fop(e, VFCMEQ, 4, 1, 1);
            fop(e, VAND, 2, 3, 4);
            inv = !inv; /* 3: sin orden = no ordenados; 7: ordenados */
            break;
        }
        if (inv) {
            E(e, 0x6E205800u | 2u << 5 | 2u); /* mvn v2.16b, v2.16b */
        }
        if (ss) {
            strs_i(e, 2, W14, xo);
        } else {
            strq_i(e, 2, W14, xo);
        }
        break;
    }
    case 0xBC: case 0xBD: { /* bsf / bsr r32 (ZF como un and del valor) */
        if (ss) {
            return false;
        }
        if (in->is_reg) {
            ldrw(e, W12, XC, ROFF(in->rm));
        } else {
            mem_at(e, in, 4, false, &s);
            ldrw_r(e, W12, W10, W11);
        }
        movw_r(e, W9, W12);
        movw_r(e, W10, W12);
        movw_r(e, W11, W12);
        emit_lf(e, LF_LOGIC); /* usa w12: el valor sigue en w11 */
        uint32_t *zero = fwd_cb(e, J_CBZ, W11);
        if (op == 0xBC) {
            E(e, 0x5AC00000u | (uint32_t)W11 << 5 | W13); /* rbit w13, w11 */
            E(e, 0x5AC01000u | (uint32_t)W13 << 5 | W13); /* clz w13, w13 */
        } else {
            E(e, 0x5AC01000u | (uint32_t)W11 << 5 | W13); /* clz w13, w11 */
            E(e, 0x52001000u | (uint32_t)W13 << 5 | W13); /* eor w13, w13, #31 */
        }
        strw(e, W13, XC, ROFF(x));
        patch(e, zero);
        break;
    }
    default:
        return false;
    }
    slow_tail(e, b, i, vm, &s);
    return true;
}

/* fcomi / fucomi / fcomip / fucomip: ZF PF CF de st0 contra st(k) */
static bool emit_fcomi(Em *e, const Insn *in)
{
    unsigned op = in->aop, reg = in->reg;
    if (!in->is_reg || !((op == 0xDB || op == 0xDF) && (reg == 5 || reg == 6))) {
        return false;
    }
    st_load(e, 1, in->rm);
    st_load(e, 0, 0);
    fcmpd(e, 0, 1);
    cset(e, W10, COND_VS);
    cset(e, W11, COND_EQ);
    cset(e, W12, COND_MI);
    rrr(e, ORRW, W11, W11, W10); /* ZF */
    rrr(e, ORRW, W12, W12, W10); /* CF */
    lslw(e, W10, W10, 2);        /* PF */
    lslw(e, W11, W11, 6);
    rrr(e, ORRW, W10, W10, W11);
    rrr(e, ORRW, W10, W10, W12);
    ldrw(e, W11, XC, OFF(flags));
    movw(e, W12, F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF);
    rrr(e, BICW, W11, W11, W12);
    rrr(e, ORRW, W11, W11, W10);
    strw(e, W11, XC, OFF(flags));
    strb(e, XZR, XC, OFF(lf_op));
    if (op == 0xDF) {
        fpop_n(e);
    }
    return true;
}

/* desplazamientos de memoria con cuenta fija (shl shr sar de 8, 16 y 32 bits) */
static bool emit_shift_mem(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    unsigned sz = in->sz, cnt = in->imm & 31;
    if (in->fn != x_shift || in->is_reg || in->imm == 0xFF ||
        (in->aop != 4 && in->aop != 5 && in->aop != 6 && in->aop != 7) || (sz != 1 && sz != 2 && sz != 4) || !cnt ||
        cnt >= 8 * sz) {
        return false;
    }
    unsigned kind = in->aop == 5 ? LF_SHR : in->aop == 7 ? LF_SAR : LF_SHL;
    Slow sl = {{0}, 0};
    mem_x13(e, in, sz, true, &sl);
    opnd_ld(e, W9, true, 0, sz);
    if (kind == LF_SHL) {
        lslw(e, W11, W9, cnt);
    } else if (kind == LF_SHR) {
        E(e, 0x53007C00u | cnt << 16 | (uint32_t)W9 << 5 | W11); /* lsr */
    } else {
        if (sz == 1) {
            E(e, 0x13001C00u | (uint32_t)W9 << 5 | W11); /* sxtb */
        } else if (sz == 2) {
            E(e, 0x13003C00u | (uint32_t)W9 << 5 | W11); /* sxth */
        } else {
            movw_r(e, W11, W9);
        }
        E(e, 0x13007C00u | cnt << 16 | (uint32_t)W11 << 5 | W11); /* asr */
    }
    zext_sz(e, W11, sz);
    opnd_st(e, W11, true, 0, sz);
    movw(e, W10, cnt);
    emit_lf_full(e, kind, sz, -1);
    slow_tail(e, b, i, vm, &sl);
    return true;
}

/* Las que no tienen otro camino nativo; true: hechas (sin poner el EIP). */
static bool emit_extra(Em *e, const Block *b, unsigned i, const Vm *vm)
{
    const Insn *in = &b->in[i];
    InsnFn f = in->fn;
    Slow s = {{0}, 0};
    if (f == x_cdq && in->sz == 4) {
        ldrw(e, W12, XC, ROFF(R_EAX));
        E(e, 0x131F7C00u | (uint32_t)W12 << 5 | W12); /* asr w12, w12, #31 */
        strw(e, W12, XC, ROFF(R_EDX));
        return true;
    }
    if (f == x_leave) { /* esp = ebp; pop ebp */
        uint32_t *s1, *s2, *s3;
        ldrw(e, W9, XC, ROFF(R_EBP));
        emit_page(e, false, &s1, &s2, &s3);
        ldrw_r(e, W12, W10, W11);
        addwi(e, W9, W9, 4);
        strw(e, W9, XC, ROFF(R_ESP));
        strw(e, W12, XC, ROFF(R_EBP));
        uint32_t *done = fwd_b(e);
        fallback_tail(e, b, i, vm, done, s1, s2, NULL);
        return true;
    }
    if (f == x_emms) {
        movw(e, W12, 0xFF);
        strb(e, W12, XC, OFF(fp.empty));
        strb(e, XZR, XC, OFF(fp.mmx));
        return true;
    }
    if (f == x_setcc && !in->is_reg) {
        emit_cond_call(e, in->aop);
        mem_at(e, in, 1, true, &s);
        strb_r(e, X0, W10, W11);
        slow_tail(e, b, i, vm, &s);
        return true;
    }
    if (emit_shift_mem(e, b, i, vm)) {
        return true;
    }
    if (is_rot_reg(in) && e->lf_dead) { /* rol / ror sin flags (nadie los lee) */
        unsigned cnt = in->imm & 31;
        if (in->sz == 4) {
            unsigned r = in->aop == 1 ? cnt : (32u - cnt) & 31u;
            ldrw(e, W9, XC, ROFF(in->rm));
            E(e, 0x13800000u | (uint32_t)W9 << 16 | r << 10 | (uint32_t)W9 << 5 | W11); /* ror w11, w9, #r */
            strw(e, W11, XC, ROFF(in->rm));
        } else if (cnt & 7) {
            unsigned k = cnt & 7;
            ldrb_i(e, W9, XC, r8off(in->rm));
            E(e, 0x2A000000u | (uint32_t)W9 << 16 | 8u << 10 | (uint32_t)W9 << 5 | W11); /* orr w11, w9, w9, lsl #8 */
            E(e, 0x53007C00u | (in->aop == 0 ? 8u - k : k) << 16 | (uint32_t)W11 << 5 | W11); /* lsr */
            strb(e, W11, XC, r8off(in->rm));
        }
        e->lf_kind = 0;
        return true;
    }
    if (f != x_interp) {
        return false;
    }
    unsigned op = in->aop;
    bool is0f = (in->sz & INTERP_0F) != 0;
    if (!is0f && op >= 0x91 && op <= 0x97 && !(in->sz & INTERP_P66)) { /* xchg eax, r32 */
        unsigned r = op & 7;
        ldrw(e, W12, XC, ROFF(R_EAX));
        ldrw(e, W13, XC, ROFF(r));
        strw(e, W13, XC, ROFF(R_EAX));
        strw(e, W12, XC, ROFF(r));
        return true;
    }
    if (!is0f && op == 0x98) { /* cwde / cbw */
        ldrw(e, W12, XC, ROFF(R_EAX));
        if (in->sz & INTERP_P66) {
            E(e, 0x13001C00u | (uint32_t)W12 << 5 | W12); /* sxtb */
            strh_i(e, W12, XC, ROFF(R_EAX));
        } else {
            E(e, 0x13003C00u | (uint32_t)W12 << 5 | W12); /* sxth */
            strw(e, W12, XC, ROFF(R_EAX));
        }
        return true;
    }
    if (!is0f && op == 0x9B) { /* fwait: nada */
        return true;
    }
    if (!is0f && op >= 0xD8 && op <= 0xDF) {
        if (emit_fcomi(e, in)) {
            return true;
        }
        mark_helper(e, b, i);
        if (in->is_reg) {
            call_c(e, (uintptr_t)jit_x87r, (int)i, false);
        } else {
            emit_ea(e, in, true);
            call_c(e, (uintptr_t)jit_x87m, (int)i, true);
        }
        helper_result(e, i, &s);
        slow_tail(e, b, i, vm, &s);
        return true;
    }
    if (!is0f && (op == 0x60 || op == 0x61 || op == 0x9C || op == 0x9E || op == 0x9F || op == 0xD7 || op == 0xF5 ||
                  op == 0xF8 || op == 0xF9 || op == 0xFC || op == 0xFD)) {
        mark_helper(e, b, i);
        call_c(e, (uintptr_t)jit_op1, (int)i, false);
        helper_result(e, i, &s);
        slow_tail(e, b, i, vm, &s);
        return true;
    }
    return emit_sse2(e, b, i, vm);
}
