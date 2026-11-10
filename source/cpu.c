/*
 * XboxNX — intérprete x86 de 32 bits.
 *
 * Implementación propia. Modo plano de 32 bits como en el Xbox: todos los
 * segmentos con base 0 salvo fs (el KPCR). Flags calculados en cada
 * instrucción (simple y exacto; la velocidad llega con FEX).
 */
#include "cpu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__aarch64__)
#include <stddef.h>
#ifdef __SWITCH__
#include <switch/types.h>
#include <switch/result.h>
#include <switch/arm/cache.h>
#include <switch/kernel/jit.h>
#else
#include <sys/mman.h>
#endif
#endif
#include <string.h>

/* ---- memoria ---- */

static void fault(Cpu *c, uint32_t addr, bool write)
{
    if (c->stop == CPU_RUNNING) {
        c->stop = CPU_FAULT;
        c->fault_addr = addr;
        c->fault_write = write;
    }
}

/* La memoria de va (válida hasta el final de su página) o NULL: la tabla
 * de páginas leída aquí mismo, sin llamar a vm.c (es lo más frecuente). */
static inline uint8_t *hp(const Cpu *c, uint32_t a)
{
    uint32_t e = c->vm->pt[a >> 12];
    return e ? c->vm->ram + ((size_t)(e - 1) << 12) + (a & 0xFFFu) : NULL;
}

static uint32_t mrd(Cpu *c, uint32_t a, int sz)
{
    uint8_t *h = hp(c, a);
    if (h && (a & (VM_PAGE - 1)) + (uint32_t)sz <= VM_PAGE) {
        if (sz == 1) {
            return h[0];
        }
        if (sz == 2) {
            return (uint32_t)h[0] | ((uint32_t)h[1] << 8);
        }
        return (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    }
    uint8_t b[4] = {0};
    if (!vm_read(c->vm, a, b, (size_t)sz)) {
        uint32_t v = 0;
        if (c->mmio && c->mmio(c->dev, a, sz, false, &v)) {
            return v;
        }
        fault(c, a, false);
        return 0;
    }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void mwr(Cpu *c, uint32_t a, int sz, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    uint8_t *h = hp(c, a);
    if (h && (a & (VM_PAGE - 1)) + (uint32_t)sz <= VM_PAGE) {
        memcpy(h, b, (size_t)sz);
        /* código escrito: otra traducción (los datos de la misma página no) */
        vm_touch_range(c->vm, (uint32_t)((size_t)(h - c->vm->ram) >> 12), a & 0xFFFu, (uint32_t)sz);
        return;
    }
    if (!vm_write(c->vm, a, b, (size_t)sz)) {
        if (c->mmio && c->mmio(c->dev, a, sz, true, &v)) {
            return;
        }
        fault(c, a, true);
    }
}

/* ---- decodificación ---- */

typedef struct Dec {
    uint32_t start;   /* EIP de la instrucción */
    bool opsize;      /* 0x66 */
    bool adsize;      /* 0x67 */
    bool fs;          /* 0x64 */
    int rep;          /* 0, 0xF2, 0xF3 */
} Dec;

/* El código se lee de la página en curso, guardada (fetch_page / fetch_host);
 * cpu_run la olvida al empezar, por si el kernel cambió las páginas. */
static inline const uint8_t *code_at(Cpu *c, uint32_t eip, unsigned n)
{
    if ((eip >> 12) != c->fetch_page || !c->fetch_host) {
        uint8_t *h = hp(c, eip);
        if (!h) {
            return NULL;
        }
        c->fetch_page = eip >> 12;
        c->fetch_host = h - (eip & 0xFFFu);
    }
    return (eip & 0xFFFu) + n <= 0x1000u ? c->fetch_host + (eip & 0xFFFu) : NULL;
}

static inline uint8_t fetch8(Cpu *c)
{
    const uint8_t *p = code_at(c, c->eip, 1);
    uint8_t v = p ? p[0] : (uint8_t)mrd(c, c->eip, 1);
    c->eip++;
    return v;
}
static inline uint16_t fetch16(Cpu *c)
{
    const uint8_t *p = code_at(c, c->eip, 2);
    uint16_t v = p ? (uint16_t)(p[0] | p[1] << 8) : (uint16_t)mrd(c, c->eip, 2);
    c->eip += 2;
    return v;
}
static inline uint32_t fetch32(Cpu *c)
{
    const uint8_t *p = code_at(c, c->eip, 4);
    uint32_t v = p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
                   : mrd(c, c->eip, 4);
    c->eip += 4;
    return v;
}

static void unimpl(Cpu *c, const Dec *d, const char *what)
{
    if (c->stop == CPU_RUNNING) {
        c->stop = CPU_UNIMPL;
        snprintf(c->why, sizeof(c->why), "%s", what);
        c->eip = d->start;
    }
}

typedef struct Ea {
    bool reg;
    int r;
    uint32_t addr;
} Ea;

static Ea modrm_ea(Cpu *c, const Dec *d, uint8_t m)
{
    Ea e = {0};
    int mod = m >> 6, rm = m & 7;
    if (mod == 3) {
        e.reg = true;
        e.r = rm;
        return e;
    }
    uint32_t a = 0;
    if (rm == 4) {
        uint8_t sib = fetch8(c);
        int scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (base == 5 && mod == 0) {
            a = fetch32(c);
        } else {
            a = c->r[base];
        }
        if (idx != 4) {
            a += c->r[idx] << scale;
        }
    } else if (rm == 5 && mod == 0) {
        a = fetch32(c);
    } else {
        a = c->r[rm];
    }
    if (mod == 1) {
        a += (uint32_t)(int32_t)(int8_t)fetch8(c);
    } else if (mod == 2) {
        a += fetch32(c);
    }
    if (d->fs) {
        a += c->fs_base;
    }
    e.addr = a;
    return e;
}

static uint32_t mask_of(int sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
static uint32_t sign_of(int sz) { return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u; }
static int32_t sext(uint32_t v, int sz)
{
    return sz == 1 ? (int32_t)(int8_t)v : sz == 2 ? (int32_t)(int16_t)v : (int32_t)v;
}

static uint32_t reg_get(Cpu *c, int r, int sz)
{
    if (sz == 1) {
        return r < 4 ? (c->r[r] & 0xFF) : ((c->r[r - 4] >> 8) & 0xFF);
    }
    if (sz == 2) {
        return c->r[r] & 0xFFFF;
    }
    return c->r[r];
}

static void reg_set(Cpu *c, int r, int sz, uint32_t v)
{
    if (sz == 1) {
        if (r < 4) {
            c->r[r] = (c->r[r] & ~0xFFu) | (v & 0xFF);
        } else {
            c->r[r - 4] = (c->r[r - 4] & ~0xFF00u) | ((v & 0xFF) << 8);
        }
    } else if (sz == 2) {
        c->r[r] = (c->r[r] & ~0xFFFFu) | (v & 0xFFFF);
    } else {
        c->r[r] = v;
    }
}

static uint32_t ea_get(Cpu *c, const Ea *e, int sz)
{
    return e->reg ? reg_get(c, e->r, sz) : mrd(c, e->addr, sz);
}

static void ea_set(Cpu *c, const Ea *e, int sz, uint32_t v)
{
    if (e->reg) {
        reg_set(c, e->r, sz, v);
    } else {
        mwr(c, e->addr, sz, v);
    }
}

/* ---- pila ---- */

static void push(Cpu *c, int sz, uint32_t v)
{
    c->r[R_ESP] -= (uint32_t)sz;
    mwr(c, c->r[R_ESP], sz, v);
}

static uint32_t pop(Cpu *c, int sz)
{
    uint32_t v = mrd(c, c->r[R_ESP], sz);
    c->r[R_ESP] += (uint32_t)sz;
    return v;
}

/* ---- flags ---- */

/* Flags perezosos: alu() guarda la operación y sus valores (lf_*) y los
 * flags aritméticos (CF PF AF ZF SF OF) se calculan solo cuando alguien los
 * lee; casi siempre la siguiente instrucción los vuelve a pisar. FL(c) da
 * los flags ya calculados; fuera del intérprete (cpu_run, cpu_step) están
 * siempre calculados. */
/* LF_SHL/SHR/SAR (solo los pone el JIT, 32 bits, cuenta 1..31): a = valor,
 * b = cuenta, res = resultado; AF a cero como en shift(). */
enum { LF_NONE, LF_ADD, LF_SUB, LF_LOGIC, LF_INC, LF_DEC, LF_SHL, LF_SHR, LF_SAR };
static inline uint32_t shift_cf(unsigned op, uint32_t a, uint32_t cnt, unsigned bits)
{
    if (op == LF_SHL) {
        return cnt <= bits ? (a >> (bits - cnt)) & 1 : 0;
    }
    if (op == LF_SHR) {
        return cnt <= bits ? (a >> (cnt - 1)) & 1 : 0;
    }
    int32_t sa = (int32_t)(a << (32 - bits)) >> (32 - bits);
    return cnt >= bits ? (uint32_t)(sa < 0) : (uint32_t)((sa >> (cnt - 1)) & 1);
}

static void flags_sync(Cpu *c)
{
    unsigned sz = c->lf_sz;
    uint32_t m = sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t s = sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u;
    uint32_t a = c->lf_a, b = c->lf_b, res = c->lf_res, cin = c->lf_cin;
    uint32_t f = c->flags & ~(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF);
    switch (c->lf_op) {
    case LF_ADD:
        f |= ((uint64_t)a + b + cin > m) ? F_CF : 0;
        f |= ((a ^ res) & (b ^ res) & s) ? F_OF : 0;
        f |= ((a ^ b ^ res) & 0x10) ? F_AF : 0;
        break;
    case LF_SUB:
        f |= ((uint64_t)a < (uint64_t)b + cin) ? F_CF : 0;
        f |= ((a ^ b) & (a ^ res) & s) ? F_OF : 0;
        f |= ((a ^ b ^ res) & 0x10) ? F_AF : 0;
        break;
    case LF_INC: /* CF se conserva (en cin) */
    case LF_DEC:
        f |= cin ? F_CF : 0;
        if (c->lf_op == LF_INC) {
            f |= ((a ^ res) & (b ^ res) & s) ? F_OF : 0;
        } else {
            f |= ((a ^ b) & (a ^ res) & s) ? F_OF : 0;
        }
        f |= ((a ^ b ^ res) & 0x10) ? F_AF : 0;
        break;
    case LF_LOGIC:
        break;
    case LF_SHL:
    case LF_SHR:
    case LF_SAR: {
        uint32_t cf = shift_cf(c->lf_op, a, b, sz * 8u);
        f |= cf ? F_CF : 0;
        if (c->lf_op == LF_SHL) {
            f |= (((res & s) != 0) != (cf != 0)) ? F_OF : 0;
        } else if (c->lf_op == LF_SHR) {
            f |= (a & s) ? F_OF : 0;
        }
        break;
    }
    default:
        return;
    }
    f |= res == 0 ? F_ZF : 0;
    f |= (res & s) ? F_SF : 0;
    f |= __builtin_parity(res & 0xFF) ? 0 : F_PF;
    c->flags = f;
    c->lf_op = LF_NONE;
}

/* Solo CF (adc, sbb, inc, dec lo necesitan sin calcular los demás). */
static inline uint32_t lazy_cf(Cpu *c)
{
    switch (c->lf_op) {
    case LF_ADD: {
        uint32_t m = c->lf_sz == 1 ? 0xFFu : c->lf_sz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        return (uint64_t)c->lf_a + c->lf_b + c->lf_cin > m;
    }
    case LF_SUB: return (uint64_t)c->lf_a < (uint64_t)c->lf_b + c->lf_cin;
    case LF_INC:
    case LF_DEC: return c->lf_cin;
    case LF_LOGIC: return 0;
    case LF_SHL:
    case LF_SHR:
    case LF_SAR: return shift_cf(c->lf_op, c->lf_a, c->lf_b, c->lf_sz * 8u);
    default: return (c->flags & F_CF) ? 1 : 0;
    }
}

static inline uint32_t *flags_ref(Cpu *c)
{
    if (c->lf_op) {
        flags_sync(c);
    }
    return &c->flags;
}
#define FL(c) (*flags_ref(c))

static void set_flag(Cpu *c, uint32_t f, bool on)
{
    if (on) {
        FL(c) |= f;
    } else {
        FL(c) &= ~f;
    }
}

static void szp(Cpu *c, uint32_t res, int sz)
{
    res &= mask_of(sz);
    set_flag(c, F_ZF, res == 0);
    set_flag(c, F_SF, (res & sign_of(sz)) != 0);
    set_flag(c, F_PF, !__builtin_parity(res & 0xFF));
}

enum { OP_ADD, OP_OR, OP_ADC, OP_SBB, OP_AND, OP_SUB, OP_XOR, OP_CMP };

static uint32_t alu(Cpu *c, int op, uint32_t a, uint32_t b, int sz)
{
    uint32_t m = mask_of(sz);
    a &= m;
    b &= m;
    uint32_t res, cin = 0;
    uint8_t kind;
    switch (op) {
    case OP_ADD:
    case OP_ADC:
        if (op == OP_ADC) {
            cin = lazy_cf(c);
        }
        res = (a + b + cin) & m;
        kind = LF_ADD;
        break;
    case OP_SUB:
    case OP_SBB:
    case OP_CMP:
        if (op == OP_SBB) {
            cin = lazy_cf(c);
        }
        res = (a - b - cin) & m;
        kind = LF_SUB;
        break;
    case OP_OR: res = a | b; kind = LF_LOGIC; break;
    case OP_AND: res = a & b; kind = LF_LOGIC; break;
    default: res = a ^ b; kind = LF_LOGIC; break;
    }
    c->lf_op = kind;
    c->lf_sz = (uint8_t)sz;
    c->lf_a = a;
    c->lf_b = b;
    c->lf_res = res;
    c->lf_cin = (uint8_t)cin;
    return res;
}

/* inc / dec: como add / sub de 1 sin tocar CF */
static uint32_t incdec(Cpu *c, bool dec, uint32_t a, int sz)
{
    uint32_t m = mask_of(sz);
    uint8_t cf = (uint8_t)lazy_cf(c);
    a &= m;
    uint32_t res = (dec ? a - 1 : a + 1) & m;
    c->lf_op = dec ? LF_DEC : LF_INC;
    c->lf_sz = (uint8_t)sz;
    c->lf_a = a;
    c->lf_b = 1;
    c->lf_res = res;
    c->lf_cin = cf;
    return res;
}

static bool cond(Cpu *c, int n)
{
    bool cf, zf, sf, of, pf = false;
    if ((c->lf_op == LF_SUB || c->lf_op == LF_LOGIC) && (n >> 1) != 5) {
        /* cmp / test seguidos de jcc: sin calcular todos los flags */
        uint32_t s = c->lf_sz == 1 ? 0x80u : c->lf_sz == 2 ? 0x8000u : 0x80000000u;
        uint32_t a = c->lf_a, b = c->lf_b, res = c->lf_res;
        zf = res == 0;
        sf = (res & s) != 0;
        if (c->lf_op == LF_SUB) {
            cf = (uint64_t)a < (uint64_t)b + c->lf_cin;
            of = ((a ^ b) & (a ^ res) & s) != 0;
        } else {
            cf = of = false;
        }
    } else {
        uint32_t f = FL(c);
        cf = f & F_CF, zf = f & F_ZF, sf = f & F_SF, of = f & F_OF, pf = f & F_PF;
    }
    bool r;
    switch (n >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf || (sf != of); break;
    }
    return (n & 1) ? !r : r;
}

/* rol ror rcl rcr shl shr sal sar */
static uint32_t shift(Cpu *c, int op, uint32_t a, unsigned cnt, int sz)
{
    unsigned bits = (unsigned)sz * 8;
    uint32_t m = mask_of(sz), s = sign_of(sz);
    a &= m;
    cnt &= 31;
    if (!cnt) {
        return a;
    }
    uint32_t res = a;
    bool cf;
    switch (op) {
    case 0: { /* rol */
        unsigned n = cnt % bits;
        res = n ? ((a << n) | (a >> (bits - n))) & m : a;
        cf = res & 1;
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, ((res & s) != 0) != cf);
        return res;
    }
    case 1: { /* ror */
        unsigned n = cnt % bits;
        res = n ? ((a >> n) | (a << (bits - n))) & m : a;
        set_flag(c, F_CF, (res & s) != 0);
        set_flag(c, F_OF, ((res & s) != 0) != ((res & (s >> 1)) != 0));
        return res;
    }
    case 2: { /* rcl */
        unsigned n = cnt % (bits + 1);
        bool carry = FL(c) & F_CF;
        for (unsigned i = 0; i < n; i++) {
            bool out = (res & s) != 0;
            res = ((res << 1) | (carry ? 1u : 0u)) & m;
            carry = out;
        }
        set_flag(c, F_CF, carry);
        set_flag(c, F_OF, ((res & s) != 0) != carry);
        return res;
    }
    case 3: { /* rcr */
        unsigned n = cnt % (bits + 1);
        bool carry = FL(c) & F_CF;
        set_flag(c, F_OF, ((res & s) != 0) != carry);
        for (unsigned i = 0; i < n; i++) {
            bool out = res & 1;
            res = (res >> 1) | (carry ? s : 0);
            carry = out;
        }
        set_flag(c, F_CF, carry);
        return res;
    }
    case 4:
    case 6: /* shl */
        cf = cnt <= bits ? ((a >> (bits - cnt)) & 1) : 0;
        res = cnt < 32 ? (a << cnt) & m : 0;
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, ((res & s) != 0) != cf);
        break;
    case 5: /* shr */
        cf = (cnt <= bits) ? ((a >> (cnt - 1)) & 1) : 0;
        res = cnt < 32 ? (a >> cnt) : 0;
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, (a & s) != 0);
        break;
    default: { /* sar */
        int32_t sa = sext(a, sz);
        unsigned n = cnt > bits - 1 ? bits - 1 : cnt;
        cf = cnt >= bits ? (sa < 0) : ((sa >> (cnt - 1)) & 1);
        res = (uint32_t)(sa >> n) & m;
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, false);
        break;
    }
    }
    FL(c) &= ~F_AF;
    szp(c, res, sz);
    return res;
}

