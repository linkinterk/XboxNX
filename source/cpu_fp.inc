/*
 * XboxNX — intérprete x86: coma flotante (x87), MMX y SSE del Pentium III.
 *
 * Se incluye desde cpu.c (usa sus funciones internas). Implementación propia.
 *
 * x87: los 8 registros en double (no en 80 bits): el Xbox arranca con la
 * precisión de 53 bits (cw 0x27F) y Direct3D la baja a 24, así que el
 * resultado es el mismo salvo en las funciones trascendentes. Con la
 * precisión de 24 bits cada resultado se redondea a float, como el chip.
 * MMX va en sus propios registros (los juegos hacen emms entre MMX y x87).
 * SSE: las operaciones en float del host, IEEE igual que el Pentium III; min,
 * max, comparaciones y conversiones con las reglas de x86.
 */
#include <fenv.h>
#include <math.h>

/* ================================================================ x87 */

#define FSW_C0 0x0100u
#define FSW_C1 0x0200u
#define FSW_C2 0x0400u
#define FSW_C3 0x4000u
#define FSW_CC (FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3)

static double *st(Cpu *c, int i) { return &c->fp.st[(c->fp.top + i) & 7]; }

/* Precisión de 24 bits (PC = 0): la mantisa a 24 bits con el modo de
 * redondeo vigente, pero el exponente sin el límite de float (como el chip). */
static double fp_round(Cpu *c, double v)
{
    if (((c->fpu_cw >> 8) & 3) != 0 || !isfinite(v) || v == 0.0) {
        return v;
    }
    int e;
    double m = frexp(v, &e);
    return ldexp(rint(ldexp(m, 24)), e - 24);
}

/* El modo de redondeo del cw (RC) en el host mientras se calcula. */
static const int k_fe_modes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};

static int rc_enter(unsigned rc)
{
    rc &= 3;
    if (rc) {
        fesetround(k_fe_modes[rc]);
    }
    return (int)rc;
}

static void rc_leave(int rc)
{
    if (rc) {
        fesetround(FE_TONEAREST);
    }
}

static void fpush(Cpu *c, double v)
{
    c->fp.top = (c->fp.top - 1) & 7;
    c->fp.st[c->fp.top] = v;
    c->fp.empty &= (uint8_t)~(1u << c->fp.top);
}

static void fpop(Cpu *c)
{
    c->fp.empty |= (uint8_t)(1u << c->fp.top);
    c->fp.top = (c->fp.top + 1) & 7;
}

static uint16_t fsw(Cpu *c) { return (uint16_t)((c->fpu_sw & ~0x3800u) | ((unsigned)c->fp.top << 11)); }

static void fcc(Cpu *c, uint16_t bits) { c->fpu_sw = (uint16_t)((c->fpu_sw & ~FSW_CC) | bits); }

/* Comparación x87: C3 C2 C0 = 000 mayor, 001 menor, 100 igual, 111 sin orden */
static void fcompare(Cpu *c, double a, double b)
{
    if (isnan(a) || isnan(b)) {
        fcc(c, FSW_C0 | FSW_C2 | FSW_C3);
    } else if (a > b) {
        fcc(c, 0);
    } else if (a < b) {
        fcc(c, FSW_C0);
    } else {
        fcc(c, FSW_C3);
    }
}

/* fcomi / fucomi y comiss / ucomiss: ZF PF CF; OF SF AF a cero */
static void fcomi_flags(Cpu *c, double a, double b)
{
    FL(c) &= ~(F_ZF | F_PF | F_CF | F_OF | F_SF | F_AF);
    if (isnan(a) || isnan(b)) {
        FL(c) |= F_ZF | F_PF | F_CF;
    } else if (a < b) {
        FL(c) |= F_CF;
    } else if (a == b) {
        FL(c) |= F_ZF;
    }
}

/* redondeo a entero según el modo (RC del cw o del MXCSR): 0 al más cercano
 * (par), 1 hacia abajo, 2 hacia arriba, 3 truncar */
static double round_mode(double v, unsigned rc)
{
    switch (rc & 3) {
    case 1: return floor(v);
    case 2: return ceil(v);
    case 3: return trunc(v);
    default: return nearbyint(v); /* el host está al más cercano (par) fuera de rc_enter */
    }
}

/* a entero con signo de bits bits; fuera de rango o NaN: el "entero indefinido" */
static int64_t to_int(double v, unsigned rc, int bits)
{
    double r = round_mode(v, rc);
    double lim = ldexp(1.0, bits - 1);
    if (isnan(r) || r >= lim || r < -lim) {
        return bits == 64 ? INT64_MIN : -(int64_t)(1ull << (bits - 1));
    }
    return (int64_t)r;
}

/* 80 bits (signo, exponente de 15, mantisa de 64 con el bit entero) <-> double */
static double f80_to_double(const uint8_t *p)
{
    uint64_t mant;
    memcpy(&mant, p, 8);
    uint16_t se = (uint16_t)(p[8] | (p[9] << 8));
    int sign = se >> 15, exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) {
        v = 0.0;
    } else if (exp == 0x7FFF) {
        v = (mant << 1) ? NAN : INFINITY;
    } else {
        v = ldexp((double)mant, exp - 16383 - 63);
    }
    return sign ? -v : v;
}

static void double_to_f80(double v, uint8_t *p)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    uint16_t sign = (uint16_t)((bits >> 63) << 15);
    int e = (int)((bits >> 52) & 0x7FF);
    uint64_t m = bits & 0xFFFFFFFFFFFFFull, mant;
    int exp;
    if (e == 0 && m == 0) {
        exp = 0;
        mant = 0;
    } else if (e == 0x7FF) {
        exp = 0x7FFF;
        mant = (1ull << 63) | (m << 11);
    } else if (e == 0) { /* subnormal: se normaliza */
        int shift = __builtin_clzll(m);
        mant = m << shift;
        exp = -1074 - shift + 16383 + 63;
    } else {
        exp = e - 1023 + 16383;
        mant = (1ull << 63) | (m << 11);
    }
    memcpy(p, &mant, 8);
    p[8] = (uint8_t)exp;
    p[9] = (uint8_t)((exp >> 8) | (sign >> 8));
}

