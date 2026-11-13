/*
 * XboxNX — etapa 4c: los vertex programs del NV2A a GLSL.
 *
 * Mismo resultado que el emulador de la CPU (vsh/nv2a_vsh_emulator.c y
 * vsh/nv2a_vsh_cpu.c): en cada paso se leen primero las entradas del MAC y
 * del ILU, luego se escribe el MAC y después el ILU; los registros r12
 * (lee oPos) y r13+ (cero), las lecturas de c[] fuera de rango (cero), A0
 * con floor(x + 0.001), el oFog con su máscara especial, oD0 en blanco si
 * el programa no lo escribe, y las funciones RCP/RCC/RSQ/EXP/LOG/LIT y las
 * multiplicaciones que se pasan a infinito como en el emulador.
 *
 * Un programa que escribe en c[] no se traduce (lo hace la CPU).
 */
#include "vsh_glsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct Out {
    char *p;
    size_t n, at;
    bool full;
} Out;

static void put(Out *o, const char *fmt, ...)
{
    if (o->full) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->p + o->at, o->n - o->at, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= o->n - o->at) {
        o->full = true;
        return;
    }
    o->at += (size_t)w;
}

static const char k_head[] =
    "#version 330 core\n"
    "uniform vec4 c[192];\n"
    "uniform vec4 u_vp;\n"
    "out vec4 v_c0; out vec4 v_c1; out vec4 v_t0; out vec4 v_t1; out vec4 v_t2; out vec4 v_t3;\n"
    "const float FMAX = 3.40282347e38;\n"
    "vec4 crel(int k) { return (k >= 0 && k < 192) ? c[k] : vec4(0.0); }\n"
    /* fix_inf_mult: si a*b da infinito sin que a o b lo sean, el máximo con su signo */
    "vec4 mulx(vec4 a, vec4 b) {\n"
    "  vec4 r = a * b;\n"
    "  vec4 f = vec4(isinf(r)) * (1.0 - vec4(isinf(a))) * (1.0 - vec4(isinf(b)));\n"
    "  return mix(r, sign(r) * FMAX, f);\n"
    "}\n"
    "float fixinf(float x) { return isinf(x) ? sign(x) * FMAX : x; }\n"
    "vec4 dp3(vec4 a, vec4 b) { vec4 m = mulx(a, b); return vec4(fixinf(m.x + m.y + m.z)); }\n"
    "vec4 dph(vec4 a, vec4 b) { vec4 m = mulx(a, b); return vec4(fixinf(m.x + m.y + m.z + b.w)); }\n"
    "vec4 dp4(vec4 a, vec4 b) { vec4 m = mulx(a, b); return vec4(fixinf(m.x + m.y + m.z + m.w)); }\n"
    "vec4 dst(vec4 a, vec4 b) { return vec4(1.0, mulx(a, b).y, a.z, b.w); }\n"
    "vec4 vmin(vec4 a, vec4 b) { return mix(b, a, vec4(lessThan(a, b))); }\n"
    "vec4 vmax(vec4 a, vec4 b) { return mix(b, a, vec4(greaterThan(a, b))); }\n"
    "vec4 slt(vec4 a, vec4 b) {\n"
    "  vec4 l = vec4(lessThan(a, b));\n"
    "  vec4 z = vec4(equal(floatBitsToUint(a), uvec4(0x80000000u))) * vec4(equal(floatBitsToUint(b), uvec4(0u)));\n"
    "  return min(l + z, vec4(1.0));\n"
    "}\n"
    "vec4 sge(vec4 a, vec4 b) { return vec4(1.0) - slt(a, b); }\n"
    "vec4 arl(vec4 a) { return vec4(floor(a.x + 0.001)); }\n"
    "vec4 rcp(vec4 a) {\n"
    "  float x = a.x, r;\n"
    "  if (x == 1.0) r = 1.0;\n"
    "  else if (x == 0.0) r = (floatBitsToUint(x) & 0x80000000u) != 0u ? uintBitsToFloat(0xFF800000u) : uintBitsToFloat(0x7F800000u);\n"
    "  else r = 1.0 / x;\n"
    "  return vec4(r);\n"
    "}\n"
    "vec4 rcc(vec4 a) {\n"
    "  float x = a.x, r;\n"
    "  if (x == 1.0) r = 1.0;\n"
    "  else {\n"
    "    r = 1.0 / x;\n"
    "    if (r > 0.0) { if (r < 5.42101e-020) r = 5.42101e-020; else if (r > 1.884467e+019) r = uintBitsToFloat(0x5F800000u); }\n"
    "    else { if (r < -1.884467e+019) r = uintBitsToFloat(0xDF800000u); else if (r > -5.42101e-020) r = -5.42101e-020; }\n"
    "  }\n"
    "  return vec4(r);\n"
    "}\n"
    "vec4 rsq(vec4 a) {\n"
    "  float x = abs(a.x), r;\n"
    "  if (x == 1.0) r = 1.0; else if (x == 0.0) r = uintBitsToFloat(0x7F800000u); else r = 1.0 / sqrt(x);\n"
    "  return vec4(r);\n"
    "}\n"
    "vec4 vexp(vec4 a) { float t = floor(a.x); return vec4(exp2(t), a.x - t, exp2(a.x), 1.0); }\n"
    "vec4 vlog(vec4 a) {\n"
    "  float t = abs(a.x);\n"
    "  if (t == 0.0) return vec4(uintBitsToFloat(0xFF800000u), 1.0, uintBitsToFloat(0xFF800000u), 1.0);\n"
    "  if (isinf(t)) return vec4(uintBitsToFloat(0x7F800000u), 1.0, uintBitsToFloat(0x7F800000u), 1.0);\n"
    "  uint u = floatBitsToUint(t);\n"
    "  float e = float(int((u >> 23) & 0xFFu) - 127);\n"
    "  return vec4(e, uintBitsToFloat((u & 0x7FFFFFu) | 0x3F800000u), log2(t), 1.0);\n"
    "}\n"
    "vec4 lit(vec4 a) {\n"
    "  vec4 r = vec4(1.0, 0.0, 0.0, 1.0);\n"
    "  float p = clamp(a.w, -127.9961, 127.9961);\n"
    "  if (a.x > 0.0) { r.y = a.x; if (a.y > 0.0) r.z = pow(a.y, p); }\n"
    "  return r;\n"
    "}\n";