/* ---- instrucciones de cadena ---- */

static void string_op(Cpu *c, const Dec *d, uint8_t op, int sz)
{
    int32_t step = (FL(c) & F_DF) ? -sz : sz;
    bool rep = d->rep != 0;
    uint32_t src_base = d->fs ? c->fs_base : 0;
    for (;;) {
        if (rep && c->r[R_ECX] == 0) {
            break;
        }
        switch (op) {
        case 0xA4: case 0xA5: /* movs */
            mwr(c, c->r[R_EDI], sz, mrd(c, src_base + c->r[R_ESI], sz));
            c->r[R_ESI] += (uint32_t)step;
            c->r[R_EDI] += (uint32_t)step;
            break;
        case 0xA6: case 0xA7: /* cmps */
            alu(c, OP_CMP, mrd(c, src_base + c->r[R_ESI], sz), mrd(c, c->r[R_EDI], sz), sz);
            c->r[R_ESI] += (uint32_t)step;
            c->r[R_EDI] += (uint32_t)step;
            break;
        case 0xAA: case 0xAB: /* stos */
            mwr(c, c->r[R_EDI], sz, reg_get(c, R_EAX, sz));
            c->r[R_EDI] += (uint32_t)step;
            break;
        case 0xAC: case 0xAD: /* lods */
            reg_set(c, R_EAX, sz, mrd(c, src_base + c->r[R_ESI], sz));
            c->r[R_ESI] += (uint32_t)step;
            break;
        default: /* scas */
            alu(c, OP_CMP, reg_get(c, R_EAX, sz), mrd(c, c->r[R_EDI], sz), sz);
            c->r[R_EDI] += (uint32_t)step;
            break;
        }
        if (c->stop != CPU_RUNNING) {
            return;
        }
        if (!rep) {
            break;
        }
        c->r[R_ECX]--;
        bool cmp = op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF;
        if (cmp && d->rep == 0xF3 && !(FL(c) & F_ZF)) {
            break;
        }
        if (cmp && d->rep == 0xF2 && (FL(c) & F_ZF)) {
            break;
        }
    }
}

/* ---- grupo 3: test/not/neg/mul/imul/div/idiv ---- */

static void group3(Cpu *c, const Dec *d, int sz, uint8_t m)
{
    Ea e = modrm_ea(c, d, m);
    int op = (m >> 3) & 7;
    uint32_t msk = mask_of(sz);
    switch (op) {
    case 0:
    case 1: { /* test */
        uint32_t imm = sz == 1 ? fetch8(c) : sz == 2 ? fetch16(c) : fetch32(c);
        alu(c, OP_AND, ea_get(c, &e, sz), imm, sz);
        return;
    }
    case 2:
        ea_set(c, &e, sz, ~ea_get(c, &e, sz) & msk);
        return;
    case 3: {
        uint32_t v = ea_get(c, &e, sz);
        uint32_t res = alu(c, OP_SUB, 0, v, sz);
        set_flag(c, F_CF, (v & msk) != 0);
        ea_set(c, &e, sz, res);
        return;
    }
    case 4:
    case 5: { /* mul / imul */
        uint32_t v = ea_get(c, &e, sz);
        bool over;
        if (sz == 1) {
            uint32_t al = c->r[R_EAX] & 0xFF;
            uint32_t res = op == 4 ? al * v : (uint32_t)((int32_t)(int8_t)al * (int32_t)(int8_t)v);
            reg_set(c, R_EAX, 2, res);
            over = op == 4 ? (res & 0xFF00) != 0 : (int32_t)(int16_t)res != (int32_t)(int8_t)res;
            szp(c, res, 1);
        } else if (sz == 2) {
            uint32_t ax = c->r[R_EAX] & 0xFFFF;
            uint32_t res = op == 4 ? ax * v : (uint32_t)((int32_t)(int16_t)ax * (int32_t)(int16_t)v);
            reg_set(c, R_EAX, 2, res);
            reg_set(c, R_EDX, 2, res >> 16);
            over = op == 4 ? (res >> 16) != 0 : (int32_t)res != (int32_t)(int16_t)res;
            szp(c, res, 2);
        } else {
            uint64_t res = op == 4 ? (uint64_t)c->r[R_EAX] * v
                                   : (uint64_t)((int64_t)(int32_t)c->r[R_EAX] * (int64_t)(int32_t)v);
            c->r[R_EAX] = (uint32_t)res;
            c->r[R_EDX] = (uint32_t)(res >> 32);
            over = op == 4 ? (res >> 32) != 0 : (int64_t)res != (int64_t)(int32_t)res;
            szp(c, (uint32_t)res, 4);
        }
        set_flag(c, F_CF, over);
        set_flag(c, F_OF, over);
        return;
    }
    default: { /* div / idiv */
        uint32_t v = ea_get(c, &e, sz);
        if (c->stop != CPU_RUNNING) {
            return;
        }
        if ((v & msk) == 0) {
            c->stop = CPU_DIVIDE;
            c->eip = d->start;
            return;
        }
        if (sz == 1) {
            uint32_t ax = c->r[R_EAX] & 0xFFFF;
            if (op == 6) {
                uint32_t q = ax / v, r = ax % v;
                if (q > 0xFF) goto ovf;
                reg_set(c, R_EAX, 1, q);
                reg_set(c, 4, 1, r); /* AH */
            } else {
                int32_t n = (int16_t)ax, dv = (int8_t)v;
                int32_t q = n / dv, r = n % dv;
                if (q > 127 || q < -128) goto ovf;
                reg_set(c, R_EAX, 1, (uint32_t)q);
                reg_set(c, 4, 1, (uint32_t)r);
            }
        } else if (sz == 2) {
            uint32_t n = ((c->r[R_EDX] & 0xFFFF) << 16) | (c->r[R_EAX] & 0xFFFF);
            if (op == 6) {
                uint32_t q = n / v, r = n % v;
                if (q > 0xFFFF) goto ovf;
                reg_set(c, R_EAX, 2, q);
                reg_set(c, R_EDX, 2, r);
            } else {
                int32_t sn = (int32_t)n, dv = (int16_t)v;
                if (sn == INT32_MIN && dv == -1) goto ovf;
                int32_t q = sn / dv, r = sn % dv;
                if (q > 32767 || q < -32768) goto ovf;
                reg_set(c, R_EAX, 2, (uint32_t)q);
                reg_set(c, R_EDX, 2, (uint32_t)r);
            }
        } else {
            uint64_t n = ((uint64_t)c->r[R_EDX] << 32) | c->r[R_EAX];
            if (op == 6) {
                uint64_t q = n / v, r = n % v;
                if (q > 0xFFFFFFFFull) goto ovf;
                c->r[R_EAX] = (uint32_t)q;
                c->r[R_EDX] = (uint32_t)r;
            } else {
                int64_t sn = (int64_t)n, dv = (int32_t)v;
                if (sn == INT64_MIN && dv == -1) goto ovf;
                int64_t q = sn / dv, r = sn % dv;
                if (q > INT32_MAX || q < INT32_MIN) goto ovf;
                c->r[R_EAX] = (uint32_t)q;
                c->r[R_EDX] = (uint32_t)r;
            }
        }
        return;
    ovf:
        c->stop = CPU_DIVIDE;
        c->eip = d->start;
        return;
    }
    }
}

/* ---- bt / bts / btr / btc ---- */

static void bit_op(Cpu *c, int op, const Ea *e, int sz, int32_t bit, bool reg_offset)
{
    uint32_t addr = e->addr;
    if (!e->reg && reg_offset) {
        addr += (uint32_t)((bit >> (sz == 2 ? 4 : 5)) * (sz == 2 ? 2 : 4));
    }
    unsigned b = (unsigned)bit & (sz == 2 ? 15u : 31u);
    Ea t = *e;
    t.addr = addr;
    uint32_t v = ea_get(c, &t, sz);
    set_flag(c, F_CF, (v >> b) & 1);
    switch (op) {
    case 1: v |= 1u << b; break;
    case 2: v &= ~(1u << b); break;
    case 3: v ^= 1u << b; break;
    default: return;
    }
    ea_set(c, &t, sz, v);
}