static float rdf32(Cpu *c, uint32_t a)
{
    uint32_t u = mrd(c, a, 4);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void wrf32(Cpu *c, uint32_t a, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    mwr(c, a, 4, u);
}

static uint64_t rd64(Cpu *c, uint32_t a) { return (uint64_t)mrd(c, a, 4) | ((uint64_t)mrd(c, a + 4, 4) << 32); }

static void wr64(Cpu *c, uint32_t a, uint64_t v)
{
    mwr(c, a, 4, (uint32_t)v);
    mwr(c, a + 4, 4, (uint32_t)(v >> 32));
}

static double rdf64(Cpu *c, uint32_t a)
{
    uint64_t u = rd64(c, a);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static void wrf64(Cpu *c, uint32_t a, double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    wr64(c, a, u);
}

static double rdf80(Cpu *c, uint32_t a)
{
    uint8_t b[10];
    for (int i = 0; i < 10; i++) {
        b[i] = (uint8_t)mrd(c, a + (uint32_t)i, 1);
    }
    return f80_to_double(b);
}

static void wrf80(Cpu *c, uint32_t a, double v)
{
    uint8_t b[10];
    double_to_f80(v, b);
    for (int i = 0; i < 10; i++) {
        mwr(c, a + (uint32_t)i, 1, b[i]);
    }
}

/* la aritmética de D8/DC/DA/DE: dst = dst op src (o al revés en los r) */
static double farith(Cpu *c, int op, double a, double b)
{
    volatile double va = a, vb = b;
    double r;
    int rc = rc_enter(c->fpu_cw >> 10);
    switch (op) {
    case 0: r = va + vb; break;
    case 1: r = va * vb; break;
    case 4: r = va - vb; break;
    case 5: r = vb - va; break;
    case 6: r = va / vb; break;
    default: r = vb / va; break; /* 7 */
    }
    r = fp_round(c, r);
    rc_leave(rc);
    return r;
}

static float to_f32(Cpu *c, double v)
{
    volatile double vv = v;
    int rc = rc_enter(c->fpu_cw >> 10);
    float f = (float)vv;
    rc_leave(rc);
    return f;
}

static double fsqrt_rc(Cpu *c, double v)
{
    volatile double vv = v;
    int rc = rc_enter(c->fpu_cw >> 10);
    double r = fp_round(c, sqrt(vv));
    rc_leave(rc);
    return r;
}

static uint16_t ftag_word(Cpu *c)
{
    uint16_t tw = 0;
    for (int i = 0; i < 8; i++) {
        unsigned t;
        double v = c->fp.st[i];
        if (c->fp.empty & (1u << i)) {
            t = 3;
        } else if (v == 0.0) {
            t = 1;
        } else if (!isnormal(v)) {
            t = 2;
        } else {
            t = 0;
        }
        tw |= (uint16_t)(t << (2 * i));
    }
    return tw;
}

static void fpu_reset(Cpu *c)
{
    c->fpu_cw = 0x037F;
    c->fpu_sw = 0;
    c->fp.top = 0;
    c->fp.empty = 0xFF;
}

/* entorno de 28 bytes (modo protegido de 32 bits) */
static void fstenv(Cpu *c, uint32_t a)
{
    mwr(c, a, 4, 0xFFFF0000u | c->fpu_cw);
    mwr(c, a + 4, 4, 0xFFFF0000u | fsw(c));
    mwr(c, a + 8, 4, 0xFFFF0000u | ftag_word(c));
    for (uint32_t i = 12; i < 28; i += 4) {
        mwr(c, a + i, 4, 0);
    }
}

static void fldenv(Cpu *c, uint32_t a)
{
    c->fpu_cw = (uint16_t)mrd(c, a, 2);
    uint16_t sw = (uint16_t)mrd(c, a + 4, 2);
    c->fpu_sw = sw & (uint16_t)~0x3800u;
    c->fp.top = (sw >> 11) & 7;
    uint16_t tw = (uint16_t)mrd(c, a + 8, 2);
    c->fp.empty = 0;
    for (int i = 0; i < 8; i++) {
        if (((tw >> (2 * i)) & 3) == 3) {
            c->fp.empty |= (uint8_t)(1u << i);
        }
    }
}

static void fxam(Cpu *c)
{
    double v = *st(c, 0);
    uint16_t cc = signbit(v) ? FSW_C1 : 0;
    if (c->fp.empty & (1u << c->fp.top)) {
        cc |= FSW_C3 | FSW_C0;
    } else if (isnan(v)) {
        cc |= FSW_C0;
    } else if (isinf(v)) {
        cc |= FSW_C2 | FSW_C0;
    } else if (v == 0.0) {
        cc |= FSW_C3;
    } else if (!isnormal(v)) {
        cc |= FSW_C3 | FSW_C2;
    } else {
        cc |= FSW_C2;
    }
    fcc(c, cc);
}

static void fprem(Cpu *c, bool ieee)
{
    double a = *st(c, 0), b = *st(c, 1);
    double q = ieee ? round_mode(a / b, 0) : trunc(a / b);
    double r = ieee ? remainder(a, b) : fmod(a, b);
    *st(c, 0) = r;
    int64_t qi = isnan(q) || isinf(q) ? 0 : (int64_t)fabs(q);
    uint16_t cc = 0;
    if (qi & 1) {
        cc |= FSW_C1;
    }
    if (qi & 2) {
        cc |= FSW_C3;
    }
    if (qi & 4) {
        cc |= FSW_C0;
    }
    fcc(c, cc); /* C2 = 0: reducción completa */
}

static bool fcmov_cond(Cpu *c, int n)
{
    switch (n) {
    case 0: return FL(c) & F_CF;
    case 1: return FL(c) & F_ZF;
    case 2: return FL(c) & (F_CF | F_ZF);
    default: return FL(c) & F_PF;
    }
}

static void fpu(Cpu *c, const Dec *d, uint8_t op)
{
    uint8_t m = fetch8(c);
    int reg = (m >> 3) & 7, i = m & 7;
    char b[48];
    if (m < 0xC0) {
        Ea e = modrm_ea(c, d, m);
        uint32_t a = e.addr;
        switch (op) {
        case 0xD8: /* m32fp */
        case 0xDC: /* m64fp */
        case 0xDA: /* m32int */
        case 0xDE: { /* m16int */
            double v = op == 0xD8 ? rdf32(c, a) : op == 0xDC ? rdf64(c, a)
                     : op == 0xDA ? (double)(int32_t)mrd(c, a, 4) : (double)(int16_t)mrd(c, a, 2);
            if (reg == 2 || reg == 3) {
                fcompare(c, *st(c, 0), v);
                if (reg == 3) {
                    fpop(c);
                }
            } else {
                *st(c, 0) = farith(c, reg, *st(c, 0), v);
            }
            return;
        }
        case 0xD9:
            switch (reg) {
            case 0: fpush(c, rdf32(c, a)); return;
            case 2: wrf32(c, a, to_f32(c, *st(c, 0))); return;
            case 3: wrf32(c, a, to_f32(c, *st(c, 0))); fpop(c); return;
            case 4: fldenv(c, a); return;
            case 5: c->fpu_cw = (uint16_t)mrd(c, a, 2); return;
            case 6: fstenv(c, a); return;
            case 7: mwr(c, a, 2, c->fpu_cw); return;
            default: break;
            }
            break;
        case 0xDB:
            switch (reg) {
            case 0: fpush(c, (double)(int32_t)mrd(c, a, 4)); return;
            case 2: mwr(c, a, 4, (uint32_t)to_int(*st(c, 0), c->fpu_cw >> 10, 32)); return;
            case 3: mwr(c, a, 4, (uint32_t)to_int(*st(c, 0), c->fpu_cw >> 10, 32)); fpop(c); return;
            case 5: fpush(c, rdf80(c, a)); return;
            case 7: wrf80(c, a, *st(c, 0)); fpop(c); return;
            default: break;
            }
            break;
        case 0xDD:
            switch (reg) {
            case 0: fpush(c, rdf64(c, a)); return;
            case 2: wrf64(c, a, *st(c, 0)); return;
            case 3: wrf64(c, a, *st(c, 0)); fpop(c); return;
            case 4: /* frstor */
                fldenv(c, a);
                for (int k = 0; k < 8; k++) {
                    *st(c, k) = rdf80(c, a + 28 + 10 * (uint32_t)k);
                }
                return;
            case 6: /* fnsave */
                fstenv(c, a);
                for (int k = 0; k < 8; k++) {
                    wrf80(c, a + 28 + 10 * (uint32_t)k, *st(c, k));
                }
                fpu_reset(c);
                return;
            case 7: mwr(c, a, 2, fsw(c)); return;
            default: break;
            }
            break;
        case 0xDF:
            switch (reg) {
            case 0: fpush(c, (double)(int16_t)mrd(c, a, 2)); return;
            case 2: mwr(c, a, 2, (uint32_t)to_int(*st(c, 0), c->fpu_cw >> 10, 16)); return;
            case 3: mwr(c, a, 2, (uint32_t)to_int(*st(c, 0), c->fpu_cw >> 10, 16)); fpop(c); return;
            case 5: fpush(c, (double)(int64_t)rd64(c, a)); return;
            case 7: wr64(c, a, (uint64_t)to_int(*st(c, 0), c->fpu_cw >> 10, 64)); fpop(c); return;
            default: break;
            }
            break;
        default:
            break;
        }
        snprintf(b, sizeof(b), "FPU x87 (%02X /%d)", op, reg);
        unimpl(c, d, b);
        return;
    }
    /* formas de registro */
    switch (op) {
    case 0xD8: /* st0 = st0 op st(i) */
        if (reg == 2 || reg == 3) {
            fcompare(c, *st(c, 0), *st(c, i));
            if (reg == 3) {
                fpop(c);
            }
        } else {
            *st(c, 0) = farith(c, reg, *st(c, 0), *st(c, i));
        }
        return;
    case 0xDC: /* st(i) = st(i) op st0 (sub/subr y div/divr cambiados) */
    case 0xDE: { /* lo mismo y pop */
        if (op == 0xDE && m == 0xD9) { /* fcompp */
            fcompare(c, *st(c, 0), *st(c, 1));
            fpop(c);
            fpop(c);
            return;
        }
        if (reg == 2 || reg == 3) {
            if (op == 0xDC) { /* fcom/fcomp st(i) (alias) */
                fcompare(c, *st(c, 0), *st(c, i));
                if (reg == 3) {
                    fpop(c);
                }
                return;
            }
            break;
        }
        static const int swap[8] = {0, 1, 2, 3, 5, 4, 7, 6};
        *st(c, i) = farith(c, swap[reg], *st(c, i), *st(c, 0));
        if (op == 0xDE) {
            fpop(c);
        }
        return;
    }
    case 0xD9:
        if (reg == 0) { /* fld st(i) */
            fpush(c, *st(c, i));
            return;
        }
        if (reg == 1) { /* fxch */
            double t = *st(c, 0);
            *st(c, 0) = *st(c, i);
            *st(c, i) = t;
            return;
        }
        switch (m) {
        case 0xD0: return; /* fnop */
        case 0xE0: *st(c, 0) = -*st(c, 0); return;
        case 0xE1: *st(c, 0) = fabs(*st(c, 0)); return;
        case 0xE4: fcompare(c, *st(c, 0), 0.0); return;
        case 0xE5: fxam(c); return;
        case 0xE8: fpush(c, 1.0); return;
        case 0xE9: fpush(c, 3.321928094887362347870); return; /* log2(10) */
        case 0xEA: fpush(c, 1.442695040888963407360); return; /* log2(e) */
        case 0xEB: fpush(c, 3.141592653589793238463); return;
        case 0xEC: fpush(c, 0.301029995663981195214); return; /* log10(2) */
        case 0xED: fpush(c, 0.693147180559945309417); return; /* ln(2) */
        case 0xEE: fpush(c, 0.0); return;
        case 0xF0: *st(c, 0) = exp2(*st(c, 0)) - 1.0; return;
        case 0xF1: *st(c, 1) = fp_round(c, *st(c, 1) * log2(*st(c, 0))); fpop(c); return;
        case 0xF2: *st(c, 0) = tan(*st(c, 0)); fpush(c, 1.0); fcc(c, 0); return;
        case 0xF3: *st(c, 1) = atan2(*st(c, 1), *st(c, 0)); fpop(c); return;
        case 0xF4: { /* fxtract */
            double v = *st(c, 0);
            int ex = 0;
            double mant = frexp(v, &ex);
            *st(c, 0) = (double)(ex - 1);
            fpush(c, mant * 2.0);
            return;
        }
        case 0xF5: fprem(c, true); return;
        case 0xF6: c->fp.top = (c->fp.top - 1) & 7; return;
        case 0xF7: c->fp.top = (c->fp.top + 1) & 7; return;
        case 0xF8: fprem(c, false); return;
        case 0xF9: *st(c, 1) = fp_round(c, *st(c, 1) * log2(*st(c, 0) + 1.0)); fpop(c); return;
        case 0xFA: *st(c, 0) = fsqrt_rc(c, *st(c, 0)); return;
        case 0xFB: { /* fsincos */
            double v = *st(c, 0);
            *st(c, 0) = sin(v);
            fpush(c, cos(v));
            fcc(c, 0);
            return;
        }
        case 0xFC: *st(c, 0) = round_mode(*st(c, 0), c->fpu_cw >> 10); return;
        case 0xFD: { /* fscale */
            double sc = trunc(*st(c, 1));
            if (isnan(sc) || isinf(sc)) {
                *st(c, 0) = isnan(sc) ? sc : *st(c, 0) * (sc > 0 ? INFINITY : 0.0);
                return;
            }
            int rc = rc_enter(c->fpu_cw >> 10);
            *st(c, 0) = ldexp(*st(c, 0), sc > 100000 ? 100000 : sc < -100000 ? -100000 : (int)sc);
            rc_leave(rc);
            return;
        }
        case 0xFE: *st(c, 0) = sin(*st(c, 0)); fcc(c, 0); return;
        case 0xFF: *st(c, 0) = cos(*st(c, 0)); fcc(c, 0); return;
        default: break;
        }
        break;
    case 0xDA:
        if (reg < 4) { /* fcmovb / e / be / u */
            if (fcmov_cond(c, reg)) {
                *st(c, 0) = *st(c, i);
            }
            return;
        }
        if (m == 0xE9) { /* fucompp */
            fcompare(c, *st(c, 0), *st(c, 1));
            fpop(c);
            fpop(c);
            return;
        }
        break;
    case 0xDB:
        if (reg < 4) { /* fcmovnb / ne / nbe / nu */
            if (!fcmov_cond(c, reg)) {
                *st(c, 0) = *st(c, i);
            }
            return;
        }
        if (m == 0xE2) { /* fnclex */
            c->fpu_sw &= 0x7F00;
            return;
        }
        if (m == 0xE3) { /* fninit */
            fpu_reset(c);
            return;
        }
        if (reg == 5 || reg == 6) { /* fucomi / fcomi */
            fcomi_flags(c, *st(c, 0), *st(c, i));
            return;
        }
        if (m == 0xE0 || m == 0xE1 || m == 0xE4) { /* feni fdisi fsetpm: nada */
            return;
        }
        break;
    case 0xDD:
        if (reg == 0) { /* ffree */
            c->fp.empty |= (uint8_t)(1u << ((c->fp.top + i) & 7));
            return;
        }
        if (reg == 2 || reg == 3) { /* fst / fstp st(i) */
            *st(c, i) = *st(c, 0);
            if (reg == 3) {
                fpop(c);
            }
            return;
        }
        if (reg == 4 || reg == 5) { /* fucom / fucomp */
            fcompare(c, *st(c, 0), *st(c, i));
            if (reg == 5) {
                fpop(c);
            }
            return;
        }
        break;
    case 0xDF:
        if (m == 0xE0) { /* fnstsw ax */
            reg_set(c, R_EAX, 2, fsw(c));
            return;
        }
        if (reg == 5 || reg == 6) { /* fucomip / fcomip */
            fcomi_flags(c, *st(c, 0), *st(c, i));
            fpop(c);
            return;
        }
        if (reg == 0) { /* ffreep */
            c->fp.empty |= (uint8_t)(1u << ((c->fp.top + i) & 7));
            fpop(c);
            return;
        }
        break;
    default:
        break;
    }
    snprintf(b, sizeof(b), "FPU x87 (%02X %02X)", op, m);
    unimpl(c, d, b);
}

/* fxsave / fxrstor (512 bytes) */
static void fxsave(Cpu *c, uint32_t a)
{
    for (uint32_t k = 0; k < 512; k += 4) {
        mwr(c, a + k, 4, 0);
    }
    mwr(c, a, 2, c->fpu_cw);
    mwr(c, a + 2, 2, fsw(c));
    mwr(c, a + 4, 1, (uint8_t)~c->fp.empty); /* etiqueta abreviada: 1 = válido */
    mwr(c, a + 24, 4, c->mxcsr);
    mwr(c, a + 28, 4, 0xFFBF);
    for (int k = 0; k < 8; k++) {
        if (c->fp.mmx) {
            wr64(c, a + 32 + 16 * (uint32_t)k, c->fp.mm[k]);
            mwr(c, a + 32 + 16 * (uint32_t)k + 8, 2, 0xFFFF);
        } else {
            wrf80(c, a + 32 + 16 * (uint32_t)k, *st(c, k));
        }
    }
    for (int k = 0; k < 8; k++) {
        for (int w = 0; w < 4; w++) {
            mwr(c, a + 160 + 16 * (uint32_t)k + 4 * (uint32_t)w, 4, c->fp.xmm[k].u[w]);
        }
    }
}

static void fxrstor(Cpu *c, uint32_t a)
{
    c->fpu_cw = (uint16_t)mrd(c, a, 2);
    uint16_t sw = (uint16_t)mrd(c, a + 2, 2);
    c->fpu_sw = sw & (uint16_t)~0x3800u;
    c->fp.top = (sw >> 11) & 7;
    c->fp.empty = (uint8_t)~mrd(c, a + 4, 1);
    c->mxcsr = mrd(c, a + 24, 4);
    for (int k = 0; k < 8; k++) {
        *st(c, k) = rdf80(c, a + 32 + 16 * (uint32_t)k);
        c->fp.mm[(c->fp.top + k) & 7] = rd64(c, a + 32 + 16 * (uint32_t)k);
    }
    for (int k = 0; k < 8; k++) {
        for (int w = 0; w < 4; w++) {
            c->fp.xmm[k].u[w] = mrd(c, a + 160 + 16 * (uint32_t)k + 4 * (uint32_t)w, 4);
        }
    }
}

/* ================================================================ SSE */

static X128 xmm_src(Cpu *c, const Dec *d, uint8_t m, int bytes)
{
    X128 v = {{0}};
    if ((m >> 6) == 3) {
        return c->fp.xmm[m & 7];
    }
    Ea e = modrm_ea(c, d, m);
    for (int w = 0; w < bytes / 4; w++) {
        v.u[w] = mrd(c, e.addr + 4 * (uint32_t)w, 4);
    }
    return v;
}

static void xmm_store(Cpu *c, const Dec *d, uint8_t m, const X128 *v, int bytes)
{
    if ((m >> 6) == 3) {
        c->fp.xmm[m & 7] = *v;
        return;
    }
    Ea e = modrm_ea(c, d, m);
    for (int w = 0; w < bytes / 4; w++) {
        mwr(c, e.addr + 4 * (uint32_t)w, 4, v->u[w]);
    }
}

static float sse_op(int op, float a, float b)
{
    switch (op) {
    case 0x58: return a + b;
    case 0x59: return a * b;
    case 0x5C: return a - b;
    case 0x5E: return a / b;
    case 0x5D: return a < b ? a : b; /* min: si no, el segundo */
    case 0x5F: return a > b ? a : b; /* max */
    case 0x51: return sqrtf(b);
    case 0x52: return 1.0f / sqrtf(b);
    default: return 1.0f / b; /* 0x53 rcp */
    }
}

static bool sse_cmp(int pred, float a, float b)
{
    bool un = isnan(a) || isnan(b);
    switch (pred & 7) {
    case 0: return !un && a == b;
    case 1: return !un && a < b;
    case 2: return !un && a <= b;
    case 3: return un;
    case 4: return un || a != b;
    case 5: return un || !(a < b);
    case 6: return un || !(a <= b);
    default: return !un;
    }
}

static int32_t sse_to_int(Cpu *c, float v, bool trunc_)
{
    return (int32_t)to_int(v, trunc_ ? 3 : (c->mxcsr >> 13), 32);
}

/* ================================================================ MMX */

static uint64_t mm_src(Cpu *c, const Dec *d, uint8_t m)
{
    if ((m >> 6) == 3) {
        return c->fp.mm[m & 7];
    }
    Ea e = modrm_ea(c, d, m);
    return rd64(c, e.addr);
}

static int64_t sat(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint64_t lane_get(uint64_t v, int bits, int i)
{
    return (v >> (bits * i)) & (bits == 64 ? ~0ull : ((1ull << bits) - 1));
}

static uint64_t lane_set(uint64_t v, int bits, int i, uint64_t x)
{
    uint64_t msk = bits == 64 ? ~0ull : ((1ull << bits) - 1);
    return (v & ~(msk << (bits * i))) | ((x & msk) << (bits * i));
}

static int64_t lane_s(uint64_t v, int bits, int i)
{
    uint64_t x = lane_get(v, bits, i);
    return bits == 64 ? (int64_t)x : (int64_t)(x << (64 - bits)) >> (64 - bits);
}

/* operaciones por carriles: devuelve false si no es una de estas */
static bool mmx_lanes(uint8_t op, uint64_t a, uint64_t b, uint64_t *out)
{
    int bits;
    uint64_t r = 0;
    switch (op) {
    case 0xFC: case 0xF8: case 0xEC: case 0xE8: case 0xDC: case 0xD8: case 0x74: case 0x64:
    case 0xDA: case 0xDE: case 0xE0: bits = 8; break;
    case 0xFD: case 0xF9: case 0xED: case 0xE9: case 0xDD: case 0xD9: case 0x75: case 0x65:
    case 0xD5: case 0xE5: case 0xE4: case 0xEA: case 0xEE: case 0xE3: bits = 16; break;
    case 0xFE: case 0xFA: case 0x76: case 0x66: bits = 32; break;
    case 0xDB: *out = a & b; return true;
    case 0xDF: *out = ~a & b; return true;
    case 0xEB: *out = a | b; return true;
    case 0xEF: *out = a ^ b; return true;
    default: return false;
    }
    int n = 64 / bits;
    int64_t smin = -(1ll << (bits - 1)), smax = (1ll << (bits - 1)) - 1;
    int64_t umax = (int64_t)((1ull << bits) - 1);
    for (int i = 0; i < n; i++) {
        uint64_t ua = lane_get(a, bits, i), ub = lane_get(b, bits, i);
        int64_t sa = lane_s(a, bits, i), sb = lane_s(b, bits, i);
        uint64_t x;
        switch (op) {
        case 0xFC: case 0xFD: case 0xFE: x = ua + ub; break;
        case 0xF8: case 0xF9: case 0xFA: x = ua - ub; break;
        case 0xEC: case 0xED: x = (uint64_t)sat(sa + sb, smin, smax); break;
        case 0xE8: case 0xE9: x = (uint64_t)sat(sa - sb, smin, smax); break;
        case 0xDC: case 0xDD: x = (uint64_t)sat((int64_t)(ua + ub), 0, umax); break;
        case 0xD8: case 0xD9: x = (uint64_t)sat((int64_t)ua - (int64_t)ub, 0, umax); break;
        case 0x74: case 0x75: case 0x76: x = ua == ub ? ~0ull : 0; break;
        case 0x64: case 0x65: case 0x66: x = sa > sb ? ~0ull : 0; break;
        case 0xD5: x = (uint64_t)(sa * sb); break;
        case 0xE5: x = (uint64_t)((sa * sb) >> 16); break;
        case 0xE4: x = (ua * ub) >> 16; break;
        case 0xDA: x = ua < ub ? ua : ub; break;
        case 0xDE: x = ua > ub ? ua : ub; break;
        case 0xEA: x = (uint64_t)(sa < sb ? sa : sb); break;
        case 0xEE: x = (uint64_t)(sa > sb ? sa : sb); break;
        case 0xE0: case 0xE3: x = (ua + ub + 1) >> 1; break;
        default: x = 0; break;
        }
        r = lane_set(r, bits, i, x);
    }
    *out = r;
    return true;
}

static uint64_t mmx_shift(uint8_t kind, int bits, uint64_t a, uint64_t cnt)
{
    /* kind: 2 lógico a la derecha, 4 aritmético a la derecha, 6 a la izquierda */
    uint64_t r = 0;
    int n = 64 / bits;
    for (int i = 0; i < n; i++) {
        uint64_t x;
        if (kind == 4) {
            int64_t s = lane_s(a, bits, i);
            x = (uint64_t)(s >> (cnt >= (uint64_t)bits ? bits - 1 : (int)cnt));
        } else if (cnt >= (uint64_t)bits) {
            x = 0;
        } else {
            uint64_t u = lane_get(a, bits, i);
            x = kind == 2 ? u >> cnt : u << cnt;
        }
        r = lane_set(r, bits, i, x);
    }
    return r;
}

static uint64_t unpack(uint64_t a, uint64_t b, int bits, bool high)
{
    uint64_t r = 0;
    int n = 32 / bits, base = high ? n : 0;
    for (int i = 0; i < n; i++) {
        r = lane_set(r, bits, 2 * i, lane_get(a, bits, base + i));
        r = lane_set(r, bits, 2 * i + 1, lane_get(b, bits, base + i));
    }
    return r;
}

static uint64_t pack(uint64_t a, uint64_t b, int bits, bool sign_sat_unsigned)
{
    /* de bits a bits/2: con signo (packss) o a sin signo (packus) */
    int half = bits / 2, n = 64 / bits;
    int64_t lo = sign_sat_unsigned ? 0 : -(1ll << (half - 1));
    int64_t hi = sign_sat_unsigned ? (1ll << half) - 1 : (1ll << (half - 1)) - 1;
    uint64_t r = 0;
    for (int i = 0; i < n; i++) {
        r = lane_set(r, half, i, (uint64_t)sat(lane_s(a, bits, i), lo, hi));
        r = lane_set(r, half, n + i, (uint64_t)sat(lane_s(b, bits, i), lo, hi));
    }
    return r;
}

/* Instrucciones 0F de SSE y MMX. false si no es una de estas. */
static bool sse_mmx(Cpu *c, const Dec *d, uint8_t op)
{
    bool ss = d->rep == 0xF3;
    if (d->opsize && op != 0x1F) {
        return false; /* SSE2 (66): el Pentium III no la tiene */
    }
    switch (op) {
    case 0x10: case 0x11: { /* movups / movss */
        uint8_t m = fetch8(c);
        int x = (m >> 3) & 7;
        if (op == 0x10) {
            if (ss) {
                X128 v = xmm_src(c, d, m, 4);
                if ((m >> 6) == 3) {
                    c->fp.xmm[x].u[0] = v.u[0];
                } else {
                    c->fp.xmm[x] = v; /* desde memoria: el resto a cero */
                }
            } else {
                c->fp.xmm[x] = xmm_src(c, d, m, 16);
            }
        } else if (ss) {
            if ((m >> 6) == 3) {
                c->fp.xmm[m & 7].u[0] = c->fp.xmm[x].u[0];
            } else {
                xmm_store(c, d, m, &c->fp.xmm[x], 4);
            }
        } else {
            xmm_store(c, d, m, &c->fp.xmm[x], 16);
        }
        return true;
    }
    case 0x28: case 0x29: case 0x2B: { /* movaps / movntps */
        uint8_t m = fetch8(c);
        int x = (m >> 3) & 7;
        if (op == 0x28) {
            c->fp.xmm[x] = xmm_src(c, d, m, 16);
        } else {
            xmm_store(c, d, m, &c->fp.xmm[x], 16);
        }
        return true;
    }
    case 0x12: case 0x13: case 0x16: case 0x17: { /* movlps movhlps / movhps movlhps */
        uint8_t m = fetch8(c);
        int x = (m >> 3) & 7;
        bool high = op >= 0x16;
        if (op == 0x12 || op == 0x16) {
            if ((m >> 6) == 3) {
                X128 s = c->fp.xmm[m & 7];
                c->fp.xmm[x].q[high ? 1 : 0] = s.q[high ? 0 : 1];
            } else {
                Ea e = modrm_ea(c, d, m);
                c->fp.xmm[x].q[high ? 1 : 0] = rd64(c, e.addr);
            }
        } else {
            Ea e = modrm_ea(c, d, m);
            if (!e.reg) {
                wr64(c, e.addr, c->fp.xmm[x].q[high ? 1 : 0]);
            }
        }
        return true;
    }
    case 0x14: case 0x15: { /* unpcklps / unpckhps */
        uint8_t m = fetch8(c);
        X128 a = c->fp.xmm[(m >> 3) & 7], b = xmm_src(c, d, m, 16), r;
        int base = op == 0x14 ? 0 : 2;
        r.u[0] = a.u[base];
        r.u[1] = b.u[base];
        r.u[2] = a.u[base + 1];
        r.u[3] = b.u[base + 1];
        c->fp.xmm[(m >> 3) & 7] = r;
        return true;
    }
    case 0x2A: { /* cvtpi2ps / cvtsi2ss */
        uint8_t m = fetch8(c);
        int x = (m >> 3) & 7;
        if (ss) {
            Ea e = modrm_ea(c, d, m);
            c->fp.xmm[x].f[0] = (float)(int32_t)ea_get(c, &e, 4);
        } else {
            uint64_t v = mm_src(c, d, m);
            c->fp.xmm[x].f[0] = (float)(int32_t)(uint32_t)v;
            c->fp.xmm[x].f[1] = (float)(int32_t)(uint32_t)(v >> 32);
        }
        return true;
    }
    case 0x2C: case 0x2D: { /* cvt(t)ps2pi / cvt(t)ss2si */
        uint8_t m = fetch8(c);
        int x = (m >> 3) & 7;
        bool tr = op == 0x2C;
        if (ss) {
            X128 v = xmm_src(c, d, m, 4);
            c->r[x] = (uint32_t)sse_to_int(c, v.f[0], tr);
        } else {
            X128 v = xmm_src(c, d, m, 8);
            c->fp.mm[x] = (uint64_t)(uint32_t)sse_to_int(c, v.f[0], tr) |
                          ((uint64_t)(uint32_t)sse_to_int(c, v.f[1], tr) << 32);
        }
        return true;
    }
    case 0x2E: case 0x2F: { /* ucomiss / comiss */
        uint8_t m = fetch8(c);
        X128 v = xmm_src(c, d, m, 4);
        fcomi_flags(c, c->fp.xmm[(m >> 3) & 7].f[0], v.f[0]);
        return true;
    }
    case 0x50: { /* movmskps */
        uint8_t m = fetch8(c);
        X128 v = c->fp.xmm[m & 7];
        c->r[(m >> 3) & 7] = (v.u[0] >> 31) | ((v.u[1] >> 31) << 1) | ((v.u[2] >> 31) << 2) | ((v.u[3] >> 31) << 3);
        return true;
    }
    case 0x51: case 0x52: case 0x53: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint8_t m = fetch8(c);
        int x = (m >> 3) & 7;
        X128 b = xmm_src(c, d, m, ss ? 4 : 16);
        X128 *a = &c->fp.xmm[x];
        for (int w = 0; w < (ss ? 1 : 4); w++) {
            a->f[w] = sse_op(op, a->f[w], b.f[w]);
        }
        return true;
    }
    case 0x54: case 0x55: case 0x56: case 0x57: { /* andps andnps orps xorps */
        uint8_t m = fetch8(c);
        X128 b = xmm_src(c, d, m, 16);
        X128 *a = &c->fp.xmm[(m >> 3) & 7];
        for (int w = 0; w < 4; w++) {
            a->u[w] = op == 0x54 ? a->u[w] & b.u[w] : op == 0x55 ? ~a->u[w] & b.u[w]
                    : op == 0x56 ? a->u[w] | b.u[w] : a->u[w] ^ b.u[w];
        }
        return true;
    }
    case 0xC2: { /* cmpps / cmpss */
        uint8_t m = fetch8(c);
        X128 b = xmm_src(c, d, m, ss ? 4 : 16);
        uint8_t pred = fetch8(c);
        X128 *a = &c->fp.xmm[(m >> 3) & 7];
        for (int w = 0; w < (ss ? 1 : 4); w++) {
            a->u[w] = sse_cmp(pred, a->f[w], b.f[w]) ? 0xFFFFFFFFu : 0;
        }
        return true;
    }
    case 0xC6: { /* shufps */
        uint8_t m = fetch8(c);
        X128 b = xmm_src(c, d, m, 16);
        uint8_t imm = fetch8(c);
        X128 a = c->fp.xmm[(m >> 3) & 7], r;
        r.u[0] = a.u[imm & 3];
        r.u[1] = a.u[(imm >> 2) & 3];
        r.u[2] = b.u[(imm >> 4) & 3];
        r.u[3] = b.u[(imm >> 6) & 3];
        c->fp.xmm[(m >> 3) & 7] = r;
        return true;
    }
    /* ---- MMX ---- */
    case 0x6E: { /* movd mm, r/m32 */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        c->fp.mm[(m >> 3) & 7] = ea_get(c, &e, 4);
        c->fp.mmx = true;
        return true;
    }
    case 0x7E: { /* movd r/m32, mm */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        ea_set(c, &e, 4, (uint32_t)c->fp.mm[(m >> 3) & 7]);
        return true;
    }
    case 0x6F: { /* movq mm, mm/m64 */
        uint8_t m = fetch8(c);
        c->fp.mm[(m >> 3) & 7] = mm_src(c, d, m);
        c->fp.mmx = true;
        return true;
    }
    case 0x7F: case 0xE7: { /* movq / movntq mm/m64, mm */
        uint8_t m = fetch8(c);
        uint64_t v = c->fp.mm[(m >> 3) & 7];
        if ((m >> 6) == 3) {
            c->fp.mm[m & 7] = v;
        } else {
            Ea e = modrm_ea(c, d, m);
            wr64(c, e.addr, v);
        }
        return true;
    }
    case 0x77: /* emms */
        c->fp.empty = 0xFF;
        c->fp.mmx = false;
        return true;
    case 0x60: case 0x61: case 0x62: case 0x68: case 0x69: case 0x6A: { /* punpckl/h bw wd dq */
        uint8_t m = fetch8(c);
        uint64_t b = mm_src(c, d, m);
        uint64_t *a = &c->fp.mm[(m >> 3) & 7];
        int bits = (op & 3) == 0 ? 8 : (op & 3) == 1 ? 16 : 32;
        *a = unpack(*a, b, bits, op >= 0x68);
        return true;
    }
    case 0x63: case 0x67: case 0x6B: { /* packsswb packuswb packssdw */
        uint8_t m = fetch8(c);
        uint64_t b = mm_src(c, d, m);
        uint64_t *a = &c->fp.mm[(m >> 3) & 7];
        *a = pack(*a, b, op == 0x6B ? 32 : 16, op == 0x67);
        return true;
    }
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3: { /* desplazamientos */
        uint8_t m = fetch8(c);
        uint64_t cnt = mm_src(c, d, m);
        uint64_t *a = &c->fp.mm[(m >> 3) & 7];
        uint8_t kind = op < 0xE0 ? 2 : op < 0xF0 ? 4 : 6;
        int bits = 16 << ((op & 3) - 1);
        *a = mmx_shift(kind, bits, *a, cnt);
        return true;
    }
    case 0x71: case 0x72: case 0x73: { /* desplazamientos con inmediato */
        uint8_t m = fetch8(c);
        uint8_t imm = fetch8(c);
        int sub = (m >> 3) & 7;
        int bits = op == 0x71 ? 16 : op == 0x72 ? 32 : 64;
        if ((m >> 6) != 3 || (sub != 2 && sub != 4 && sub != 6) || (bits == 64 && sub == 4)) {
            return false;
        }
        c->fp.mm[m & 7] = mmx_shift((uint8_t)sub, bits, c->fp.mm[m & 7], imm);
        return true;
    }
    case 0x70: { /* pshufw */
        uint8_t m = fetch8(c);
        uint64_t b = mm_src(c, d, m);
        uint8_t imm = fetch8(c);
        uint64_t r = 0;
        for (int i = 0; i < 4; i++) {
            r = lane_set(r, 16, i, lane_get(b, 16, (imm >> (2 * i)) & 3));
        }
        c->fp.mm[(m >> 3) & 7] = r;
        return true;
    }
    case 0xC4: { /* pinsrw mm, r32/m16, imm */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        uint32_t v = e.reg ? c->r[e.r] : mrd(c, e.addr, 2);
        uint8_t imm = fetch8(c);
        c->fp.mm[(m >> 3) & 7] = lane_set(c->fp.mm[(m >> 3) & 7], 16, imm & 3, v);
        return true;
    }
    case 0xC5: { /* pextrw r32, mm, imm */
        uint8_t m = fetch8(c);
        uint8_t imm = fetch8(c);
        c->r[(m >> 3) & 7] = (uint32_t)lane_get(c->fp.mm[m & 7], 16, imm & 3);
        return true;
    }
    case 0xD7: { /* pmovmskb */
        uint8_t m = fetch8(c);
        uint64_t v = c->fp.mm[m & 7];
        uint32_t r = 0;
        for (int i = 0; i < 8; i++) {
            r |= (uint32_t)((v >> (8 * i + 7)) & 1) << i;
        }
        c->r[(m >> 3) & 7] = r;
        return true;
    }
    case 0xF5: { /* pmaddwd */
        uint8_t m = fetch8(c);
        uint64_t b = mm_src(c, d, m);
        uint64_t a = c->fp.mm[(m >> 3) & 7];
        uint64_t r = 0;
        for (int i = 0; i < 2; i++) {
            int32_t s = (int32_t)(lane_s(a, 16, 2 * i) * lane_s(b, 16, 2 * i) +
                                  lane_s(a, 16, 2 * i + 1) * lane_s(b, 16, 2 * i + 1));
            r = lane_set(r, 32, i, (uint32_t)s);
        }
        c->fp.mm[(m >> 3) & 7] = r;
        return true;
    }
    case 0xF6: { /* psadbw */
        uint8_t m = fetch8(c);
        uint64_t b = mm_src(c, d, m);
        uint64_t a = c->fp.mm[(m >> 3) & 7];
        uint32_t sum = 0;
        for (int i = 0; i < 8; i++) {
            int x = (int)lane_get(a, 8, i) - (int)lane_get(b, 8, i);
            sum += (uint32_t)(x < 0 ? -x : x);
        }
        c->fp.mm[(m >> 3) & 7] = sum;
        return true;
    }
    default: {
        uint64_t r;
        if ((op >= 0x60 && op <= 0x7F) || op >= 0xD0) {
            uint32_t save = c->eip;
            uint8_t m = fetch8(c);
            uint64_t b = mm_src(c, d, m);
            if (mmx_lanes(op, c->fp.mm[(m >> 3) & 7], b, &r)) {
                c->fp.mm[(m >> 3) & 7] = r;
                return true;
            }
            c->eip = save;
        }
        return false;
    }
    }
}