static const char k_sw[4] = {'x', 'y', 'z', 'w'};

/* el valor de una entrada como expresión GLSL */
static bool src(Out *o, const Nv2aVshInput *in, uint16_t *mask)
{
    char base[48];
    switch (in->type) {
    case NV2ART_TEMPORARY:
        if (in->index == 12) {
            snprintf(base, sizeof(base), "o0");
        } else if (in->index > 12) {
            snprintf(base, sizeof(base), "vec4(0.0)");
        } else {
            snprintf(base, sizeof(base), "r%u", (unsigned)in->index);
        }
        break;
    case NV2ART_INPUT:
        if (in->index >= 16) {
            return false;
        }
        snprintf(base, sizeof(base), "v%u", (unsigned)in->index);
        *mask |= (uint16_t)(1u << in->index);
        break;
    case NV2ART_CONTEXT:
        if (in->is_relative) {
            snprintf(base, sizeof(base), "crel(%d + int(a0.x))", (int)in->index);
        } else if (in->index < 192) {
            snprintf(base, sizeof(base), "c[%u]", (unsigned)in->index);
        } else {
            snprintf(base, sizeof(base), "vec4(0.0)");
        }
        break;
    default:
        return false;
    }
    put(o, "%s(%s).%c%c%c%c", in->is_negated ? "-" : "", base, k_sw[in->swizzle[0] & 3], k_sw[in->swizzle[1] & 3],
        k_sw[in->swizzle[2] & 3], k_sw[in->swizzle[3] & 3]);
    return true;
}

/* la operación sobre a, b, cc (nombres ya calculados) */
static bool op_expr(Out *o, Nv2aVshOpcode op, const char *a, const char *b, const char *cc)
{
    switch (op) {
    case NV2AOP_MOV: put(o, "%s", a); break;
    case NV2AOP_MUL: put(o, "mulx(%s, %s)", a, b); break;
    case NV2AOP_ADD: put(o, "(%s + %s)", a, b); break;
    case NV2AOP_MAD: put(o, "(mulx(%s, %s) + %s)", a, b, cc); break;
    case NV2AOP_DP3: put(o, "dp3(%s, %s)", a, b); break;
    case NV2AOP_DPH: put(o, "dph(%s, %s)", a, b); break;
    case NV2AOP_DP4: put(o, "dp4(%s, %s)", a, b); break;
    case NV2AOP_DST: put(o, "dst(%s, %s)", a, b); break;
    case NV2AOP_MIN: put(o, "vmin(%s, %s)", a, b); break;
    case NV2AOP_MAX: put(o, "vmax(%s, %s)", a, b); break;
    case NV2AOP_SLT: put(o, "slt(%s, %s)", a, b); break;
    case NV2AOP_SGE: put(o, "sge(%s, %s)", a, b); break;
    case NV2AOP_ARL: put(o, "arl(%s)", a); break;
    case NV2AOP_RCP: put(o, "rcp(%s)", a); break;
    case NV2AOP_RCC: put(o, "rcc(%s)", a); break;
    case NV2AOP_RSQ: put(o, "rsq(%s)", a); break;
    case NV2AOP_EXP: put(o, "vexp(%s)", a); break;
    case NV2AOP_LOG: put(o, "vlog(%s)", a); break;
    case NV2AOP_LIT: put(o, "lit(%s)", a); break;
    default: return false;
    }
    return true;
}