/* ---- coma flotante, MMX y SSE ---- */

#include "cpu_fp.inc"

/* ---- instrucciones de dos bytes (0F) ---- */

static void two_byte(Cpu *c, const Dec *d, int osz)
{
    uint8_t op = fetch8(c);
    char b[48];
    if (op >= 0x80 && op <= 0x8F) { /* jcc rel32 */
        int32_t rel = (int32_t)fetch32(c);
        if (cond(c, op & 15)) {
            c->eip += (uint32_t)rel;
        }
        return;
    }
    if (op >= 0x90 && op <= 0x9F) { /* setcc */
        Ea e = modrm_ea(c, d, fetch8(c));
        ea_set(c, &e, 1, cond(c, op & 15) ? 1 : 0);
        return;
    }
    if (op >= 0x40 && op <= 0x4F) { /* cmovcc */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        uint32_t v = ea_get(c, &e, osz);
        if (cond(c, op & 15)) {
            reg_set(c, (m >> 3) & 7, osz, v);
        }
        return;
    }
    if (op >= 0xC8 && op <= 0xCF) { /* bswap */
        c->r[op - 0xC8] = __builtin_bswap32(c->r[op - 0xC8]);
        return;
    }
    switch (op) {
    case 0x0B:
        unimpl(c, d, "ud2");
        return;
    case 0x09: /* wbinvd */
    case 0x08:
        return;
    case 0x18: /* prefetch */
    case 0x1F: /* nop Ev */
        modrm_ea(c, d, fetch8(c));
        return;
    case 0x31: { /* rdtsc: el Pentium III a 733 MHz sobre el tiempo virtual */
        uint64_t per = c->ns_per_instr ? c->ns_per_instr : 1;
        uint64_t ns = c->clock_ns ? *c->clock_ns + (c->icount - c->clock_icount) * per : c->icount * per;
        uint64_t t = ns * 733 / 1000;
        c->r[R_EAX] = (uint32_t)t;
        c->r[R_EDX] = (uint32_t)(t >> 32);
        return;
    }
    case 0xA2: { /* cpuid: Pentium III (Coppermine) del Xbox */
        uint32_t leaf = c->r[R_EAX];
        if (leaf == 0) {
            c->r[R_EAX] = 2;
            c->r[R_EBX] = 0x756E6547; /* "Genu" */
            c->r[R_EDX] = 0x49656E69; /* "ineI" */
            c->r[R_ECX] = 0x6C65746E; /* "ntel" */
        } else if (leaf == 1) {
            c->r[R_EAX] = 0x0000068A;
            c->r[R_EBX] = 0;
            c->r[R_ECX] = 0;
            c->r[R_EDX] = 0x0383F9FF; /* FPU TSC CX8 CMOV MMX FXSR SSE ... */
        } else {
            c->r[R_EAX] = c->r[R_EBX] = c->r[R_ECX] = c->r[R_EDX] = 0;
        }
        return;
    }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: { /* bt bts btr btc r */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        int32_t bit = osz == 2 ? (int16_t)reg_get(c, (m >> 3) & 7, 2) : (int32_t)c->r[(m >> 3) & 7];
        int kind = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
        bit_op(c, kind, &e, osz, bit, true);
        return;
    }
    case 0xBA: { /* grupo 8: bt* imm */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        int sub = (m >> 3) & 7;
        uint8_t imm = fetch8(c);
        if (sub < 4) {
            unimpl(c, d, "0F BA /0-3");
            return;
        }
        bit_op(c, sub - 4, &e, osz, imm, false);
        return;
    }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: { /* shld / shrd */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        unsigned cnt = (op == 0xA4 || op == 0xAC) ? fetch8(c) : (c->r[R_ECX] & 0xFF);
        cnt &= 31;
        if (!cnt) {
            return;
        }
        unsigned bits = (unsigned)osz * 8;
        uint32_t dst = ea_get(c, &e, osz), src = reg_get(c, (m >> 3) & 7, osz), res;
        uint32_t msk = mask_of(osz), s = sign_of(osz);
        bool cf;
        if (cnt > bits) {
            unimpl(c, d, "shld/shrd 16 bits > 16");
            return;
        }
        if (op == 0xA4 || op == 0xA5) {
            uint64_t w = ((uint64_t)dst << bits) | src;
            res = (uint32_t)((w << cnt) >> bits) & msk;
            cf = (dst >> (bits - cnt)) & 1;
        } else {
            uint64_t w = ((uint64_t)src << bits) | dst;
            res = (uint32_t)(w >> cnt) & msk;
            cf = (dst >> (cnt - 1)) & 1;
        }
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, ((res ^ dst) & s) != 0);
        FL(c) &= ~F_AF;
        szp(c, res, osz);
        ea_set(c, &e, osz, res);
        return;
    }
    case 0xAF: { /* imul Gv, Ev */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        int r = (m >> 3) & 7;
        int64_t full = (int64_t)sext(reg_get(c, r, osz), osz) * (int64_t)sext(ea_get(c, &e, osz), osz);
        uint32_t res = (uint32_t)full & mask_of(osz);
        bool over = full != (int64_t)sext(res, osz);
        reg_set(c, r, osz, res);
        set_flag(c, F_CF, over);
        set_flag(c, F_OF, over);
        szp(c, res, osz);
        return;
    }
    case 0xB0: case 0xB1: { /* cmpxchg */
        int sz = op == 0xB0 ? 1 : osz;
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        uint32_t v = ea_get(c, &e, sz);
        alu(c, OP_CMP, reg_get(c, R_EAX, sz), v, sz);
        if (FL(c) & F_ZF) {
            ea_set(c, &e, sz, reg_get(c, (m >> 3) & 7, sz));
        } else {
            reg_set(c, R_EAX, sz, v);
        }
        return;
    }
    case 0xC0: case 0xC1: { /* xadd */
        int sz = op == 0xC0 ? 1 : osz;
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        int r = (m >> 3) & 7;
        uint32_t a = ea_get(c, &e, sz), b2 = reg_get(c, r, sz);
        uint32_t sum = alu(c, OP_ADD, a, b2, sz);
        reg_set(c, r, sz, a);
        ea_set(c, &e, sz, sum);
        return;
    }
    case 0xC7: { /* cmpxchg8b */
        uint8_t m = fetch8(c);
        if (((m >> 3) & 7) != 1 || (m >> 6) == 3) {
            unimpl(c, d, "0F C7");
            return;
        }
        Ea e = modrm_ea(c, d, m);
        uint32_t lo = mrd(c, e.addr, 4), hi = mrd(c, e.addr + 4, 4);
        if (lo == c->r[R_EAX] && hi == c->r[R_EDX]) {
            mwr(c, e.addr, 4, c->r[R_EBX]);
            mwr(c, e.addr + 4, 4, c->r[R_ECX]);
            FL(c) |= F_ZF;
        } else {
            c->r[R_EAX] = lo;
            c->r[R_EDX] = hi;
            FL(c) &= ~F_ZF;
        }
        return;
    }
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: { /* movzx / movsx */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        int ssz = (op == 0xB6 || op == 0xBE) ? 1 : 2;
        uint32_t v = ea_get(c, &e, ssz);
        if (op >= 0xBE) {
            v = (uint32_t)sext(v, ssz);
        }
        reg_set(c, (m >> 3) & 7, osz, v);
        return;
    }
    case 0xBC: case 0xBD: { /* bsf / bsr */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, d, m);
        uint32_t v = ea_get(c, &e, osz);
        if (!v) {
            FL(c) |= F_ZF;
            return;
        }
        FL(c) &= ~F_ZF;
        unsigned idx = op == 0xBC ? (unsigned)__builtin_ctz(v) : 31u - (unsigned)__builtin_clz(v);
        reg_set(c, (m >> 3) & 7, osz, idx);
        return;
    }
    case 0xAE: { /* fxsave / fxrstor / ldmxcsr / stmxcsr */
        uint8_t m = fetch8(c);
        int sub = (m >> 3) & 7;
        if ((m >> 6) == 3) {
            if (sub >= 5) { /* lfence mfence sfence */
                return;
            }
            unimpl(c, d, "0F AE reg");
            return;
        }
        Ea e = modrm_ea(c, d, m);
        if (sub == 0) {
            fxsave(c, e.addr);
        } else if (sub == 1) {
            fxrstor(c, e.addr);
        } else if (sub == 2) {
            c->mxcsr = mrd(c, e.addr, 4);
        } else if (sub == 3) {
            mwr(c, e.addr, 4, c->mxcsr);
        } else {
            unimpl(c, d, "0F AE");
        }
        return;
    }
    default:
        if (sse_mmx(c, d, op)) {
            return;
        }
        snprintf(b, sizeof(b), "%s0F %02X", d->rep == 0xF3 ? "F3 " : d->rep == 0xF2 ? "F2 " : d->opsize ? "66 " : "", op);
        unimpl(c, d, b);
        return;
    }
}

/* ---- una instrucción ---- */

/* los bytes de prefijo */
static const bool k_prefix[256] = {[0x66] = 1, [0x67] = 1, [0x64] = 1, [0xF2] = 1, [0xF3] = 1, [0xF0] = 1,
                                   [0x26] = 1, [0x2E] = 1, [0x36] = 1, [0x3E] = 1, [0x65] = 1};

