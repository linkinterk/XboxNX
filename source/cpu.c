/*
 * XboxNX — intérprete x86 de 32 bits.
 *
 * Implementación propia. Modo plano de 32 bits como en el Xbox: todos los
 * segmentos con base 0 salvo fs (el KPCR). Flags calculados en cada
 * instrucción (simple y exacto; la velocidad llega con FEX).
 */
#include "cpu.h"

#include <stdio.h>
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

static uint32_t mrd(Cpu *c, uint32_t a, int sz)
{
    uint8_t *h = vm_host(c->vm, a);
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
        fault(c, a, false);
        return 0;
    }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void mwr(Cpu *c, uint32_t a, int sz, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    uint8_t *h = vm_host(c->vm, a);
    if (h && (a & (VM_PAGE - 1)) + (uint32_t)sz <= VM_PAGE) {
        memcpy(h, b, (size_t)sz);
        return;
    }
    if (!vm_write(c->vm, a, b, (size_t)sz)) {
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

static uint8_t fetch8(Cpu *c)
{
    uint8_t v = (uint8_t)mrd(c, c->eip, 1);
    c->eip++;
    return v;
}
static uint16_t fetch16(Cpu *c)
{
    uint16_t v = (uint16_t)mrd(c, c->eip, 2);
    c->eip += 2;
    return v;
}
static uint32_t fetch32(Cpu *c)
{
    uint32_t v = mrd(c, c->eip, 4);
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

static void set_flag(Cpu *c, uint32_t f, bool on)
{
    if (on) {
        c->flags |= f;
    } else {
        c->flags &= ~f;
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
    uint32_t m = mask_of(sz), s = sign_of(sz);
    a &= m;
    b &= m;
    uint32_t res, cin;
    switch (op) {
    case OP_ADD:
    case OP_ADC: {
        cin = (op == OP_ADC && (c->flags & F_CF)) ? 1 : 0;
        uint64_t full = (uint64_t)a + b + cin;
        res = (uint32_t)full & m;
        set_flag(c, F_CF, full > m);
        set_flag(c, F_OF, ((a ^ res) & (b ^ res) & s) != 0);
        set_flag(c, F_AF, ((a ^ b ^ res) & 0x10) != 0);
        break;
    }
    case OP_SUB:
    case OP_SBB:
    case OP_CMP: {
        cin = (op == OP_SBB && (c->flags & F_CF)) ? 1 : 0;
        res = (a - b - cin) & m;
        set_flag(c, F_CF, (uint64_t)a < (uint64_t)b + cin);
        set_flag(c, F_OF, ((a ^ b) & (a ^ res) & s) != 0);
        set_flag(c, F_AF, ((a ^ b ^ res) & 0x10) != 0);
        break;
    }
    case OP_OR: res = a | b; goto logic;
    case OP_AND: res = a & b; goto logic;
    default: res = a ^ b;
    logic:
        c->flags &= ~(F_CF | F_OF | F_AF);
        break;
    }
    szp(c, res, sz);
    return res;
}

static bool cond(Cpu *c, int n)
{
    uint32_t f = c->flags;
    bool cf = f & F_CF, zf = f & F_ZF, sf = f & F_SF, of = f & F_OF, pf = f & F_PF;
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
        bool carry = c->flags & F_CF;
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
        bool carry = c->flags & F_CF;
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
    c->flags &= ~F_AF;
    szp(c, res, sz);
    return res;
}

/* ---- instrucciones de cadena ---- */

static void string_op(Cpu *c, const Dec *d, uint8_t op, int sz)
{
    int32_t step = (c->flags & F_DF) ? -sz : sz;
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
        if (cmp && d->rep == 0xF3 && !(c->flags & F_ZF)) {
            break;
        }
        if (cmp && d->rep == 0xF2 && (c->flags & F_ZF)) {
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

/* ---- FPU mínima ---- */

static void fpu(Cpu *c, const Dec *d, uint8_t op)
{
    uint8_t m = fetch8(c);
    int reg = (m >> 3) & 7;
    if (op == 0xDB && m == 0xE3) { /* fninit */
        c->fpu_cw = 0x037F;
        c->fpu_sw = 0;
        return;
    }
    if (op == 0xDB && m == 0xE2) { /* fnclex */
        c->fpu_sw &= 0x7F00;
        return;
    }
    if (op == 0xDF && m == 0xE0) { /* fnstsw ax */
        reg_set(c, R_EAX, 2, c->fpu_sw);
        return;
    }
    if (m < 0xC0) {
        Ea e = modrm_ea(c, d, m);
        if (op == 0xD9 && reg == 7) { /* fnstcw */
            mwr(c, e.addr, 2, c->fpu_cw);
            return;
        }
        if (op == 0xD9 && reg == 5) { /* fldcw */
            c->fpu_cw = (uint16_t)mrd(c, e.addr, 2);
            return;
        }
        if (op == 0xDD && reg == 7) { /* fnstsw */
            mwr(c, e.addr, 2, c->fpu_sw);
            return;
        }
    }
    char b[48];
    snprintf(b, sizeof(b), "FPU x87 (%02X %02X)", op, m);
    unimpl(c, d, b);
}

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
    case 0x31: { /* rdtsc */
        uint64_t t = c->icount;
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
        c->flags &= ~F_AF;
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
        if (c->flags & F_ZF) {
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
            c->flags |= F_ZF;
        } else {
            c->r[R_EAX] = lo;
            c->r[R_EDX] = hi;
            c->flags &= ~F_ZF;
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
            c->flags |= F_ZF;
            return;
        }
        c->flags &= ~F_ZF;
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
            for (uint32_t i = 0; i < 512; i += 4) {
                mwr(c, e.addr + i, 4, 0);
            }
            mwr(c, e.addr, 2, c->fpu_cw);
            mwr(c, e.addr + 2, 2, c->fpu_sw);
            mwr(c, e.addr + 24, 4, c->mxcsr);
        } else if (sub == 1) {
            c->fpu_cw = (uint16_t)mrd(c, e.addr, 2);
            c->mxcsr = mrd(c, e.addr + 24, 4);
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
        snprintf(b, sizeof(b), "0F %02X", op);
        unimpl(c, d, b);
        return;
    }
}

/* ---- una instrucción ---- */

CpuStop cpu_step(Cpu *c)
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
        bool cf = c->flags & F_CF;
        uint32_t res = alu(c, op < 0x48 ? OP_ADD : OP_SUB, reg_get(c, r, osz), 1, osz);
        set_flag(c, F_CF, cf);
        reg_set(c, r, osz, res);
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
        push(c, osz, (c->flags & 0x00244FD5u) | 0x2u);
        break;
    case 0x9D: {
        uint32_t v = pop(c, osz);
        uint32_t keep = osz == 2 ? 0xFFFF0000u : 0;
        c->flags = (c->flags & keep) | (v & 0x00244FD5u & ~keep) | 0x2u;
        break;
    }
    case 0x9E: /* sahf */
        c->flags = (c->flags & ~0xD5u) | ((c->r[R_EAX] >> 8) & 0xD5u);
        break;
    case 0x9F: /* lahf */
        reg_set(c, 4, 1, (c->flags & 0xD5u) | 0x2u);
        break;
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
            go = go && !(c->flags & F_ZF);
        } else if (op == 0xE1) {
            go = go && (c->flags & F_ZF);
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
        c->flags ^= F_CF;
        break;
    case 0xF6:
    case 0xF7:
        group3(c, &d, op == 0xF6 ? 1 : osz, fetch8(c));
        break;
    case 0xF8: c->flags &= ~F_CF; break;
    case 0xF9: c->flags |= F_CF; break;
    case 0xFA: c->flags &= ~F_IF; break;
    case 0xFB: c->flags |= F_IF; break;
    case 0xFC: c->flags &= ~F_DF; break;
    case 0xFD: c->flags |= F_DF; break;
    case 0xFE:
    case 0xFF: {
        int sz = op == 0xFE ? 1 : osz;
        uint8_t m = fetch8(c);
        int sub = (m >> 3) & 7;
        Ea e = modrm_ea(c, &d, m);
        if (sub <= 1) {
            bool cf = c->flags & F_CF;
            uint32_t res = alu(c, sub == 0 ? OP_ADD : OP_SUB, ea_get(c, &e, sz), 1, sz);
            set_flag(c, F_CF, cf);
            ea_set(c, &e, sz, res);
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

CpuStop cpu_run(Cpu *c, uint64_t max)
{
    for (uint64_t i = 0; i < max; i++) {
        if (cpu_step(c) != CPU_RUNNING) {
            return c->stop;
        }
    }
    return CPU_LIMIT;
}

void cpu_init(Cpu *c, Vm *vm)
{
    memset(c, 0, sizeof(*c));
    c->vm = vm;
    c->flags = 0x2u | F_IF;
    c->fpu_cw = 0x027F;
    c->mxcsr = 0x1F80;
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