/* escribir el resultado res en las salidas de la operación */
static bool writes(Out *o, const Nv2aVshOperation *op, const char *res, bool *d0x)
{
    for (int k = 0; k < 2; k++) {
        const Nv2aVshOutput *w = &op->outputs[k];
        char reg[16];
        switch (w->type) {
        case NV2ART_NONE:
            continue;
        case NV2ART_OUTPUT:
            if (w->index >= 13) {
                continue;
            }
            snprintf(reg, sizeof(reg), "o%u", (unsigned)w->index);
            break;
        case NV2ART_TEMPORARY:
            if (w->index >= 12) {
                continue;
            }
            snprintf(reg, sizeof(reg), "r%u", (unsigned)w->index);
            break;
        case NV2ART_ADDRESS:
            snprintf(reg, sizeof(reg), "a0");
            break;
        default: /* c[]: lo hace la CPU */
            return false;
        }
        unsigned m = (unsigned)w->writemask;
        char sw[5];
        int n = 0;
        if (m & NV2AWM_X) sw[n++] = 'x';
        if (m & NV2AWM_Y) sw[n++] = 'y';
        if (m & NV2AWM_Z) sw[n++] = 'z';
        if (m & NV2AWM_W) sw[n++] = 'w';
        sw[n] = 0;
        if (n) {
            put(o, "  %s.%s = %s.%s;\n", reg, sw, res, sw);
        }
        if (w->type == NV2ART_OUTPUT && w->index == 3 && (m & NV2AWM_X)) {
            *d0x = true;
        }
        if (w->type == NV2ART_OUTPUT && w->index == NV2AOR_FOG_COORD && !(m & NV2AWM_X)) {
            if (m & NV2AWM_Y) {
                put(o, "  o5.x = %s.y;\n", res);
            } else if (m & NV2AWM_Z) {
                put(o, "  o5.x = %s.z;\n", res);
            } else if (m & NV2AWM_W) {
                put(o, "  o5.x = %s.w;\n", res);
            }
        }
    }
    return true;
}

bool vsh_glsl(const Nv2aVshProgram *p, char *buf, size_t n, uint16_t *in_mask)
{
    Out o = {buf, n, 0, false};
    uint16_t mask = 0;
    bool d0x = false;
    if (!p || !p->steps) {
        return false;
    }
    /* primero el cuerpo (para saber qué entradas usa), luego la cabecera */
    static char body[96 * 1024];
    Out b = {body, sizeof(body), 0, false};
    put(&b, "  vec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0), r3 = vec4(0.0), r4 = vec4(0.0), r5 = vec4(0.0);\n"
            "  vec4 r6 = vec4(0.0), r7 = vec4(0.0), r8 = vec4(0.0), r9 = vec4(0.0), r10 = vec4(0.0), r11 = vec4(0.0);\n"
            "  vec4 a0 = vec4(0.0);\n");
    for (int i = 0; i < 13; i++) {
        put(&b, "  vec4 o%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
    }
    for (unsigned si = 0; si < 256; si++) {
        const Nv2aVshStep *st = &p->steps[si];
        put(&b, "  {\n");
        const Nv2aVshOperation *mac = &st->mac, *ilu = &st->ilu;
        if (mac->opcode) {
            for (int k = 0; k < 3; k++) {
                if (mac->inputs[k].type == NV2ART_NONE) {
                    break;
                }
                put(&b, "  vec4 m%d = ", k);
                if (!src(&b, &mac->inputs[k], &mask)) {
                    return false;
                }
                put(&b, ";\n");
            }
        }
        if (ilu->opcode) {
            put(&b, "  vec4 i0 = ");
            if (!src(&b, &ilu->inputs[0], &mask)) {
                return false;
            }
            put(&b, ";\n");
        }
        if (mac->opcode) {
            put(&b, "  vec4 mr = ");
            if (!op_expr(&b, mac->opcode, "m0", "m1", "m2")) {
                return false;
            }
            put(&b, ";\n");
            if (!writes(&b, mac, "mr", &d0x)) {
                return false;
            }
        }
        if (ilu->opcode) {
            put(&b, "  vec4 ir = ");
            if (!op_expr(&b, ilu->opcode, "i0", "i0", "i0")) {
                return false;
            }
            put(&b, ";\n");
            if (!writes(&b, ilu, "ir", &d0x)) {
                return false;
            }
        }
        put(&b, "  }\n");
        if (st->is_final) {
            break;
        }
        if (si == 255) {
            return false;
        }
    }
    if (b.full) {
        return false;
    }
    put(&o, "%s", k_head);
    for (int i = 0; i < 16; i++) {
        if (mask & (1u << i)) {
            put(&o, "layout(location=%d) in vec4 v%d;\n", i, i);
        }
    }
    put(&o, "void main() {\n%s", body);
    /* las salidas como en transform() y la posición como en el shader de siempre */
    put(&o, "  v_c0 = %s; v_c1 = o4; v_t0 = o9; v_t1 = o10; v_t2 = o11; v_t3 = o12;\n", d0x ? "o3" : "vec4(1.0)");
    put(&o, "  float w = o0.w;\n"
            "  if (!(abs(w) > 1e-20) || isinf(w)) w = 1.0;\n"
            "  vec3 n = vec3(o0.x * u_vp.x - 1.0, o0.y * u_vp.y - 1.0, o0.z * u_vp.z - 1.0);\n"
            "  gl_Position = vec4(n * w, w);\n"
            "}\n");
    if (o.full) {
        return false;
    }
    *in_mask = mask;
    return true;
}