static CpuStop step(Cpu *c)
{
    if (c->stop != CPU_RUNNING) {
        return c->stop;
    }
    Dec d = {0};
    d.start = c->eip;
    c->trace[c->trace_pos++ % CPU_TRACE] = c->eip;
    uint8_t op;
    for (int n = 0;; n++) {
        op = fetch8(c);
        if (c->stop != CPU_RUNNING || n > 14) {
            goto done;
        }
        if (!k_prefix[op]) {
            break;
        }
        if (op == 0x66) {
            d.opsize = true;
        } else if (op == 0x67) {
            d.adsize = true;
        } else if (op == 0x64) {
            d.fs = true;
        } else if (op == 0xF2 || op == 0xF3) {
            d.rep = op;
        } else if (op == 0xF0 || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x65) {
            /* lock y segmentos planos: sin efecto */
        } else {
            break;
        }
    }
    if (d.adsize) {
        unimpl(&*c, &d, "direcciones de 16 bits (0x67)");
        goto done;
    }
    int osz = d.opsize ? 2 : 4;
    char b[48];

    if (op < 0x40 && (op & 7) < 6) { /* ALU clásicas 00-3D */
        int aop = op >> 3, form = op & 7;
        int sz = (form & 1) ? osz : 1;
        if (form < 4) {
            uint8_t m = fetch8(c);
            Ea e = modrm_ea(c, &d, m);
            int r = (m >> 3) & 7;
            if (form < 2) { /* Eb,Gb / Ev,Gv */
                uint32_t res = alu(c, aop, ea_get(c, &e, sz), reg_get(c, r, sz), sz);
                if (aop != OP_CMP) {
                    ea_set(c, &e, sz, res);
                }
            } else { /* Gb,Eb / Gv,Ev */
                uint32_t res = alu(c, aop, reg_get(c, r, sz), ea_get(c, &e, sz), sz);
                if (aop != OP_CMP) {
                    reg_set(c, r, sz, res);
                }
            }
        } else { /* AL,Ib / eAX,Iz */
            uint32_t imm = sz == 1 ? fetch8(c) : sz == 2 ? fetch16(c) : fetch32(c);
            uint32_t res = alu(c, aop, reg_get(c, R_EAX, sz), imm, sz);
            if (aop != OP_CMP) {
                reg_set(c, R_EAX, sz, res);
            }
        }
        goto done;
    }
    if (op >= 0x40 && op <= 0x4F) { /* inc / dec r */
        int r = op & 7;
        reg_set(c, r, osz, incdec(c, op >= 0x48, reg_get(c, r, osz), osz));
        goto done;
    }
    if (op >= 0x50 && op <= 0x57) {
        push(c, osz, reg_get(c, op & 7, osz));
        goto done;
    }
    if (op >= 0x58 && op <= 0x5F) {
        uint32_t v = pop(c, osz);
        reg_set(c, op & 7, osz, v);
        goto done;
    }
    if (op >= 0x70 && op <= 0x7F) {
        int8_t rel = (int8_t)fetch8(c);
        if (cond(c, op & 15)) {
            c->eip += (uint32_t)(int32_t)rel;
        }
        goto done;
    }
    if (op >= 0x91 && op <= 0x97) {
        uint32_t t = reg_get(c, op & 7, osz);
        reg_set(c, op & 7, osz, reg_get(c, R_EAX, osz));
        reg_set(c, R_EAX, osz, t);
        goto done;
    }
    if (op >= 0xB0 && op <= 0xB7) {
        reg_set(c, op & 7, 1, fetch8(c));
        goto done;
    }
    if (op >= 0xB8 && op <= 0xBF) {
        reg_set(c, op & 7, osz, osz == 2 ? fetch16(c) : fetch32(c));
        goto done;
    }
    if (op >= 0xD8 && op <= 0xDF) {
        fpu(c, &d, op);
        goto done;
    }

    switch (op) {
    case 0x0F:
        two_byte(c, &d, osz);
        break;
    case 0x60: { /* pushad */
        uint32_t esp = c->r[R_ESP];
        for (int r = 0; r < 8; r++) {
            push(c, osz, r == R_ESP ? esp : c->r[r]);
        }
        break;
    }
    case 0x61: /* popad */
        for (int r = 7; r >= 0; r--) {
            uint32_t v = pop(c, osz);
            if (r != R_ESP) {
                reg_set(c, r, osz, v);
            }
        }
        break;
    case 0x68:
        push(c, osz, osz == 2 ? fetch16(c) : fetch32(c));
        break;
    case 0x6A:
        push(c, osz, (uint32_t)(int32_t)(int8_t)fetch8(c));
        break;
    case 0x69:
    case 0x6B: { /* imul Gv, Ev, I */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, &d, m);
        int32_t imm = op == 0x6B ? (int8_t)fetch8(c) : (osz == 2 ? (int16_t)fetch16(c) : (int32_t)fetch32(c));
        int64_t full = (int64_t)sext(ea_get(c, &e, osz), osz) * (int64_t)imm;
        uint32_t res = (uint32_t)full & mask_of(osz);
        bool over = full != (int64_t)sext(res, osz);
        reg_set(c, (m >> 3) & 7, osz, res);
        set_flag(c, F_CF, over);
        set_flag(c, F_OF, over);
        szp(c, res, osz);
        break;
    }
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83: { /* grupo 1 */
        int sz = (op == 0x81 || op == 0x83) ? osz : 1;
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, &d, m);
        uint32_t imm = op == 0x81 ? (osz == 2 ? fetch16(c) : fetch32(c))
                                  : (op == 0x83 ? (uint32_t)(int32_t)(int8_t)fetch8(c) : fetch8(c));
        int aop = (m >> 3) & 7;
        uint32_t res = alu(c, aop, ea_get(c, &e, sz), imm, sz);
        if (aop != OP_CMP) {
            ea_set(c, &e, sz, res);
        }
        break;
    }
    case 0x84:
    case 0x85: {
        int sz = op == 0x84 ? 1 : osz;
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, &d, m);
        alu(c, OP_AND, ea_get(c, &e, sz), reg_get(c, (m >> 3) & 7, sz), sz);
        break;
    }
    case 0x86:
    case 0x87: {
        int sz = op == 0x86 ? 1 : osz;
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, &d, m);
        int r = (m >> 3) & 7;
        uint32_t a = ea_get(c, &e, sz), rv = reg_get(c, r, sz);
        ea_set(c, &e, sz, rv);
        reg_set(c, r, sz, a);
        break;
    }
    case 0x88:
    case 0x89: {
        int sz = op == 0x88 ? 1 : osz;
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, &d, m);
        ea_set(c, &e, sz, reg_get(c, (m >> 3) & 7, sz));
        break;
    }
    case 0x8A:
    case 0x8B: {
        int sz = op == 0x8A ? 1 : osz;
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, &d, m);
        reg_set(c, (m >> 3) & 7, sz, ea_get(c, &e, sz));
        break;
    }
    case 0x8C: { /* mov Ev, Sreg: selectores planos */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, &d, m);
        static const uint16_t sel[6] = {0x10, 0x08, 0x10, 0x10, 0x28, 0x10};
        int s = (m >> 3) & 7;
        ea_set(c, &e, e.reg ? osz : 2, s < 6 ? sel[s] : 0);
        break;
    }
    case 0x8D: { /* lea (sin fs) */
        uint8_t m = fetch8(c);
        Dec nd = d;
        nd.fs = false;
        Ea e = modrm_ea(c, &nd, m);
        if (e.reg) {
            unimpl(c, &d, "lea con registro");
            break;
        }
        reg_set(c, (m >> 3) & 7, osz, e.addr);
        break;
    }
    case 0x8E: { /* mov Sreg, Ew: se ignora (modo plano) */
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, &d, m);
        (void)ea_get(c, &e, 2);
        break;
    }
    case 0x8F: {
        uint8_t m = fetch8(c);
        uint32_t v = pop(c, osz);
        Ea e = modrm_ea(c, &d, m);
        ea_set(c, &e, osz, v);
        break;
    }
    case 0x90:
        break;
    case 0x98: /* cwde / cbw */
        if (osz == 4) {
            c->r[R_EAX] = (uint32_t)(int32_t)(int16_t)c->r[R_EAX];
        } else {
            reg_set(c, R_EAX, 2, (uint32_t)(int16_t)(int8_t)c->r[R_EAX]);
        }
        break;
    case 0x99: /* cdq / cwd */
        if (osz == 4) {
            c->r[R_EDX] = (c->r[R_EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
        } else {
            reg_set(c, R_EDX, 2, (c->r[R_EAX] & 0x8000) ? 0xFFFF : 0);
        }
        break;
    case 0x9B: /* wait */
        break;
    case 0x9C:
        push(c, osz, (FL(c) & 0x00244FD5u) | 0x2u);
        break;
    case 0x9D: {
        uint32_t v = pop(c, osz);
        uint32_t keep = osz == 2 ? 0xFFFF0000u : 0;
        FL(c) = (FL(c) & keep) | (v & 0x00244FD5u & ~keep) | 0x2u;
        break;
    }
    case 0x9E: /* sahf */
        FL(c) = (FL(c) & ~0xD5u) | ((c->r[R_EAX] >> 8) & 0xD5u);
        break;
    case 0x9F: /* lahf */
        reg_set(c, 4, 1, (FL(c) & 0xD5u) | 0x2u);
        break;
    case 0xD7: { /* xlat: AL = [EBX + AL] */
        uint32_t v = mrd(c, c->r[R_EBX] + (c->r[R_EAX] & 0xFF) + (d.fs ? c->fs_base : 0), 1);
        if (c->stop == CPU_RUNNING) {
            reg_set(c, R_EAX, 1, v);
        }
        break;
    }
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: { /* mov moffs */
        int sz = (op & 1) ? osz : 1;
        uint32_t a = fetch32(c) + (d.fs ? c->fs_base : 0);
        if (op < 0xA2) {
            reg_set(c, R_EAX, sz, mrd(c, a, sz));
        } else {
            mwr(c, a, sz, reg_get(c, R_EAX, sz));
        }
        break;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, &d, op, (op & 1) ? osz : 1);
        break;
    case 0xA8:
        alu(c, OP_AND, c->r[R_EAX], fetch8(c), 1);
        break;
    case 0xA9:
        alu(c, OP_AND, c->r[R_EAX], osz == 2 ? fetch16(c) : fetch32(c), osz);
        break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: { /* grupo 2 */
        int sz = (op & 1) ? osz : 1;
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, &d, m);
        unsigned cnt = (op <= 0xC1) ? fetch8(c) : (op <= 0xD1 ? 1u : (c->r[R_ECX] & 0xFF));
        uint32_t v = ea_get(c, &e, sz);
        if ((cnt & 31) == 0) {
            break;
        }
        ea_set(c, &e, sz, shift(c, (m >> 3) & 7, v, cnt, sz));
        break;
    }
    case 0xC2: {
        uint16_t n = fetch16(c);
        c->eip = pop(c, 4);
        c->r[R_ESP] += n;
        break;
    }
    case 0xC3:
        c->eip = pop(c, 4);
        break;
    case 0xC6:
    case 0xC7: {
        int sz = op == 0xC6 ? 1 : osz;
        uint8_t m = fetch8(c);
        Ea e = modrm_ea(c, &d, m);
        uint32_t imm = sz == 1 ? fetch8(c) : sz == 2 ? fetch16(c) : fetch32(c);
        ea_set(c, &e, sz, imm);
        break;
    }
    case 0xC8: { /* enter, nivel 0 */
        uint16_t size = fetch16(c);
        uint8_t level = fetch8(c);
        if (level) {
            unimpl(c, &d, "enter con nivel");
            break;
        }
        push(c, 4, c->r[R_EBP]);
        c->r[R_EBP] = c->r[R_ESP];
        c->r[R_ESP] -= size;
        break;
    }
    case 0xC9: /* leave */
        c->r[R_ESP] = c->r[R_EBP];
        c->r[R_EBP] = pop(c, 4);
        break;
    case 0xCC:
        c->stop = CPU_BREAK;
        c->eip = d.start;
        break;
    case 0xCD: {
        uint8_t v = fetch8(c);
        if (v == 0x3F) { /* llamada al kernel: int 0x3F, ordinal de 16 bits */
            c->kcall_ordinal = fetch16(c);
            c->stop = CPU_KCALL;
            break;
        }
        snprintf(b, sizeof(b), "int 0x%02X", v);
        unimpl(c, &d, b);
        break;
    }
    case 0xE0: case 0xE1: case 0xE2: { /* loopne loope loop */
        int8_t rel = (int8_t)fetch8(c);
        c->r[R_ECX]--;
        bool go = c->r[R_ECX] != 0;
        if (op == 0xE0) {
            go = go && !(FL(c) & F_ZF);
        } else if (op == 0xE1) {
            go = go && (FL(c) & F_ZF);
        }
        if (go) {
            c->eip += (uint32_t)(int32_t)rel;
        }
        break;
    }
    case 0xE3: {
        int8_t rel = (int8_t)fetch8(c);
        if (c->r[R_ECX] == 0) {
            c->eip += (uint32_t)(int32_t)rel;
        }
        break;
    }
    case 0xE8: {
        int32_t rel = (int32_t)fetch32(c);
        push(c, 4, c->eip);
        c->eip += (uint32_t)rel;
        break;
    }
    case 0xE9: {
        int32_t rel = (int32_t)fetch32(c);
        c->eip += (uint32_t)rel;
        break;
    }
    case 0xEB: {
        int8_t rel = (int8_t)fetch8(c);
        c->eip += (uint32_t)(int32_t)rel;
        break;
    }
    case 0xF4:
        c->stop = CPU_HLT;
        c->eip = d.start;
        break;
    case 0xF5:
        FL(c) ^= F_CF;
        break;
    case 0xF6:
    case 0xF7:
        group3(c, &d, op == 0xF6 ? 1 : osz, fetch8(c));
        break;
    case 0xF8: FL(c) &= ~F_CF; break;
    case 0xF9: FL(c) |= F_CF; break;
    case 0xE4: case 0xE5: case 0xEC: case 0xED: { /* in */
        int sz = (op & 1) ? osz : 1;
        uint16_t port = op < 0xEC ? fetch8(c) : (uint16_t)c->r[R_EDX];
        uint32_t ones = sz == 4 ? 0xFFFFFFFFu : (1u << (8 * sz)) - 1;
        reg_set(c, R_EAX, sz, c->port_in ? c->port_in(c->dev, port, sz) : ones);
        break;
    }
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: { /* out */
        int sz = (op & 1) ? osz : 1;
        uint16_t port = op < 0xEE ? fetch8(c) : (uint16_t)c->r[R_EDX];
        if (c->port_out) {
            c->port_out(c->dev, port, sz, reg_get(c, R_EAX, sz));
        }
        break;
    }
    case 0xFA: FL(c) &= ~F_IF; break;
    case 0xFB: FL(c) |= F_IF; break;
    case 0xFC: FL(c) &= ~F_DF; break;
    case 0xFD: FL(c) |= F_DF; break;
    case 0xFE:
    case 0xFF: {
        int sz = op == 0xFE ? 1 : osz;
        uint8_t m = fetch8(c);
        int sub = (m >> 3) & 7;
        Ea e = modrm_ea(c, &d, m);
        if (sub <= 1) {
            ea_set(c, &e, sz, incdec(c, sub != 0, ea_get(c, &e, sz), sz));
        } else if (op == 0xFF && sub == 2) {
            uint32_t t = ea_get(c, &e, 4);
            push(c, 4, c->eip);
            c->eip = t;
        } else if (op == 0xFF && sub == 4) {
            c->eip = ea_get(c, &e, 4);
        } else if (op == 0xFF && sub == 6) {
            push(c, osz, ea_get(c, &e, osz));
        } else {
            snprintf(b, sizeof(b), "%02X /%d", op, sub);
            unimpl(c, &d, b);
        }
        break;
    }
    default:
        snprintf(b, sizeof(b), "opcode %02X", op);
        unimpl(c, &d, b);
        break;
    }

done:
    if (c->stop == CPU_RUNNING) {
        c->icount++;
    } else {
        c->stop_eip = d.start;
        vm_read(c->vm, d.start, c->stop_bytes, sizeof(c->stop_bytes));
        if (c->stop == CPU_FAULT || c->stop == CPU_UNIMPL || c->stop == CPU_DIVIDE) {
            c->eip = d.start; /* se puede repetir cuando se resuelva */
        }
    }
    return c->stop;
}

/* ================================================== bloques decodificados
 *
 * Etapa 2d.1: cada trozo de código se decodifica una sola vez en un bloque
 * de instrucciones ya entendidas (operandos, modrm, inmediatos) y luego se
 * ejecuta llamando a la función de cada una, sin volver a leer los bytes.
 * Solo las instrucciones más comunes tienen función propia; la primera que
 * no la tiene termina el bloque y la hace el intérprete (step), así nada se
 * pierde. Un bloque vive en una página física: si alguien escribe en ella
 * (vm_touch), su gen sube y el bloque se vuelve a decodificar. */

#define NOREG 8
#define BLK_MAX 64
#define BLK_HASH (1u << 19) /* Halo 2 tiene cientos de miles de bloques: con 65536 se echaban unos a otros y se volvían a traducir */

typedef struct Insn Insn;
typedef void (*InsnFn)(Cpu *c, const Insn *in);
struct Insn {
    InsnFn fn;
    uint32_t start, next;   /* su EIP y el de la siguiente */
    uint32_t imm;           /* inmediato (o el destino de un salto) */
    uint32_t disp;          /* desplazamiento de la dirección */
    uint8_t sz;             /* tamaño del operando: 1, 2, 4 */
    uint8_t reg;            /* el campo reg del modrm (o el registro del opcode) */
    uint8_t aop;            /* operación ALU, condición, tamaño fuente... */
    uint8_t is_reg, rm;     /* el operando E es un registro (rm) */
    uint8_t base, index, scale, fs;
};

typedef struct Block {
    uint32_t eip, ppage, gen;
    unsigned n;
    void *jit;            /* su traducción a ARM64 (2d.2), si la hay */
    void *jit_chain;      /* su entrada desde otro bloque traducido (encadenar) */
    uint32_t jcnt;        /* su contador de veces en el JIT (g_jcnt) */
    bool spin;            /* un bucle sobre sí mismo que solo lee y compara */
    uint32_t jit_epoch;   /* del búfer del JIT en que se tradujo */
    uint32_t hits;        /* veces que corrió sin traducción */
    uint32_t execs;       /* veces que corrió (para el perfil) */
    uint64_t slow;        /* JIT: instrucciones que llaman a su función de C (bit i) */
    Insn in[];
} Block;

typedef struct BlockSlot {
    uint32_t eip;
    Block *b;
} BlockSlot;

static inline uint32_t b_ea(Cpu *c, const Insn *in)
{
    uint32_t a = in->disp;
    if (in->base != NOREG) {
        a += c->r[in->base];
    }
    if (in->index != NOREG) {
        a += c->r[in->index] << in->scale;
    }
    if (in->fs) {
        a += c->fs_base;
    }
    return a;
}

static inline uint32_t e_get(Cpu *c, const Insn *in, int sz)
{
    return in->is_reg ? reg_get(c, in->rm, sz) : mrd(c, b_ea(c, in), sz);
}

static inline void e_set(Cpu *c, const Insn *in, int sz, uint32_t v)
{
    if (in->is_reg) {
        reg_set(c, in->rm, sz, v);
    } else {
        mwr(c, b_ea(c, in), sz, v);
    }
}

/* ---- las funciones de cada instrucción (c->eip ya es in->next) ---- */

static void x_alu_eg(Cpu *c, const Insn *in)
{
    if (in->is_reg) {
        uint32_t r = alu(c, in->aop, reg_get(c, in->rm, in->sz), reg_get(c, in->reg, in->sz), in->sz);
        if (in->aop != OP_CMP) {
            reg_set(c, in->rm, in->sz, r);
        }
        return;
    }
    uint32_t a = b_ea(c, in);
    uint32_t v = mrd(c, a, in->sz);
    uint32_t r = alu(c, in->aop, v, reg_get(c, in->reg, in->sz), in->sz);
    if (in->aop != OP_CMP && c->stop == CPU_RUNNING) {
        mwr(c, a, in->sz, r);
    }
}

static void x_alu_ge(Cpu *c, const Insn *in)
{
    uint32_t r = alu(c, in->aop, reg_get(c, in->reg, in->sz), e_get(c, in, in->sz), in->sz);
    if (in->aop != OP_CMP) {
        reg_set(c, in->reg, in->sz, r);
    }
}

static void x_alu_ei(Cpu *c, const Insn *in)
{
    if (in->is_reg) {
        uint32_t r = alu(c, in->aop, reg_get(c, in->rm, in->sz), in->imm, in->sz);
        if (in->aop != OP_CMP) {
            reg_set(c, in->rm, in->sz, r);
        }
        return;
    }
    uint32_t a = b_ea(c, in);
    uint32_t v = mrd(c, a, in->sz);
    uint32_t r = alu(c, in->aop, v, in->imm, in->sz);
    if (in->aop != OP_CMP && c->stop == CPU_RUNNING) {
        mwr(c, a, in->sz, r);
    }
}

static void x_test_eg(Cpu *c, const Insn *in)
{
    alu(c, OP_AND, e_get(c, in, in->sz), reg_get(c, in->reg, in->sz), in->sz);
}

static void x_test_ei(Cpu *c, const Insn *in) { alu(c, OP_AND, e_get(c, in, in->sz), in->imm, in->sz); }

static void x_incdec(Cpu *c, const Insn *in)
{
    if (in->is_reg) {
        reg_set(c, in->rm, in->sz, incdec(c, in->aop, reg_get(c, in->rm, in->sz), in->sz));
        return;
    }
    uint32_t a = b_ea(c, in);
    uint32_t v = mrd(c, a, in->sz);
    if (c->stop == CPU_RUNNING) {
        mwr(c, a, in->sz, incdec(c, in->aop, v, in->sz));
    }
}

static void x_push_r(Cpu *c, const Insn *in) { push(c, in->sz, reg_get(c, in->reg, in->sz)); }

static void x_pop_r(Cpu *c, const Insn *in)
{
    uint32_t v = pop(c, in->sz);
    if (c->stop == CPU_RUNNING) {
        reg_set(c, in->reg, in->sz, v);
    }
}

static void x_push_i(Cpu *c, const Insn *in) { push(c, in->sz, in->imm); }

static void x_push_e(Cpu *c, const Insn *in)
{
    uint32_t v = e_get(c, in, in->sz);
    if (c->stop == CPU_RUNNING) {
        push(c, in->sz, v);
    }
}

static void x_mov_eg(Cpu *c, const Insn *in) { e_set(c, in, in->sz, reg_get(c, in->reg, in->sz)); }

static void x_mov_ge(Cpu *c, const Insn *in)
{
    uint32_t v = e_get(c, in, in->sz);
    if (c->stop == CPU_RUNNING) {
        reg_set(c, in->reg, in->sz, v);
    }
}

static void x_mov_ei(Cpu *c, const Insn *in) { e_set(c, in, in->sz, in->imm); }

static void x_lea(Cpu *c, const Insn *in) { reg_set(c, in->reg, in->sz, b_ea(c, in)); }

static void x_movx(Cpu *c, const Insn *in)
{
    /* aop: tamaño de la fuente; imm: 1 con signo */
    uint32_t v = e_get(c, in, in->aop);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    if (in->imm) {
        v = (uint32_t)sext(v, in->aop);
    }
    reg_set(c, in->reg, in->sz, v);
}

static void x_jcc(Cpu *c, const Insn *in)
{
    if (cond(c, in->aop)) {
        c->eip = in->imm;
    }
}

static void x_jmp(Cpu *c, const Insn *in) { c->eip = in->imm; }

static void x_call(Cpu *c, const Insn *in)
{
    push(c, 4, in->next);
    c->eip = in->imm;
}

static void x_ret(Cpu *c, const Insn *in)
{
    uint32_t t = pop(c, 4);
    if (c->stop == CPU_RUNNING) {
        c->eip = t;
        c->r[R_ESP] += in->imm;
    }
}

static void x_call_e(Cpu *c, const Insn *in)
{
    uint32_t t = e_get(c, in, 4);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    push(c, 4, in->next);
    c->eip = t;
}

static void x_jmp_e(Cpu *c, const Insn *in)
{
    uint32_t t = e_get(c, in, 4);
    if (c->stop == CPU_RUNNING) {
        c->eip = t;
    }
}

static void x_setcc(Cpu *c, const Insn *in) { e_set(c, in, 1, cond(c, in->aop) ? 1 : 0); }

static void x_cmov(Cpu *c, const Insn *in)
{
    uint32_t v = e_get(c, in, in->sz); /* lee siempre (como el hardware) */
    if (c->stop == CPU_RUNNING && cond(c, in->aop)) {
        reg_set(c, in->reg, in->sz, v);
    }
}

static void x_shift(Cpu *c, const Insn *in)
{
    /* imm: la cuenta (0xFF: CL) */
    unsigned cnt = in->imm == 0xFF ? (c->r[R_ECX] & 0xFF) : in->imm;
    uint32_t a = in->is_reg ? 0 : b_ea(c, in);
    uint32_t v = in->is_reg ? reg_get(c, in->rm, in->sz) : mrd(c, a, in->sz);
    if (c->stop != CPU_RUNNING || (cnt & 31) == 0) {
        return;
    }
    uint32_t r = shift(c, in->aop, v, cnt, in->sz);
    if (in->is_reg) {
        reg_set(c, in->rm, in->sz, r);
    } else {
        mwr(c, a, in->sz, r);
    }
}

static void x_imul(Cpu *c, const Insn *in)
{
    /* aop 1: Gv = Ev * imm; 0: Gv = Gv * Ev */
    uint32_t e = e_get(c, in, in->sz);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    int64_t other = in->aop ? (int64_t)(int32_t)in->imm : (int64_t)sext(reg_get(c, in->reg, in->sz), in->sz);
    int64_t full = (int64_t)sext(e, in->sz) * other;
    uint32_t res = (uint32_t)full & mask_of(in->sz);
    bool over = full != (int64_t)sext(res, in->sz);
    reg_set(c, in->reg, in->sz, res);
    set_flag(c, F_CF, over);
    set_flag(c, F_OF, over);
    szp(c, res, in->sz);
}

static void x_cdq(Cpu *c, const Insn *in)
{
    if (in->sz == 4) {
        c->r[R_EDX] = (c->r[R_EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
    } else {
        reg_set(c, R_EDX, 2, (c->r[R_EAX] & 0x8000) ? 0xFFFF : 0);
    }
}

static void x_leave(Cpu *c, const Insn *in)
{
    (void)in;
    uint32_t esp = c->r[R_ESP];
    c->r[R_ESP] = c->r[R_EBP];
    uint32_t v = pop(c, 4);
    if (c->stop == CPU_RUNNING) {
        c->r[R_EBP] = v;
    } else {
        c->r[R_ESP] = esp;
    }
}

static void x_nop(Cpu *c, const Insn *in)
{
    (void)c;
    (void)in;
}

static void x_xchg(Cpu *c, const Insn *in)
{
    if (in->is_reg) {
        uint32_t t = reg_get(c, in->rm, in->sz);
        reg_set(c, in->rm, in->sz, reg_get(c, in->reg, in->sz));
        reg_set(c, in->reg, in->sz, t);
        return;
    }
    uint32_t a = b_ea(c, in);
    uint32_t v = mrd(c, a, in->sz);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    mwr(c, a, in->sz, reg_get(c, in->reg, in->sz));
    if (c->stop == CPU_RUNNING) {
        reg_set(c, in->reg, in->sz, v);
    }
}

/* ---- MMX (etapa 2d.1): el video Bink las usa sin parar ---- */

static inline uint64_t mm_b(Cpu *c, const Insn *in)
{
    return in->is_reg ? c->fp.mm[in->rm] : rd64(c, b_ea(c, in));
}

static inline int sat_s8(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static inline int sat_s16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static inline int sat_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline int sat_u16(int v) { return v < 0 ? 0 : v > 65535 ? 65535 : v; }

#define MMX_FN(name, T, N, EXPR)                                  \
    static void name(Cpu *c, const Insn *in)                      \
    {                                                             \
        uint64_t bv = mm_b(c, in);                                \
        if (c->stop != CPU_RUNNING) {                             \
            return;                                               \
        }                                                         \
        T a[N], b[N];                                             \
        memcpy(a, &c->fp.mm[in->reg], 8);                         \
        memcpy(b, &bv, 8);                                        \
        for (int i = 0; i < N; i++) {                             \
            EXPR;                                                 \
        }                                                         \
        memcpy(&c->fp.mm[in->reg], a, 8);                         \
    }

MMX_FN(x_paddb, uint8_t, 8, a[i] = (uint8_t)(a[i] + b[i]))
MMX_FN(x_paddw, uint16_t, 4, a[i] = (uint16_t)(a[i] + b[i]))
MMX_FN(x_paddd, uint32_t, 2, a[i] += b[i])
MMX_FN(x_psubb, uint8_t, 8, a[i] = (uint8_t)(a[i] - b[i]))
MMX_FN(x_psubw, uint16_t, 4, a[i] = (uint16_t)(a[i] - b[i]))
MMX_FN(x_psubd, uint32_t, 2, a[i] -= b[i])
MMX_FN(x_paddsb, int8_t, 8, a[i] = (int8_t)sat_s8(a[i] + b[i]))
MMX_FN(x_paddsw, int16_t, 4, a[i] = (int16_t)sat_s16(a[i] + b[i]))
MMX_FN(x_psubsb, int8_t, 8, a[i] = (int8_t)sat_s8(a[i] - b[i]))
MMX_FN(x_psubsw, int16_t, 4, a[i] = (int16_t)sat_s16(a[i] - b[i]))
MMX_FN(x_paddusb, uint8_t, 8, a[i] = (uint8_t)sat_u8(a[i] + b[i]))
MMX_FN(x_paddusw, uint16_t, 4, a[i] = (uint16_t)sat_u16(a[i] + b[i]))
MMX_FN(x_psubusb, uint8_t, 8, a[i] = (uint8_t)sat_u8(a[i] - b[i]))
MMX_FN(x_psubusw, uint16_t, 4, a[i] = (uint16_t)sat_u16(a[i] - b[i]))
MMX_FN(x_pmullw, uint16_t, 4, a[i] = (uint16_t)((uint32_t)a[i] * b[i]))
MMX_FN(x_pmulhw, int16_t, 4, a[i] = (int16_t)(((int32_t)a[i] * b[i]) >> 16))
MMX_FN(x_pmulhuw, uint16_t, 4, a[i] = (uint16_t)(((uint32_t)a[i] * b[i]) >> 16))
MMX_FN(x_pcmpeqb, uint8_t, 8, a[i] = a[i] == b[i] ? 0xFF : 0)
MMX_FN(x_pcmpeqw, uint16_t, 4, a[i] = a[i] == b[i] ? 0xFFFF : 0)
MMX_FN(x_pcmpeqd, uint32_t, 2, a[i] = a[i] == b[i] ? 0xFFFFFFFFu : 0)
MMX_FN(x_pcmpgtb, int8_t, 8, a[i] = a[i] > b[i] ? -1 : 0)
MMX_FN(x_pcmpgtw, int16_t, 4, a[i] = a[i] > b[i] ? -1 : 0)
MMX_FN(x_pcmpgtd, int32_t, 2, a[i] = a[i] > b[i] ? -1 : 0)
MMX_FN(x_pminub, uint8_t, 8, a[i] = a[i] < b[i] ? a[i] : b[i])
MMX_FN(x_pmaxub, uint8_t, 8, a[i] = a[i] > b[i] ? a[i] : b[i])
MMX_FN(x_pminsw, int16_t, 4, a[i] = a[i] < b[i] ? a[i] : b[i])
MMX_FN(x_pmaxsw, int16_t, 4, a[i] = a[i] > b[i] ? a[i] : b[i])
MMX_FN(x_pavgb, uint8_t, 8, a[i] = (uint8_t)((a[i] + b[i] + 1) >> 1))
MMX_FN(x_pavgw, uint16_t, 4, a[i] = (uint16_t)((a[i] + b[i] + 1) >> 1))

static void x_mmx_logic(Cpu *c, const Insn *in)
{
    uint64_t b = mm_b(c, in);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    uint64_t *a = &c->fp.mm[in->reg];
    switch (in->aop) {
    case 0xDB: *a &= b; break;
    case 0xDF: *a = ~*a & b; break;
    case 0xEB: *a |= b; break;
    default: *a ^= b; break;
    }
}

static void x_pmaddwd(Cpu *c, const Insn *in)
{
    uint64_t bv = mm_b(c, in);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    int16_t a[4], b[4];
    memcpy(a, &c->fp.mm[in->reg], 8);
    memcpy(b, &bv, 8);
    uint32_t r[2];
    for (int i = 0; i < 2; i++) {
        r[i] = (uint32_t)((int32_t)a[2 * i] * b[2 * i]) + (uint32_t)((int32_t)a[2 * i + 1] * b[2 * i + 1]);
    }
    memcpy(&c->fp.mm[in->reg], r, 8);
}

static void x_punpck(Cpu *c, const Insn *in)
{
    uint64_t b = mm_b(c, in);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    uint64_t *a = &c->fp.mm[in->reg];
    int bits = (in->aop & 3) == 0 ? 8 : (in->aop & 3) == 1 ? 16 : 32;
    *a = unpack(*a, b, bits, in->aop >= 0x68);
}

static void x_pack(Cpu *c, const Insn *in)
{
    uint64_t b = mm_b(c, in);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    uint64_t *a = &c->fp.mm[in->reg];
    *a = pack(*a, b, in->aop == 0x6B ? 32 : 16, in->aop == 0x67);
}

/* desplazamientos: imm (0F 71/72/73, cuenta en in->imm) o por mm/m64 */
static void x_mmx_shift_i(Cpu *c, const Insn *in)
{
    c->fp.mm[in->rm] = mmx_shift(in->reg, in->sz * 8, c->fp.mm[in->rm], in->imm);
}

static void x_mmx_shift_r(Cpu *c, const Insn *in)
{
    uint64_t cnt = mm_b(c, in);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    uint8_t kind = in->aop < 0xE0 ? 2 : in->aop < 0xF0 ? 4 : 6;
    int bits = 16 << ((in->aop & 3) - 1);
    c->fp.mm[in->reg] = mmx_shift(kind, bits, c->fp.mm[in->reg], cnt);
}

static void x_movq_load(Cpu *c, const Insn *in) /* 0F 6F */
{
    uint64_t v = mm_b(c, in);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    c->fp.mm[in->reg] = v;
    c->fp.mmx = true;
}

static void x_movq_store(Cpu *c, const Insn *in) /* 0F 7F, 0F E7 */
{
    uint64_t v = c->fp.mm[in->reg];
    if (in->is_reg) {
        c->fp.mm[in->rm] = v;
    } else {
        wr64(c, b_ea(c, in), v);
    }
}

static void x_movd_load(Cpu *c, const Insn *in) /* 0F 6E */
{
    uint32_t v = e_get(c, in, 4);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    c->fp.mm[in->reg] = v;
    c->fp.mmx = true;
}

static void x_movd_store(Cpu *c, const Insn *in) /* 0F 7E */
{
    e_set(c, in, 4, (uint32_t)c->fp.mm[in->reg]);
}

static void x_emms(Cpu *c, const Insn *in)
{
    (void)in;
    c->fp.empty = 0xFF;
    c->fp.mmx = false;
}

static void x_pshufw(Cpu *c, const Insn *in)
{
    uint64_t bv = mm_b(c, in);
    if (c->stop != CPU_RUNNING) {
        return;
    }
    uint16_t b[4], r[4];
    memcpy(b, &bv, 8);
    for (int i = 0; i < 4; i++) {
        r[i] = b[(in->imm >> (2 * i)) & 3];
    }
    memcpy(&c->fp.mm[in->reg], r, 8);
}

/* Cualquier otra instrucción sin salto propio: la hace el intérprete
 * sin salir del bloque. Si el largo decodificado no era el real, el EIP
 * no coincide con in->next y el bloque se corta ahí (no pasa nada). */
static void x_interp(Cpu *c, const Insn *in)
{
    c->eip = in->start;
    if (step(c) == CPU_RUNNING) {
        c->icount--; /* run_blocks la cuenta */
    }
}

/* ---- decodificar ---- */

typedef struct Rd {
    const uint8_t *p;   /* el principio de la página */
    uint32_t pos;       /* dentro de la página */
    bool bad;           /* se pasó del final de la página */
} Rd;

static inline uint8_t rd8(Rd *r)
{
    if (r->pos >= VM_PAGE) {
        r->bad = true;
        return 0;
    }
    return r->p[r->pos++];
}

static inline uint32_t rdn(Rd *r, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        v |= (uint32_t)rd8(r) << (8 * i);
    }
    return v;
}

static void dec_modrm(Rd *r, Insn *in, uint8_t m)
{
    int mod = m >> 6, rm = m & 7;
    in->reg = (m >> 3) & 7;
    in->base = in->index = NOREG;
    in->scale = 0;
    in->disp = 0;
    if (mod == 3) {
        in->is_reg = 1;
        in->rm = (uint8_t)rm;
        return;
    }
    in->is_reg = 0;
    if (rm == 4) {
        uint8_t sib = rd8(r);
        int scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (base == 5 && mod == 0) {
            in->disp = rdn(r, 4);
        } else {
            in->base = (uint8_t)base;
        }
        if (idx != 4) {
            in->index = (uint8_t)idx;
            in->scale = (uint8_t)scale;
        }
    } else if (rm == 5 && mod == 0) {
        in->disp = rdn(r, 4);
    } else {
        in->base = (uint8_t)rm;
    }
    if (mod == 1) {
        in->disp += (uint32_t)(int32_t)(int8_t)rd8(r);
    } else if (mod == 2) {
        in->disp += rdn(r, 4);
    }
}

static InsnFn mmx_fn(uint8_t op)
{
    switch (op) {
    case 0xFC: return x_paddb;
    case 0xFD: return x_paddw;
    case 0xFE: return x_paddd;
    case 0xF8: return x_psubb;
    case 0xF9: return x_psubw;
    case 0xFA: return x_psubd;
    case 0xEC: return x_paddsb;
    case 0xED: return x_paddsw;
    case 0xE8: return x_psubsb;
    case 0xE9: return x_psubsw;
    case 0xDC: return x_paddusb;
    case 0xDD: return x_paddusw;
    case 0xD8: return x_psubusb;
    case 0xD9: return x_psubusw;
    case 0xD5: return x_pmullw;
    case 0xE5: return x_pmulhw;
    case 0xE4: return x_pmulhuw;
    case 0x74: return x_pcmpeqb;
    case 0x75: return x_pcmpeqw;
    case 0x76: return x_pcmpeqd;
    case 0x64: return x_pcmpgtb;
    case 0x65: return x_pcmpgtw;
    case 0x66: return x_pcmpgtd;
    case 0xDA: return x_pminub;
    case 0xDE: return x_pmaxub;
    case 0xEA: return x_pminsw;
    case 0xEE: return x_pmaxsw;
    case 0xE0: return x_pavgb;
    case 0xE3: return x_pavgw;
    case 0xDB: case 0xDF: case 0xEB: case 0xEF: return x_mmx_logic;
    case 0xF5: return x_pmaddwd;
    case 0x60: case 0x61: case 0x62: case 0x68: case 0x69: case 0x6A: return x_punpck;
    case 0x63: case 0x67: case 0x6B: return x_pack;
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3: return x_mmx_shift_r;
    case 0x6F: return x_movq_load;
    case 0x7F: case 0xE7: return x_movq_store;
    case 0x6E: return x_movd_load;
    case 0x7E: return x_movd_store;
    default: return NULL;
    }
}

/* Las que hace el intérprete quedan marcadas para el JIT (2d.3, x87 y SSE):
 * aop = el opcode (el segundo byte en las 0F), sz = INTERP_MARK | INTERP_0F
 * si es 0F | los prefijos (P66, PF3, PF2), imm = el inmediato de 8 bits. */
#define INTERP_MARK 0x80u
#define INTERP_0F 0x40u
#define INTERP_P66 1u
#define INTERP_PF3 2u
#define INTERP_PF2 4u

/* El largo de una 0F que hace el intérprete (sin saltos). false: no. */
static bool dec_interp_0f(Rd *r, Insn *in, uint8_t op2, unsigned pfx)
{
    bool modrm, imm8 = false;
    if (op2 == 0x77 || op2 == 0x31 || op2 == 0xA2 || (op2 >= 0xC8 && op2 <= 0xCF)) {
        modrm = false;
    } else if ((op2 >= 0x10 && op2 <= 0x1F) || (op2 >= 0x28 && op2 <= 0x2F) || (op2 >= 0x40 && op2 <= 0x7F) ||
               (op2 >= 0x90 && op2 <= 0x9F) || op2 == 0xA3 || op2 == 0xA4 || op2 == 0xA5 ||
               (op2 >= 0xAB && op2 <= 0xB1) || op2 == 0xB3 || (op2 >= 0xB6 && op2 <= 0xB7) ||
               (op2 >= 0xBA && op2 <= 0xC6) || op2 >= 0xD0) {
        modrm = true;
        imm8 = (op2 >= 0x70 && op2 <= 0x73) || op2 == 0xA4 || op2 == 0xAC || op2 == 0xBA ||
               (op2 >= 0xC2 && op2 <= 0xC6 && op2 != 0xC3);
    } else {
        return false;
    }
    if (modrm) {
        dec_modrm(r, in, rd8(r));
    }
    in->imm = imm8 ? rd8(r) : 0;
    in->fn = x_interp;
    in->aop = op2;
    in->sz = (uint8_t)(INTERP_MARK | INTERP_0F | pfx);
    return true;
}

/* El largo de una de un byte que hace el intérprete (sin saltos). */
static bool dec_interp_1(Rd *r, Insn *in, uint8_t op, unsigned pfx)
{
    if ((op >= 0xD8 && op <= 0xDF) || op == 0x8F) {
        dec_modrm(r, in, rd8(r));
    } else if (!((op >= 0xA4 && op <= 0xA7) || (op >= 0xAA && op <= 0xAF) || (op >= 0x91 && op <= 0x98) ||
                 (op >= 0x9B && op <= 0x9F) || op == 0x60 || op == 0x61 || op == 0x90 || op == 0xF5 ||
                 op == 0xF8 || op == 0xF9 || op == 0xFC || op == 0xFD || op == 0xD7)) {
        return false;
    }
    in->fn = x_interp;
    in->aop = op;
    in->sz = (uint8_t)(INTERP_MARK | pfx);
    return true;
}

/* Una instrucción con función propia; false: la hace el intérprete. */
static bool decode(Rd *r, uint32_t page_va, Insn *in)
{
    uint32_t start = r->pos;
    bool o16 = false, fs = false;
    uint8_t op, rep_ = 0;
    for (;;) {
        op = rd8(r);
        if (op == 0x66) {
            o16 = true;
        } else if (op == 0x64) {
            fs = true;
        } else if (op == 0xF2 || op == 0xF3) {
            rep_ = op;
        } else if (op == 0x3E || op == 0x26 || op == 0x2E || op == 0x36 || op == 0xF0) {
            /* segmentos planos */
        } else {
            break;
        }
        if (r->pos - start > 4) {
            return false;
        }
    }
    int osz = o16 ? 2 : 4;
    memset(in, 0, sizeof(*in));
    in->start = page_va + start;
    in->fs = fs;
    in->base = in->index = NOREG;
    unsigned pfx = (o16 ? INTERP_P66 : 0) | (rep_ == 0xF3 ? INTERP_PF3 : 0) | (rep_ == 0xF2 ? INTERP_PF2 : 0);
    if (rep_) { /* rep, movss...: siempre el intérprete */
        if (op == 0x0F) {
            if (!dec_interp_0f(r, in, rd8(r), pfx)) {
                return false;
            }
        } else if (!dec_interp_1(r, in, op, pfx)) {
            return false;
        }
    } else if (op < 0x40 && (op & 7) < 6) { /* ALU 00-3D */
        int form = op & 7;
        in->aop = op >> 3;
        in->sz = (uint8_t)((form & 1) ? osz : 1);
        if (form < 4) {
            dec_modrm(r, in, rd8(r));
            in->fn = form < 2 ? x_alu_eg : x_alu_ge;
        } else {
            in->is_reg = 1;
            in->rm = R_EAX;
            in->imm = rdn(r, in->sz);
            in->fn = x_alu_ei;
        }
    } else if (op >= 0x40 && op <= 0x4F) {
        in->is_reg = 1;
        in->rm = op & 7;
        in->sz = (uint8_t)osz;
        in->aop = op >= 0x48;
        in->fn = x_incdec;
    } else if (op >= 0x50 && op <= 0x57) {
        in->reg = op & 7;
        in->sz = (uint8_t)osz;
        in->fn = x_push_r;
    } else if (op >= 0x58 && op <= 0x5F) {
        in->reg = op & 7;
        in->sz = (uint8_t)osz;
        in->fn = x_pop_r;
    } else if (op == 0x68 || op == 0x6A) {
        in->sz = (uint8_t)osz;
        in->imm = op == 0x68 ? rdn(r, osz) : (uint32_t)(int32_t)(int8_t)rd8(r);
        in->fn = x_push_i;
    } else if (op == 0x69 || op == 0x6B) {
        in->sz = (uint8_t)osz;
        dec_modrm(r, in, rd8(r));
        in->imm = op == 0x6B ? (uint32_t)(int32_t)(int8_t)rd8(r)
                             : (osz == 2 ? (uint32_t)(int32_t)(int16_t)rdn(r, 2) : rdn(r, 4));
        in->aop = 1;
        in->fn = x_imul;
    } else if (op >= 0x70 && op <= 0x7F) {
        int8_t rel = (int8_t)rd8(r);
        in->aop = op & 15;
        in->imm = page_va + r->pos + (uint32_t)(int32_t)rel;
        in->fn = x_jcc;
    } else if (op == 0x80 || op == 0x81 || op == 0x83) {
        in->sz = (uint8_t)(op == 0x80 ? 1 : osz);
        dec_modrm(r, in, rd8(r));
        in->aop = in->reg;
        in->imm = op == 0x81 ? rdn(r, osz) : (op == 0x83 ? (uint32_t)(int32_t)(int8_t)rd8(r) : rd8(r));
        in->fn = x_alu_ei;
    } else if (op == 0x84 || op == 0x85) {
        in->sz = (uint8_t)(op == 0x84 ? 1 : osz);
        dec_modrm(r, in, rd8(r));
        in->fn = x_test_eg;
    } else if (op == 0x86 || op == 0x87) {
        in->sz = (uint8_t)(op == 0x86 ? 1 : osz);
        dec_modrm(r, in, rd8(r));
        in->fn = x_xchg;
    } else if (op >= 0x88 && op <= 0x8B) {
        in->sz = (uint8_t)((op & 1) ? osz : 1);
        dec_modrm(r, in, rd8(r));
        in->fn = op <= 0x89 ? x_mov_eg : x_mov_ge;
    } else if (op == 0x8D) {
        in->sz = (uint8_t)osz;
        dec_modrm(r, in, rd8(r));
        if (in->is_reg) {
            return false;
        }
        in->fs = 0; /* lea no suma el segmento */
        in->fn = x_lea;
    } else if (op == 0x90) {
        in->fn = x_nop;
    } else if (op == 0x99) {
        in->sz = (uint8_t)osz;
        in->fn = x_cdq;
    } else if (op >= 0xA0 && op <= 0xA3) {
        in->sz = (uint8_t)((op & 1) ? osz : 1);
        in->disp = rdn(r, 4);
        in->reg = R_EAX;
        in->fn = op < 0xA2 ? x_mov_ge : x_mov_eg;
    } else if (op == 0xA8 || op == 0xA9) {
        in->sz = (uint8_t)(op == 0xA8 ? 1 : osz);
        in->is_reg = 1;
        in->rm = R_EAX;
        in->imm = rdn(r, in->sz);
        in->fn = x_test_ei;
    } else if (op >= 0xB0 && op <= 0xBF) {
        in->sz = (uint8_t)(op < 0xB8 ? 1 : osz);
        in->is_reg = 1;
        in->rm = op & 7;
        in->imm = rdn(r, in->sz);
        in->fn = x_mov_ei;
    } else if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) {
        in->sz = (uint8_t)((op & 1) ? osz : 1);
        dec_modrm(r, in, rd8(r));
        in->aop = in->reg;
        in->imm = op <= 0xC1 ? rd8(r) : (op <= 0xD1 ? 1u : 0xFFu);
        if (op <= 0xC1 && in->imm == 0xFF) {
            return false; /* la cuenta 0xFF se confundiría con CL */
        }
        in->fn = x_shift;
    } else if (op == 0xC2 || op == 0xC3) {
        if (o16) {
            return false;
        }
        in->imm = op == 0xC2 ? rdn(r, 2) : 0;
        in->fn = x_ret;
    } else if (op == 0xC6 || op == 0xC7) {
        in->sz = (uint8_t)(op == 0xC6 ? 1 : osz);
        dec_modrm(r, in, rd8(r));
        if (in->reg != 0) {
            return false;
        }
        in->imm = rdn(r, in->sz);
        in->fn = x_mov_ei;
    } else if (op == 0xC9) {
        if (o16) {
            return false;
        }
        in->fn = x_leave;
    } else if (op == 0xE8 || op == 0xE9 || op == 0xEB) {
        if (o16) {
            return false;
        }
        int32_t rel = op == 0xEB ? (int8_t)rd8(r) : (int32_t)rdn(r, 4);
        in->imm = page_va + r->pos + (uint32_t)rel;
        in->fn = op == 0xE8 ? x_call : x_jmp;
    } else if (op == 0xF6 || op == 0xF7) {
        in->sz = (uint8_t)(op == 0xF6 ? 1 : osz);
        dec_modrm(r, in, rd8(r));
        if (in->reg >= 2) {
            in->fn = x_interp; /* not, neg, mul, imul, div, idiv */
        } else {
            in->imm = rdn(r, in->sz);
            in->fn = x_test_ei;
        }
    } else if (op == 0xFE || op == 0xFF) {
        in->sz = (uint8_t)(op == 0xFE ? 1 : osz);
        dec_modrm(r, in, rd8(r));
        int sub = in->reg;
        if (sub <= 1) {
            in->aop = (uint8_t)sub;
            in->fn = x_incdec;
        } else if (op == 0xFF && sub == 2 && !o16) {
            in->fn = x_call_e;
        } else if (op == 0xFF && sub == 4 && !o16) {
            in->fn = x_jmp_e;
        } else if (op == 0xFF && sub == 6) {
            in->fn = x_push_e;
        } else {
            return false;
        }
    } else if (op == 0x0F) {
        uint8_t op2 = rd8(r);
        if (op2 >= 0x80 && op2 <= 0x8F) {
            if (o16) {
                return false;
            }
            int32_t rel = (int32_t)rdn(r, 4);
            in->aop = op2 & 15;
            in->imm = page_va + r->pos + (uint32_t)rel;
            in->fn = x_jcc;
        } else if (op2 >= 0x90 && op2 <= 0x9F) {
            dec_modrm(r, in, rd8(r));
            in->aop = op2 & 15;
            in->fn = x_setcc;
        } else if (op2 >= 0x40 && op2 <= 0x4F) {
            in->sz = (uint8_t)osz;
            dec_modrm(r, in, rd8(r));
            in->aop = op2 & 15;
            in->fn = x_cmov;
        } else if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
            in->sz = (uint8_t)osz;
            dec_modrm(r, in, rd8(r));
            in->aop = (op2 == 0xB6 || op2 == 0xBE) ? 1 : 2;
            in->imm = op2 >= 0xBE;
            in->fn = x_movx;
        } else if (op2 == 0xAF) {
            in->sz = (uint8_t)osz;
            dec_modrm(r, in, rd8(r));
            in->aop = 0;
            in->fn = x_imul;
        } else if (!o16 && mmx_fn(op2)) {
            in->aop = op2;
            dec_modrm(r, in, rd8(r));
            in->fn = mmx_fn(op2);
        } else if (!o16 && op2 >= 0x71 && op2 <= 0x73) {
            dec_modrm(r, in, rd8(r));
            in->imm = rd8(r);
            in->sz = (uint8_t)(op2 == 0x71 ? 2 : op2 == 0x72 ? 4 : 8);
            bool ok = in->is_reg && (in->reg == 2 || in->reg == 4 || in->reg == 6) && !(op2 == 0x73 && in->reg == 4);
            in->fn = ok ? x_mmx_shift_i : x_interp;
        } else if (!o16 && op2 == 0x70) {
            dec_modrm(r, in, rd8(r));
            in->imm = rd8(r);
            in->fn = x_pshufw;
        } else if (!o16 && op2 == 0x77) {
            in->fn = x_emms;
        } else if (!dec_interp_0f(r, in, op2, pfx)) {
            return false;
        }
    } else if (!dec_interp_1(r, in, op, pfx)) {
        return false;
    }
    if (r->bad) {
        return false;
    }
    in->next = page_va + r->pos;
    return true;
}

static bool ends_block(InsnFn f)
{
    return f == x_jcc || f == x_jmp || f == x_call || f == x_ret || f == x_call_e || f == x_jmp_e;
}

static uint64_t g_decodes; /* bloques decodificados (para la pantalla) */
static uint64_t g_compile_ticks; /* tiempo traduciendo al JIT */

static inline uint32_t nb_end_off(const Insn *in, unsigned n)
{
    uint32_t e = in[n - 1].next & 0xFFFu;
    return e ? e : 0x1000u; /* la última termina justo al final de la página */
}

/* ¿Un bucle de espera? Salta a su propio principio y antes solo lee
 * registros o memoria y compara: si los registros no cambian de una vuelta
 * a otra, solo saldrá cuando otro (un ISR, el hardware, otro hilo) cambie
 * la memoria. */
static bool is_spin(const Block *b)
{
    if (b->n < 2 || b->n > 16) {
        return false;
    }
    const Insn *last = &b->in[b->n - 1];
    if (!((last->fn == x_jcc || last->fn == x_jmp) && last->imm == b->eip)) {
        return false;
    }
    for (unsigned i = 0; i + 1 < b->n; i++) {
        const Insn *in = &b->in[i];
        InsnFn f = in->fn;
        bool ok = f == x_nop || f == x_lea || f == x_test_eg || f == x_test_ei || f == x_mov_ge || f == x_movx ||
                  f == x_alu_ge || (f == x_alu_ei && (in->is_reg || in->aop == OP_CMP)) ||
                  ((f == x_mov_eg || f == x_mov_ei) && in->is_reg) || (f == x_alu_eg && (in->is_reg || in->aop == OP_CMP));
        if (!ok || (in->aop == OP_ADC || in->aop == OP_SBB)) {
            return false;
        }
    }
    return true;
}

/* El bloque que empieza en eip (decodificado ahora si hace falta). */
static Block *get_block(Cpu *c, uint32_t eip)
{
    BlockSlot *tab = c->blocks;
    if (!tab) {
        tab = c->blocks = calloc(BLK_HASH, sizeof(BlockSlot));
        if (!tab) {
            return NULL;
        }
    }
    uint32_t e = c->vm->pt[eip >> 12];
    if (!e) {
        return NULL;
    }
    uint32_t pp = e - 1;
    BlockSlot *s = &tab[(eip ^ (eip >> 16)) & (BLK_HASH - 1)];
    Block *b = s->b;
    if (b && s->eip == eip && b->ppage == pp && b->gen == c->vm->gen[pp] && c->vm->code[pp]) {
        return b;
    }
    /* decodificar */
    Insn tmp[BLK_MAX];
    Rd r = {c->vm->ram + ((size_t)pp << 12), eip & 0xFFFu, false};
    uint32_t page_va = eip & ~0xFFFu;
    unsigned n = 0;
    while (n < BLK_MAX) {
        Rd save = r;
        if (!decode(&r, page_va, &tmp[n])) {
            r = save;
            break;
        }
        n++;
        if (ends_block(tmp[n - 1].fn)) {
            break;
        }
    }
    /* n == 0: la primera ya no tiene función (queda guardado: el intérprete) */
    Block *nb = realloc(b, sizeof(Block) + n * sizeof(Insn));
    if (!nb) {
        return NULL;
    }
    nb->eip = eip;
    nb->ppage = pp;
    if (!c->vm->code[pp]) {
        c->vm->code[pp] = 1;
    }
    nb->gen = c->vm->gen[pp];
    nb->n = n;
    if (n) {
        vm_mark_code(c->vm, pp, eip & 0xFFFu, nb_end_off(tmp, n));
    }
    g_decodes++;
    nb->jit = NULL;
    nb->jit_chain = NULL;
    nb->jcnt = 0;
    nb->jit_epoch = 0;
    nb->hits = 0;
    nb->execs = 0;
    nb->slow = ~0ull;
    memcpy(nb->in, tmp, n * sizeof(Insn));
    nb->spin = is_spin(nb);
    s->eip = eip;
    s->b = nb;
    return nb;
}

#if defined(__aarch64__)
#define HAVE_JIT 1
#include "cpu_jit.inc"
#endif

void cpu_jit_stats(CpuJitStats *s)
{
    memset(s, 0, sizeof(*s));
#ifdef HAVE_JIT
    s->on = g_jit.ok;
    s->compiled = g_jit.compiled;
    s->used = g_jit.used;
    s->size = g_jit.size;
    s->flushes = g_jit.epoch ? g_jit.epoch - 1 : 0;
#endif
    s->decodes = g_decodes;
    s->compile_ticks = g_compile_ticks;
#if defined(__SWITCH__)
    s->ticks_per_s = 19200000ull; /* el reloj del sistema de la Switch */
#elif defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    s->ticks_per_s = f;
#else
    s->ticks_per_s = 1000000000ull; /* aproximado */
#endif
}

/* Qué instrucciones gastan más fuera del código nativo: las que el JIT
 * deja a su función de C (o que corren sin traducir), pesadas por las veces
 * que corrió su bloque. Clave: el byte del opcode (0F xx: 256 + xx). */
void cpu_profile(Cpu *c, CpuProfile *p)
{
    static uint64_t w[512];
    memset(w, 0, sizeof(w));
    memset(p, 0, sizeof(*p));
    BlockSlot *tab = c->blocks;
    if (!tab) {
        return;
    }
    for (unsigned i = 0; i < BLK_HASH; i++) {
        const Block *b = tab[i].b;
        if (!b) {
            continue;
        }
        uint64_t ex = b->execs;
#ifdef HAVE_JIT
        if (b->jit && b->jit_epoch == g_jit.epoch && b->jcnt) {
            ex += g_jcnt[b->jcnt];
        }
#endif
        if (!ex) {
            continue;
        }
        p->insns += ex * b->n;
        p->blocks += ex;
        for (unsigned k = 0; k < b->n; k++) {
            if (!((b->slow >> (k & 63)) & 1)) {
                continue;
            }
            p->slow += ex;
            uint8_t by[4] = {0};
            uint32_t a = b->in[k].start;
            vm_read(c->vm, a, by, 1);
            for (int j = 0; j < 4 && (by[0] == 0x66 || by[0] == 0xF2 || by[0] == 0xF3 || by[0] == 0xF0 ||
                                       by[0] == 0x2E || by[0] == 0x3E || by[0] == 0x26 || by[0] == 0x36 ||
                                       by[0] == 0x64 || by[0] == 0x65);
                 j++) {
                vm_read(c->vm, ++a, by, 1);
            }
            unsigned key = by[0];
            if (by[0] == 0x0F) {
                vm_read(c->vm, a + 1, by + 1, 1);
                key = 256 + by[1];
            }
            w[key] += ex;
        }
    }
    for (int t = 0; t < CPU_PROF_TOP; t++) {
        unsigned best = 0;
        for (unsigned k = 1; k < 512; k++) {
            if (w[k] > w[best]) {
                best = k;
            }
        }
        if (!w[best]) {
            break;
        }
        p->op[t] = (uint16_t)(best >= 256 ? 0x0F00 | (best - 256) : best);
        p->count[t] = w[best];
        w[best] = 0;
        p->n++;
    }
}

void cpu_jit_range(uintptr_t *rx, size_t *size)
{
    *rx = 0;
    *size = 0;
#ifdef HAVE_JIT
    if (g_jit.ok) {
        *rx = (uintptr_t)g_jit.rx;
        *size = g_jit.size;
    }
#endif
}

/* Hasta max instrucciones por bloques; las demás, una a una en step(). */
/* El reloj de la CPU del host (para medir dónde se va el tiempo). */
static inline uint64_t cpu_tick(void)
{
#if defined(__SWITCH__)
    uint64_t v; /* en Horizon solo cntpct_el0 se puede leer (como armGetSystemTick) */
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#elif defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}


static const volatile uint8_t g_no_yield;

static CpuStop run_blocks(Cpu *c, uint64_t max)
{
    uint64_t done = 0;
    if (!c->yield) {
        c->yield = &g_no_yield; /* el JIT lo lee sin mirar si es NULL */
    }
    uint64_t t_iter = 0; /* principio de la vuelta medida (0: esta no se mide) */
    c->st_runs++;
    while (done < max) {
        if (t_iter) {
            c->st_t_all += cpu_tick() - t_iter;
            t_iter = 0;
        }
        bool smp = (++c->st_disp & 63) == 0;
        if (smp) {
            t_iter = cpu_tick();
        }
        if (c->yield && *c->yield) {
            return CPU_LIMIT; /* el hardware pidió una interrupción */
        }
        Block *b = get_block(c, c->eip);
        if (smp) {
            c->st_t_get += cpu_tick() - t_iter;
        }
        if (!b || !b->n) {
            uint64_t t0 = smp ? cpu_tick() : 0;
            if (step(c) != CPU_RUNNING) {
                return c->stop;
            }
            if (smp) {
                c->st_t_step += cpu_tick() - t0;
            }
            c->st_steps++;
            done++;
            continue;
        }
        c->trace[c->trace_pos++ % CPU_TRACE] = b->eip;
        if (b->spin) {
            /* la misma vuelta con los mismos registros: espera algo de fuera */
            if (c->spin_eip == b->eip && !memcmp(c->spin_r, c->r, sizeof(c->r))) {
                if (++c->spin_n >= 8) {
                    c->spin_n = 0;
                    c->spin_eip = 0;
                    c->idle = true;
                    return CPU_LIMIT;
                }
            } else {
                c->spin_eip = b->eip;
                c->spin_n = 0;
                memcpy(c->spin_r, c->r, sizeof(c->r));
            }
        }
#ifdef HAVE_JIT
        if (c->jit_hot) {
            bool have = b->jit && b->jit_epoch == g_jit.epoch;
            if (!have && ++b->hits >= c->jit_hot) {
                uint64_t t0 = cpu_tick();
                have = jit_compile(c, b);
                g_compile_ticks += cpu_tick() - t0;
            }
            if (have && done + b->n <= max) {
                if (!b->spin) { /* desde otros bloques traducidos se salta aquí directo */
                    JSlot *js = &g_jtab[(b->eip ^ (b->eip >> 16)) & (JTAB - 1)];
                    js->eip = b->eip;
                    js->entry = b->jit_chain;
                }
                uint64_t t0 = smp ? cpu_tick() : 0;
                /* corre este y los que encadene, hasta max - done instrucciones */
                unsigned k = ((JitFn)b->jit)(c, max - done);
                if (smp) {
                    c->st_t_jit += cpu_tick() - t0;
                }
                c->st_jit++;
                c->icount += k;
                done += k;
                if (c->stop != CPU_RUNNING) {
                    uint32_t at = c->jit_stop_eip;
                    c->stop_eip = at;
                    vm_read(c->vm, at, c->stop_bytes, sizeof(c->stop_bytes));
                    if (c->stop == CPU_FAULT || c->stop == CPU_UNIMPL || c->stop == CPU_DIVIDE) {
                        c->eip = at;
                    }
                    return c->stop;
                }
                continue; /* la siguiente sin función: la vuelta siguiente la ve (bloque vacío) */
            }
        }
#endif
        b->execs++;
        uint64_t ti = smp ? cpu_tick() : 0;
        unsigned i = 0;
        for (; i < b->n && done < max; i++) {
            const Insn *in = &b->in[i];
            c->eip = in->next;
            in->fn(c, in);
            if (c->stop != CPU_RUNNING) {
                c->stop_eip = in->start;
                vm_read(c->vm, in->start, c->stop_bytes, sizeof(c->stop_bytes));
                if (c->stop == CPU_FAULT || c->stop == CPU_UNIMPL || c->stop == CPU_DIVIDE) {
                    c->eip = in->start;
                }
                return c->stop;
            }
            c->icount++;
            done++;
            if (c->eip != in->next) {
                break; /* salto tomado */
            }
            if (b->gen != c->vm->gen[b->ppage]) {
                break; /* se escribió en el código de este bloque */
            }
        }
        if (smp) {
            c->st_t_int += cpu_tick() - ti;
        }
        if (i == b->n && done < max && !ends_block(b->in[b->n - 1].fn) && c->eip == b->in[b->n - 1].next) {
            /* la siguiente no tenía función: el intérprete */
            uint64_t t1 = smp ? cpu_tick() : 0;
            if (step(c) != CPU_RUNNING) {
                return c->stop;
            }
            if (smp) {
                c->st_t_step += cpu_tick() - t1;
            }
            c->st_steps++;
            done++;
        }
    }
    if (t_iter) {
        c->st_t_all += cpu_tick() - t_iter;
    }
    return CPU_LIMIT;
}

CpuStop cpu_step(Cpu *c)
{
    CpuStop st = step(c);
    flags_sync(c);
    return st;
}

CpuStop cpu_run(Cpu *c, uint64_t max)
{
    c->fetch_host = NULL; /* el kernel pudo cambiar las páginas */
    if (c->use_blocks) {
        CpuStop st = run_blocks(c, max);
        flags_sync(c);
        return st;
    }
    for (uint64_t i = 0; i < max; i++) {
        if (step(c) != CPU_RUNNING) {
            flags_sync(c);
            return c->stop;
        }
    }
    flags_sync(c);
    return CPU_LIMIT;
}

void cpu_free(Cpu *c)
{
#ifdef HAVE_JIT
    jit_table_clear();
#endif
    BlockSlot *tab = c->blocks;
    if (tab) {
        for (unsigned i = 0; i < BLK_HASH; i++) {
            free(tab[i].b);
        }
        free(tab);
    }
    c->blocks = NULL;
}

void cpu_init(Cpu *c, Vm *vm)
{
#ifdef HAVE_JIT
    jit_table_clear();
#endif
    memset(c, 0, sizeof(*c));
    c->vm = vm;
    c->flags = 0x2u | F_IF;
    c->fpu_cw = 0x027F;
    c->mxcsr = 0x1F80;
    c->fp.empty = 0xFF;
}

const char *cpu_stop_text(CpuStop s)
{
    switch (s) {
    case CPU_RUNNING: return "corriendo";
    case CPU_KCALL: return "llamada al kernel";
    case CPU_HLT: return "hlt";
    case CPU_BREAK: return "int3 (punto de parada)";
    case CPU_UNIMPL: return "instrucción que falta";
    case CPU_FAULT: return "acceso a memoria sin mapear";
    case CPU_DIVIDE: return "división por cero";
    case CPU_LIMIT: return "límite de instrucciones";
    }
    return "?";
}
